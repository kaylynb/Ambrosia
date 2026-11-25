#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_set>
#include <vector>

namespace dx {
    class UObject;
    class UStruct;
    class UProperty;

    // Receives the object pointers something holds, the way the original's FArchiveTagUsed
    // receives them from UObject::Serialize. The collector keeps what it is
    // given, or sets the pointer to None when its target is being eliminated, so whatever hands
    // a pointer over must cope with finding None there afterwards.
    class ReferenceVisitor {
    public:
        virtual void visit(UObject*& reference) = 0;

        void operator()(UObject*& reference)
        {
            visit(reference);
        }

        template <typename T>
            requires std::derived_from<T, UObject>
        void operator()(T*& reference)
        {
            UObject* object = reference;
            visit(object);
            if (object != reference) {
                reference = static_cast<T*>(object);
            }
        }

        template <typename T>
        void operator()(std::vector<T*>& references)
        {
            for (T*& reference : references) {
                (*this)(reference);
            }
        }

    protected:
        ~ReferenceVisitor() = default;
    };

    // The garbage collector, after UObject::CollectGarbage: everything a root
    // reaches through script property data, UObject::addReferences or a Holder is kept, and the
    // rest of the objects that may go are deleted.
    //
    // A native class that keeps an object pointer anywhere outside its script property data
    // overrides UObject::addReferences, calls the base class's version first, hands over every
    // such pointer, and is named in a gc::reportsReferences call. C++ state that is not an
    // object does the same through a Holder. Until that is true of every live object and every
    // library, collections stay limited to what is known to be safe; see everythingReported.
    namespace gc {
        using ReleasedSet = std::unordered_set<const UObject*>;

        // C++ state outside any object that holds object pointers: statics, caches, the audio
        // mixer, the world. It is registered for as long as it lives.
        class Holder {
        public:
            Holder();
            virtual ~Holder();
            Holder(const Holder&) = delete;
            Holder& operator=(const Holder&) = delete;

            // Hands over every object pointer held. What is handed over is kept alive.
            virtual void addReferences(ReferenceVisitor& references);
            // Called with the objects about to be deleted, for pointers that cannot be handed
            // over to be set to None: map keys, pointers to const, pointers into another thread.
            virtual void objectsReleased(const ReleasedSet& released);
        };

        // A Holder whose pointers are all weak: it only needs to hear which objects go.
        class ReleaseHook final : public Holder {
        public:
            explicit ReleaseHook(std::function<void(const ReleasedSet&)> released)
                : onReleased(std::move(released))
            {
            }

            void objectsReleased(const ReleasedSet& released) override
            {
                onReleased(released);
            }

        private:
            std::function<void(const ReleasedSet&)> onReleased;
        };

        // Says that objects of exactly these C++ types report every pointer they hold.
        void reportsReferences(const std::type_info& type);

        template <typename... T>
        void reportsReferences()
        {
            (reportsReferences(typeid(T)), ...);
        }

        bool reportsReferences(const UObject* object);
        // The C++ types of live objects that have not said they report their references.
        std::vector<std::string> unreportedTypes();
        // Says that a library hands every object pointer it holds to the collector. A library
        // may call it only when all of this is true of its sources:
        // - Every C++ class derived from UObject that it defines overrides addReferences if it
        //   has any field holding an object pointer (directly, in a struct, array, vector or
        //   map value, or in a byte copy of script data), and every such class, including
        //   those with nothing to add, is named in a reportsReferences call it makes.
        // - Every static, global, singleton or long-lived C++ object that keeps an object
        //   pointer past the end of the call that got it is, or is owned by, a Holder. Strong
        //   pointers go through addReferences; caches keyed on an object, pointers to const and
        //   pointers shared with another thread are dropped in objectsReleased.
        // - Nothing keeps a pointer to an object it may not report, such as one taken from a
        //   pointer to an UObject's C++ field into another library's state.
        // Pointers that only live for one call, and scratch storage that is overwritten before
        // it is read again, need nothing.
        //
        // Where the call goes, once, at start-up:
        // - engine: registerEngineNatives (engine/natives_engine.cc)
        // - audio: the MixerReferences holder's constructor (audio/audio.cc)
        // - extension: extension::registerNatives (extension/module.cc)
        // - deusex: registerDeusExNatives (deusex/natives_deusex.cc)
        // - render: SceneRenderer::SceneRenderer (render/scene_renderer.cc)
        // - game: Game::start (game/game_start.cc)
        // The editor is not one of them: collections stay off in editor mode until its state
        // is covered too.
        void libraryReported(std::string_view library);
        // Undoes libraryReported, for tests that turn freeing during play on and back off.
        void forgetLibraryReported(std::string_view library);
        // True once every live object's type and every library reports its references. Until
        // then a collection must only free objects whose holders are all known. The answer is
        // kept up as objects come and go; unreportedTypes is the exact, slow listing.
        bool everythingReported();
        // Keep everythingReported's answer current. The object table calls the first two; code
        // that changes whether an existing object reports, such as the script compiler
        // replacing a struct's bytecode, calls referencesChanged.
        void objectRegistered(const UObject* object);
        void objectsReleased();
        void referencesChanged();

