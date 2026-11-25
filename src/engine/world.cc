#include "engine/world.h"

#include "core/objects.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/level_events.h"
#include "engine/resources.h"

#include <algorithm>
#include <initializer_list>

namespace dx {
    World* World::current = nullptr;

    namespace {
        EngineClasses classesInstance;
        engine::GameHooks gameHooksInstance;

        // The classes the engine looks actors up by. They belong to packages that stay loaded,
        // which keep them anyway; handing them over says so to the collector as well. The game
        // hooks are plain function pointers.
        struct EngineClassReferences final : gc::Holder {
            void addReferences(ReferenceVisitor& references) override
            {
                EngineClasses& c = classesInstance;
                for (UClass** cls : { &c.actor, &c.pawn, &c.playerPawn, &c.decoration, &c.inventory, &c.weapon, &c.projectile, &c.mover, &c.brush, &c.levelInfo, &c.zoneInfo, &c.skyZoneInfo, &c.warpZoneInfo,
                         &c.navigationPoint, &c.interpolationPoint, &c.carcass, &c.gameInfo, &c.camera, &c.light, &c.viewport, &c.canvas, &c.playerPawnExt, &c.scriptedPawn, &c.deusExPlayer }) {
                    references(*cls);
                }
            }
        };

        EngineClassReferences engineClassReferences;
    }

    EngineClasses& engineClasses()
    {
        return classesInstance;
    }

    engine::GameHooks& engine::gameHooks()
    {
        return gameHooksInstance;
    }

    void bindEngineClasses()
    {
        EngineClasses& c = classesInstance;
        c.actor = objects::findClass("Engine.Actor");
        c.pawn = objects::findClass("Engine.Pawn");
        c.playerPawn = objects::findClass("Engine.PlayerPawn");
        c.decoration = objects::findClass("Engine.Decoration");
        c.inventory = objects::findClass("Engine.Inventory");
        c.weapon = objects::findClass("Engine.Weapon");
        c.projectile = objects::findClass("Engine.Projectile");
        c.mover = objects::findClass("Engine.Mover");
        c.brush = objects::findClass("Engine.Brush");
        c.levelInfo = objects::findClass("Engine.LevelInfo");
        c.zoneInfo = objects::findClass("Engine.ZoneInfo");
        c.skyZoneInfo = objects::findClass("Engine.SkyZoneInfo");
        c.warpZoneInfo = objects::findClass("Engine.WarpZoneInfo");
        c.navigationPoint = objects::findClass("Engine.NavigationPoint");
        c.interpolationPoint = objects::findClass("Engine.InterpolationPoint");
        c.carcass = objects::findClass("Engine.Carcass");
        c.gameInfo = objects::findClass("Engine.GameInfo");
        c.camera = objects::findClass("Engine.Camera");
        c.light = objects::findClass("Engine.Light");
        c.viewport = objects::findClass("Engine.Viewport");
        if (!c.viewport) {
            c.viewport = objects::findClass("Engine.Player");
        }
        c.canvas = objects::findClass("Engine.Canvas");
        c.playerPawnExt = objects::findClass("Extension.PlayerPawnExt");
        c.scriptedPawn = objects::findClass("DeusEx.ScriptedPawn");
        c.deusExPlayer = objects::findClass("DeusEx.DeusExPlayer");
        objects::flushLoads();
    }

    UObject* playerPawnOf(UObject* a)
    {
        if (isPlayerPawn(a) && playerpawn::Player(a)) {
            return a;
        }
        return nullptr;
    }

    World::World()
    {
        current = this;
    }

    World::~World()
    {
        if (levelAnnounced) {
            levelAnnounced = false;
            levelevents::levelUnloading(*this);
        }
        if (current == this) {
            current = nullptr;
        }
    }

    void World::addReferences(ReferenceVisitor& references)
    {
        references(level);
        references(levelInfo);
        references(gameInfo);
        references(localPlayer);
        references(viewport);
        // A slot of `actors` set to None reads as a destroyed actor's slot does until the list
        // is packed. The other two lists are walked without checking for None.
        references(actors);
        references(destroyedActors);
        references(releasingActors);
        references(newlySpawned);
        std::erase(destroyedActors, nullptr);
        std::erase(releasingActors, nullptr);
        std::erase(newlySpawned, nullptr);
        if (actors.size() > 2 && std::find(actors.begin() + 2, actors.end(), nullptr) != actors.end()) {
            pendingCleanup = true;
        }
        addTravelReferences(incomingTravel, references);
        // Weak, and dropped in objectsReleased: the collision hash and decals, which only index
        // actors already in `actors`; the decal registry's model, a cache of the level's own; and
        // the live actor list, a copy of `actors`.
    }

    void World::objectsReleased(const gc::ReleasedSet& released)
    {
        for (const UObject* object : released) {
            UObject* actor = const_cast<UObject*>(object);
            collisionHash.remove(actor);
            decals.pieces.erase(actor);
        }
        // Another model could later be made at the same address with the same revision.
        if (decals.model && released.count(decals.model)) {
            decals.model = nullptr;
            decals.revision = 0;
            decals.surfNodes.clear();
        }
        if (liveActorsCache && std::any_of(liveActorsCache->begin(), liveActorsCache->end(), [&](const UObject* a) { return released.count(a) != 0; })) {
            liveActorsCache.reset();
        }
    }

    UObject* World::player() const
    {
        return localPlayer;
    }

    FrameTiming frameTiming = FrameTiming::Original;

    bool World::calcView(Vector& location, Rotator& rotation, float& fov)
    {
        UObject* viewer = localPlayer;
        if (!viewer || viewer->destroyed) {
            return false;
        }
        location = actor::Location(viewer);
        rotation = actor::Rotation(viewer);
        fov = pawn::FovAngle(viewer);
        static const Name namePlayerCalcView("PlayerCalcView");
        vm::Call call(viewer, namePlayerCalcView);
        if (call.valid()) {
            call.obj(viewer).v(location).r(rotation).run();
            location = *reinterpret_cast<Vector*>(call.parameter(1));
            rotation = *reinterpret_cast<Rotator*>(call.parameter(2));
        }
        return true;
    }

}
