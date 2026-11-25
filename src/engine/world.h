#pragma once

#include "core/gc.h"
#include "core/math.h"
#include "core/object.h"
#include "engine/collision_hash.h"
#include "engine/decal.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx {
    class ULevel;
    class UModel;

    struct CheckResult {
        UObject* actor = nullptr;
        Vector location;
        Vector normal;
        float time = 1.0f;
        int32_t item = -1;
        UObject* material = nullptr;
    };

    constexpr uint32_t TRACE_Pawns = 0x01;
    constexpr uint32_t TRACE_Movers = 0x02;
    constexpr uint32_t TRACE_Level = 0x04;
    constexpr uint32_t TRACE_ZoneChanging = 0x08;
    constexpr uint32_t TRACE_Others = 0x10;
    constexpr uint32_t TRACE_OnlyProjActor = 0x20;
    constexpr uint32_t TRACE_Blocking = 0x40;
    constexpr uint32_t TRACE_LevelGeometry = 0x80;
    constexpr uint32_t TRACE_ShadowCast = 0x100;
    constexpr uint32_t TRACE_StopAtFirstHit = 0x200;
    // How the game handles ticks of different lengths (docs/differences.md, "Frame-rate dependence"). Original copies
    // Deus Ex, where some things depend on the frame rate; Steady makes them run the same at any
    // frame rate, so a glitch can be checked under both. The engine starts in Original, which
    // the parity tests rely on; the game sets it from its options.
    enum class FrameTiming { Original,
        Steady };
    extern FrameTiming frameTiming;

    inline bool steadyTiming()
    {
        return frameTiming == FrameTiming::Steady;
    }

    constexpr uint32_t TRACE_World = TRACE_Movers | TRACE_Level | TRACE_LevelGeometry;
    constexpr uint32_t TRACE_AllColliding = TRACE_Pawns | TRACE_Movers | TRACE_Level | TRACE_Others | TRACE_LevelGeometry;

    struct TravelValue {
        UProperty* property = nullptr;
        int32_t index = 0;
        std::vector<uint8_t> data;
    };

    struct TravelActor {
        UClass* cls = nullptr;
        UObject* original = nullptr;
        bool isPlayer = false;
        std::vector<TravelValue> values;
    };

    // Hands over every object pointer in travel data: the classes, the actors left behind and
    // the object values.
    void addTravelReferences(std::vector<TravelActor>& travel, ReferenceVisitor& references);

    // Travel data on its way from the level being left to the next one. Storing the departing
    // level deletes destroyed actors, and the collector sets every reference it is told of to
    // them to None, so the data is held here, where the collector sees it, until the new world's
    // incomingTravel takes it over. Whatever is still here when it goes is released.
    class PendingTravel final : public gc::Holder {
    public:
        PendingTravel() = default;
        PendingTravel(PendingTravel&& other) noexcept;
        PendingTravel& operator=(PendingTravel&& other) noexcept;
        ~PendingTravel() override;

        void addReferences(ReferenceVisitor& references) override;

        // Hands the data over, leaving this empty.
        std::vector<TravelActor> take();

        std::vector<TravelActor> actors;
    };

    // A Holder: the world's own pointers to its level, actors and travel data keep them, and the
    // collision hash and decals drop actors that are deleted.
    class World : public gc::Holder {
    public:
        World();
        ~World() override;
        void addReferences(ReferenceVisitor& references) override;
        void objectsReleased(const gc::ReleasedSet& released) override;
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // The world in play, for script natives and engine callbacks that are called with no
        // way to be given one. Code that can take a World as an argument should do that
        // instead. There is none while no level is loaded, as in the editor.
        static bool hasActive()
        {
            return current != nullptr;
        }

        static World& active()
        {
            assert(current != nullptr);
            return *current;
        }

        // Keeps a world in play until the end of the scope, then puts back the one that was in
        // play before. A World makes itself current as it is constructed, so the scope can be
        // made before the world it is for: it saves the one from before, and activate() switches
        // to the new one once it exists.
        class ActiveScope {
        public:
            // Saves the world in play now, for a world that is constructed afterwards.
            ActiveScope()
                : previous(current)
            {
            }

            // Makes world current until the end of the scope.
            explicit ActiveScope(World& world)
                : previous(current)
            {
                current = &world;
            }

            // Makes world current until the end of the scope, for a world constructed after the
            // scope. The scope already saved the world that was in play before it.
            void activate(World& world)
            {
                current = &world;
            }

            ~ActiveScope()
            {
                current = previous;
            }

            ActiveScope(const ActiveScope&) = delete;
            ActiveScope& operator=(const ActiveScope&) = delete;

        private:
            World* previous;
        };

        bool loadMap(const std::string& url);
        PendingTravel collectTravel();
        std::string travelString(UObject* player) const;
        std::vector<std::pair<std::string, std::string>> arrivalTravelInfo;
        bool replaceTravelInfo = false;
        static void releaseTravel(std::vector<TravelActor>& travel);
        std::vector<UObject*> applyTravel(UObject* player);
        void rehomeCarried(const std::vector<std::pair<TravelActor*, UObject*>>& targets, const std::unordered_map<UObject*, UObject*>& remap);
        void tick(float deltaSeconds);
        bool calcView(Vector& location, Rotator& rotation, float& fov);
        UObject* player() const;

        UObject* spawn(UClass* cls, UObject* owner, Name tag, const Vector& location, const Rotator& rotation, UObject* templateActor, bool noCollisionFail, UObject* instigator = nullptr);
        bool destroy(UObject* actor);
        bool inStasis(UObject* actor);

        bool findSpot(const Vector& extent, Vector& location, bool checkActors, bool assumeFit);
        bool pointCheck(CheckResult& hit, const Vector& location, const Vector& extent, UObject* source, bool checkActors);
        bool lineCheck(CheckResult& hit, const Vector& start, const Vector& end, const Vector& extent, UObject* source, uint32_t flags);
        std::vector<CheckResult> multiLineCheck(const Vector& start, const Vector& end, const Vector& extent, UObject* source, uint32_t flags, uint8_t nodeFlags = 0);
        bool bspLineCheck(CheckResult& hit, const Vector& start, const Vector& end, const Vector& extent, uint8_t nodeFlags = 0);
        bool fastLineCheck(const Vector& end, const Vector& start);
        bool encroaches(UObject* actor, const Vector& location, bool touchNotify = false);
        bool moveActor(UObject* actor, const Vector& delta, const Rotator& rotation, CheckResult& hit, bool test, bool ignorePawns, bool ignoreBases, bool noFail);
        bool farMoveActor(UObject* actor, const Vector& location, bool test, bool noCheck);
        void setLocationZone(UObject* actor, const Vector& location, bool test = false);
        UObject* zoneAt(const Vector& location, int32_t* leaf, uint8_t* zoneNumber);
        int32_t pointLeaf(const Vector& location);
        std::shared_ptr<const std::vector<UObject*>> liveActors();

        bool tickActor(UObject* actor, float deltaSeconds);
        void tickPawnSenses(UObject* pawn, float deltaSeconds);
        void aiEvents();
        void cleanupDestroyed(bool force = false);
        // Deletes the destroyed actors cleanupDestroyed has cleared references to, once no script
        // is running. A save that script starts clears them, and the next tick deletes them.
        void releaseDestroyed();

        ULevel* level = nullptr;
        UObject* levelInfo = nullptr;
        UObject* gameInfo = nullptr;
        UObject* localPlayer = nullptr;
        // The session's viewport, which the game hands to each level and login makes on the
        // first; not this world's own.
        UObject* viewport = nullptr;
        std::vector<UObject*> actors;
        CollisionHash collisionHash;
        DecalRegistry decals;
        uint64_t actorsVersion = 0;
        float timeSeconds = 0;
        uint32_t frame = 0;
        // Steady timing: time from frames too short to tick, carried into the next tick.
        float shortFrameTime = 0;
        // Steady timing: the effect clock (SteadyEffectRate). effectTime is the time not yet
        // given to the actors on it, effectStep what this tick gives them (0 when they skip
        // it), and effectSteps counts the ticks that gave them any.
        float effectTime = 0;
        float effectStep = 0;
        uint32_t effectSteps = 0;
        std::string url;
        std::string travelUrl;
        int32_t travelType = -1;
        bool travelPending = false;
        bool travelItems = false;
        std::vector<TravelActor> incomingTravel;
        bool paused = false;
        bool pendingCleanup = false;
        std::vector<UObject*> destroyedActors;
        std::vector<UObject*> releasingActors;
        uint32_t tickParity = 0;
        size_t firstDynamicActor = 0;
        bool inTick = false;
        std::vector<UObject*> newlySpawned;
        std::function<void(UObject*, float)> inputHook;

    private:
        // The world in play: the last one made, or the one an ActiveScope gives.
        static World* current;

        // Whether the level has been announced as loaded (engine/level_events.h), and so is owed
        // an unloading notice.
        bool levelAnnounced = false;

        std::shared_ptr<const std::vector<UObject*>> liveActorsCache;
        uint64_t liveActorsVersion = 0;
        uint64_t liveActorsDestroys = 0;
    };

    namespace engine {
        void registerEngineNatives();

        // Game-specific callbacks for the world. The game layer installs them when it
        // registers its native functions, before the first level loads.
        struct GameHooks {
            // Once a level's actors are set up, before a saved game's state is restored.
            void (*levelLoaded)(World& world) = nullptr;
            // AScriptedPawn::Tick, ahead of the rest of the pawn's tick; false when the pawn
            // was destroyed and the tick should stop.
            bool (*scriptedPawnTick)(World& world, UObject* pawn, float deltaSeconds) = nullptr;
            // Under Steady timing, whether this tick the actor's script Tick runs on the effect
            // clock (SteadyEffectRate) instead of every tick.
            bool (*onEffectClock)(UObject* actor) = nullptr;
        };

        GameHooks& gameHooks();
    }
}
