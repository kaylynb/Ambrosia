#include "engine/world.h"

#include "audio/audio.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/resources.h"

#include <algorithm>

namespace dx {
    UObject* World::spawn(UClass* cls, UObject* owner, Name tag, const Vector& location, const Rotator& rotation, UObject* templateActor, bool noCollisionFail, UObject* instigator)
    {
        if (!cls) {
            return nullptr;
        }
        objects::ensureLoaded(cls);
        cls->linkClass();
        if ((cls->classFlags & CLASS_Abstract) || !cls->isChildOf(engineClasses().actor)) {
            log::error("DevSpawn", "spawn failed: class {} is abstract or not an actor", cls->name.str());
            return nullptr;
        }
        UObject* defaults = cls->defaultObject;
        if (actor::bStatic(defaults) || actor::bNoDelete(defaults)) {
            log::error("DevSpawn", "spawn failed: class {} is static or nodelete", cls->name.str());
            return nullptr;
        }
        UObject* source = templateActor ? templateActor : defaults;
        Vector spawnLocation = location;
        if ((actor::bCollideWorld(source) || actor::bCollideWhenPlacing(source)) && !noCollisionFail) {
            if (!findSpot(cylinderExtent(source), spawnLocation, false, true)) {
                return nullptr;
            }
        }
        UObject* a = objects::newObject(cls, level && level->outer ? level->outer : objects::packageObject(Name("Transient")), Name(), 0);
        if (templateActor) {
            cls->copyValue(a->data, templateActor->data);
            objects::syncObjectProperties(a);
        }
        actors.push_back(a);
        actorsVersion++;
        if (level) {
            level->actors = actors;
        }
        actor::Tag(a) = tag.none() ? cls->name : tag;
        actor::Level(a) = levelInfo;
        actor::XLevel(a) = level;
        actor::bTicked(a) = tickParity ? 0 : 1;
        actor::Brush(a) = nullptr;
        actor::Location(a) = spawnLocation;
        actor::OldLocation(a) = spawnLocation;
        actor::Rotation(a) = rotation;
        collisionHash.update(a);
        region(a) = { levelInfo, -1, 0 };
        if (isPawn(a)) {
            *reinterpret_cast<PointRegion*>(pawn::FootRegion(a)) = { levelInfo, -1, 0 };
            *reinterpret_cast<PointRegion*>(pawn::HeadRegion(a)) = { levelInfo, -1, 0 };
        }
        if (engineClasses().playerPawnExt && a->isA(engineClasses().playerPawnExt)) {
            uint8_t* flagBase = a->propertyAddress(Name("flagBase"));
            UClass* flagClass = objects::findClass("Extension.FlagBase");
            if (flagBase && flagClass && !*reinterpret_cast<UObject**>(flagBase)) {
                *reinterpret_cast<UObject**>(flagBase) = objects::newObject(flagClass, objects::packageObject(Name("Transient")), Name(), 0);
            }
        }
        if (owner) {
            setOwner(a, owner);
        }
        actor::Instigator(a) = instigator;
        vm::event(a, Name("Spawned"));
        vm::event(a, Name("PreBeginPlay"));
        vm::event(a, Name("BeginPlay"));
        if (a->destroyed || actor::bDeleteMe(a)) {
            return nullptr;
        }
        setLocationZone(a, actor::Location(a));
        actor::LastRenderTime(a) = -10.0f;
        vm::event(a, Name("PostBeginPlay"));
        if (a->destroyed) {
            return nullptr;
        }
        if (!noCollisionFail && encroaches(a, actor::Location(a))) {
            destroy(a);
            return nullptr;
        }
        vm::event(a, Name("SetInitialState"));
        if (a->destroyed) {
            return nullptr;
        }
        if (!actor::Base(a) && actor::bCollideWorld(a) && (a->isA(engineClasses().decoration) || a->isA(engineClasses().inventory) || isPawn(a))) {
            uint8_t physics = actor::Physics(a);
            if (physics == PHYS_None || physics == PHYS_Rotating) {
                findBase(a);
            }
        }
        vm::event(a, Name("PostPostBeginPlay"));
        if (a->destroyed) {
            return nullptr;
        }
        if (inTick) {
            newlySpawned.push_back(a);
        }
        return a;
    }

    bool World::destroy(UObject* a)
    {
        if (!a) {
            return false;
        }
        if (actor::bStatic(a) || actor::bNoDelete(a)) {
            return false;
        }
        if (actor::bDeleteMe(a) || a->destroyed) {
            return true;
        }
        if (a->stateFrame && a->stateFrame->state) {
            vm::event(a, Name("EndState"));
            if (a->destroyed) {
                return true;
            }
        }
        if (actor::Base(a)) {
            setBase(a, nullptr, true);
        }
        if (actor::StandingCount(a) > 0) {
            for (size_t i = 0; i < actors.size(); i++) {
                UObject* other = actors[i];
                if (other && actor::Base(other) == a) {
                    setBase(other, nullptr, true);
                }
            }
        }
        collisionHash.remove(a);
        vm::event(a, Name("Destroyed"));
        if (a->destroyed) {
            return true;
        }
        for (size_t index = 0; index < actors.size(); index++) {
            UObject* other = actors[index];
            if (!other || other == a || other->destroyed) {
                continue;
            }
            if (actor::Owner(other) == a) {
                setOwner(other, nullptr);
            } else {
                for (int i = 0; i < 4; i++) {
                    if (actor::Touching(other, i) == a) {
                        endTouch(a, other, true);
                    }
                }
            }
        }
        if (actor::Owner(a)) {
            vm::Call(actor::Owner(a), Name("LostChild")).obj(a).run();
        }
        auto it = std::find(actors.begin(), actors.end(), a);
        if (it != actors.end()) {
            *it = nullptr;
        }
        actorsVersion++;
        actor::bDeleteMe(a) = 1;
        if (UObject* iterator = actor::RenderInterface(a)) {
            actor::RenderInterface(a) = nullptr;
            if (!iterator->destroyed) {
                objects::destroyObject(iterator);
            }
        }
        a->destroyed = true;
        collisionHash.remove(a);
        // Decal.Destroyed detaches the decal, and no shipped class overrides it. A script that
        // skips it would leave the original's surfaces pointing at a freed actor.
        detachDecal(*this, a);
        audio::actorDestroyed(a);
        if (localPlayer == a) {
            localPlayer = nullptr;
        }
        destroyedActors.push_back(a);
        pendingCleanup = true;
        return true;
    }

    std::shared_ptr<const std::vector<UObject*>> World::liveActors()
    {
        uint64_t destroys = objects::destroyCount();
        if (!liveActorsCache || liveActorsVersion != actorsVersion || liveActorsDestroys != destroys) {
            auto list = std::make_shared<std::vector<UObject*>>();
            list->reserve(actors.size());
            for (UObject* a : actors) {
                if (a && !a->destroyed) {
                    list->push_back(a);
                }
            }
            liveActorsCache = list;
            liveActorsVersion = actorsVersion;
            liveActorsDestroys = destroys;
        }
        return liveActorsCache;
    }
}