        struct Options {
            // Objects that may be deleted. Every other object is kept and traced as a root. Left
            // empty, every object not reached from a root may go except native ones and those
            // of a package that is still loaded.
            std::function<bool(const UObject*)> candidate;
            // References to these are set to None and they are never kept, as with the
            // original's RF_EliminateObject. Those that are candidates are deleted.
            std::function<bool(const UObject*)> eliminate;
            // A candidate whose outer is kept is kept too, unless it is destroyed. Covers
            // objects found only by looking through their outer, such as the flags inside a
            // FlagBase.
            bool keepInners = false;
            // Unloaded candidates that are kept are loaded first, so they survive their file
            // being released.
            bool loadKept = false;
            // Where a kept object whose outer is deleted goes, given that outer. Null, or
            // returning null, puts it in the transient package.
            std::function<UObject*(UObject*)> adoptOuter;
            // Packages whose linker and file image go along with the deleted objects.
            std::vector<UObject*> packages;
        };

        struct Result {
            size_t freed = 0;
            // Candidates kept because something still reaches them.
            size_t kept = 0;
            // References set to None.
            size_t cleared = 0;
        };

        // Off, nothing is freed: GarbageCollection=False under [Engine.GameEngine]. On by default.
        // collect and release check it themselves and do nothing when it is off, so no caller
        // has to.
        void setEnabled(bool enabled);
        bool enabled();
        // Whether objects may be deleted during play now, as CleanupDestroyed and SafeDestroy do:
        // freeing is on, every library and type reports its references, no collection is
        // running and no script is on the stack.
        bool mayReleaseNow();

        Result collect(const Options& options);

        // What the collector has done since the program started, for the debug overlay: full
        // collections (collect) and direct releases (release, releaseLater's at the next
        // collection), each counted only when it got as far as looking for what to delete.
        struct Stats {
            size_t collections = 0;
            size_t collectedObjects = 0;
            double collectMilliseconds = 0;
            size_t lastCollectedObjects = 0;
            double lastCollectMilliseconds = 0;
            size_t releases = 0;
            size_t releasedObjects = 0;
            double releaseMilliseconds = 0;
            // freeObjects: calls, objects deleted (or kept, with KeepFreedObjects), time, and the
            // longest single call.
            size_t frees = 0;
            size_t freedObjects = 0;
            double freeMilliseconds = 0;
            double longestFreeMilliseconds = 0;
            // References the last collection set to None because they held a freed address.
            size_t lastDanglingCleared = 0;
        };

        Stats stats();

        // Objects deleted by freeObjects, by class name, since the program started; and those
        // KeepFreedObjects is holding now. For the memory tool.
        struct ClassCount {
            std::string className;
            size_t freed = 0;
            uint64_t freedBytes = 0;
        };

        std::vector<ClassCount> freedByClass();
        // The same since the last level change (resetLevelFigures), the most script data first.
        std::vector<ClassCount> freedByClassInLevel();
        size_t keptFreedObjects();

        // One per level change, recorded by the game after the collection: the figures its
        // "memlog" log line gives, kept for the memory tool's history.
        struct LevelMemory {
            std::string map;
            // memoryClock when it was recorded.
            double seconds = 0;
            uint64_t processBytes = 0;
            size_t objects = 0;
            uint64_t scriptBytes = 0;
            double collectMilliseconds = 0;
            size_t collected = 0;
            size_t danglingCleared = 0;
            // freeObjects during the level that was left: objects, and the longest call.
            size_t freedInLevel = 0;
            double longestFreeMilliseconds = 0;
        };

        void recordLevelMemory(LevelMemory record);

        // Script property data the object table makes and deletes (objects::allocateObjectData,
        // objects::freeObjectData): bytes and blocks since the program started.
        struct DataTotals {
            uint64_t allocatedBytes = 0;
            uint64_t freedBytes = 0;
            uint64_t allocations = 0;
            uint64_t frees = 0;
        };

        void dataAllocated(size_t bytes);
        void dataFreed(size_t bytes);
        DataTotals dataTotals();

        // A sample of the totals for the memory tool's plot, taken at most four times a second
        // when sampleMemory is called (once a frame); the last ten minutes are kept. Seconds are
        // of real time since the first sample. Level changes carry the same clock (LevelMemory).
        struct MemorySample {
            double seconds = 0;
            uint64_t allocatedBytes = 0;
            uint64_t freedBytes = 0;
        };

        void sampleMemory();
        const std::vector<MemorySample>& memorySamples();
        double memoryClock();
        const std::vector<LevelMemory>& levelMemory();
        // Starts the per-level figures again, for the next level's record: the longest call and
        // freedByClassInLevel.
        void resetLevelFigures();
        // The rule collect uses when Options::candidate is empty.
        bool isDefaultCandidate(const UObject* object);
        bool isCollecting();

        // Deletes these objects now, setting every reference to them to None first: a walk of
        // every live object. For tests and tools; play uses freeObjects. With clearReferences false the
        // caller vouches that nothing refers to them any more.
        Result release(const std::vector<UObject*>& objects, bool clearReferences = true);
        // Deletes these objects now without looking through other objects for references to
        // them, as the original does during play: XWindow::SafeDestroy
        // deletes a window, ULevel::CleanupDestroyed the destroyed actors,
        // UEventManager::CleanupSlot the events it unlinks. A script
        // variable or native field in another object that still holds one is left pointing at
        // freed memory, as in the original. C++ state registered as a Holder is still cleared,
        // and the next collection sets any such reference it reaches to None (wasFreed).
        // An object whose outer is freed keeps that outer until the next collection moves it.
        // With clearHolders false the caller vouches that no Holder holds them either, as for AI
        // events, which only the event manager's lists held.
        Result freeObjects(const std::vector<UObject*>& objects, bool clearHolders = true);
        // Whether an object was deleted by freeObjects since the last collection and its address has not
        // gone to a new object since. Code that may read a reference left behind by freeObjects, such as
        // the collection's mark and the save writer, asks before following it.
        bool wasFreed(const UObject* object);
        // Debug (KeepFreedObjects): freeObjects keeps the objects instead of
        // deleting them, marks them `released`, and they go at the next collection with every
        // reference set to None. Script that reads one is logged (vm.cc), naming it.
        void setKeepFreed(bool on);
        bool keepFreed();
        // Destroyed objects to delete at the next collection, with references to them set to
        // None then.
        void releaseLater(const std::vector<UObject*>& objects);

        // Times the parts of each release and freeObjects call (each holder's objectsReleased, the
        // deletion) from now, forgetting earlier figures; and the figures.
        void setReleaseTiming(bool on);
        std::string releaseTimingReport();

        // The object pointers in script data laid out as `type`: a whole struct, or one value
        // of `property`. For C++ state that keeps copies of script values.
        void addPropertyReferences(const UStruct* type, uint8_t* data, ReferenceVisitor& references);
        void addValueReferences(const UProperty* property, uint8_t* memory, ReferenceVisitor& references);
    }
}
