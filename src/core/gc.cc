#include "core/gc.h"

#include "core/log.h"
#include "core/object.h"
#include "core/objects.h"
#include "core/profile.h"
#include "core/vm.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <typeindex>

namespace dx::gc {
    namespace {
        constexpr std::array<std::string_view, 6> Libraries = { "engine", "audio", "extension", "deusex", "render", "game" };

        struct State {
            std::vector<Holder*> holders;
            std::unordered_set<std::type_index> reported;
            std::unordered_set<std::string> librariesReported;
            std::vector<UObject*> later;
            // Addresses freeObjects deleted since the last collection, until a new object takes one.
            ReleasedSet freed;
            std::unordered_map<std::string, ClassCount> freedByClass;
            // The same since the last level change (resetLevelFigures).
            std::unordered_map<std::string, ClassCount> levelFreedByClass;
            std::vector<LevelMemory> levels;
            DataTotals data;
            // A ring, oldest first from samplesStart once it is full.
            std::vector<MemorySample> samples;
            std::vector<MemorySample> ordered;
            size_t samplesStart = 0;
            bool orderedValid = false;
            bool clockStarted = false;
            std::chrono::steady_clock::time_point clockStart;
            double lastSample = -1;
            bool keepFreed = false;
            bool collecting = false;
            bool enabled = true;
            // Whether every live object reports its references: 1 yes, 0 no, -1 not known. Kept
            // up as objects are registered and released, so asking does not walk the table.
            int8_t objectsReported = -1;
            Stats stats;
        };

        // Never destroyed: holders that are statics in other files unregister during exit, after
        // a static here would already be gone.
        State& state()
        {
            static State* instance = new State();
            return *instance;
        }

        // Adds a collection or release to the figures when it ends: a clock read at each end
        // and a few sums, nothing per object.
        class Timing {
        public:
            Timing(const Result& counted, bool fullCollection)
                : result(counted)
                , collection(fullCollection)
                , start(std::chrono::steady_clock::now())
            {
            }

            ~Timing()
            {
                double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                Stats& stats = state().stats;
                if (collection) {
                    stats.collections++;
                    stats.collectedObjects += result.freed;
                    stats.collectMilliseconds += milliseconds;
                    stats.lastCollectedObjects = result.freed;
                    stats.lastCollectMilliseconds = milliseconds;
                } else {
                    stats.releases++;
                    stats.releasedObjects += result.freed;
                    stats.releaseMilliseconds += milliseconds;
                }
            }

            Timing(const Timing&) = delete;
            Timing& operator=(const Timing&) = delete;

        private:
            const Result& result;
            bool collection;
            std::chrono::steady_clock::time_point start;
        };

        std::unordered_set<std::type_index>& reported()
        {
            State& s = state();
            if (s.reported.empty()) {
                s.reported.insert(typeid(UObject));
                s.reported.insert(typeid(UField));
                s.reported.insert(typeid(UTextBuffer));
                s.reported.insert(typeid(UEnum));
                s.reported.insert(typeid(UConst));
                s.reported.insert(typeid(UStruct));
                s.reported.insert(typeid(UFunction));
                s.reported.insert(typeid(UState));
                s.reported.insert(typeid(UClass));
                s.reported.insert(typeid(UProperty));
            }
            return s.reported;
        }

        bool inTable(const UObject* object)
        {
            const std::vector<UObject*>& table = objects::all();
            uint32_t slot = object ? objects::handleSlot(object->handle) : 0;
            return slot != 0 && slot < table.size() && table[slot] == object;
        }

        void addValues(const UProperty* property, uint8_t* memory, ReferenceVisitor& references, int depth);

        void addStruct(const UStruct* type, uint8_t* data, ReferenceVisitor& references, int depth, bool object)
        {
            if (depth > 32) {
                return;
            }
            static const Name outerName("Outer");
            static const Name className("Class");
            for (const UProperty* property : type->properties) {
                // The script copies of an object's own outer and class, which its C++ fields
                // hold and the collector handles.
                if (object && depth == 0 && (property->name == outerName || property->name == className)) {
                    continue;
                }
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    addValues(property, data + property->offset + static_cast<size_t>(i) * property->elementSize(), references, depth);
                }
            }
        }

        void addValues(const UProperty* property, uint8_t* memory, ReferenceVisitor& references, int depth)
        {
            switch (property->type) {
            case PropType::Object:
            case PropType::Class:
                references(*reinterpret_cast<UObject**>(memory));
                break;
            case PropType::Struct:
                if (property->structType) {
                    addStruct(property->structType, memory, references, depth + 1, false);
                }
                break;
            case PropType::Array:
                if (property->inner) {
                    auto* bytes = reinterpret_cast<std::vector<uint8_t>*>(memory);
                    size_t size = property->inner->elementSize();
                    for (size_t offset = 0; size != 0 && offset + size <= bytes->size(); offset += size) {
                        addValues(property->inner, bytes->data() + offset, references, depth + 1);
                    }
                }
                break;
            case PropType::FixedArray:
                if (property->inner) {
                    for (int32_t i = 0; i < property->fixedCount; i++) {
                        addValues(property->inner, memory + static_cast<size_t>(i) * property->inner->elementSize(), references, depth + 1);
                    }
                }
                break;
            case PropType::Byte:
            case PropType::Int:
            case PropType::Bool:
            case PropType::Float:
            case PropType::Name:
            case PropType::Str:
            case PropType::String:
            case PropType::Map:
            case PropType::Pointer:
                break;
            }
        }

        // What the collector knows about each object, by handle.
        enum class Mark : uint8_t {
            Unseen,
            Root,
            Candidate,
            Kept,
        };

        struct Marker {
            const Options& options;
            const std::unordered_set<const UObject*>& later;
            std::vector<Mark> marks;
            std::vector<int8_t> eliminatedCache;
            std::vector<UObject*> work;
            // Objects reached that are not in the object table. They cannot be deleted; what
            // they hold is followed once.
            std::unordered_set<const UObject*> outside;
            size_t cleared = 0;
            size_t danglingCleared = 0;
            bool freedAny = !state().freed.empty();

            Marker(const Options& given, const std::unordered_set<const UObject*>& releasing)
                : options(given)
                , later(releasing)
            {
            }

            // Marks are kept by slot.
            void grow(uint32_t slot)
            {
                if (marks.size() <= slot) {
                    marks.resize(objects::all().size() + 1, Mark::Unseen);
                    eliminatedCache.resize(marks.size(), -1);
                }
            }

            // Sorts an object in the table the first time it is seen.
            Mark& classify(const UObject* object)
            {
                uint32_t slot = objects::handleSlot(object->handle);
                grow(slot);
                Mark& mark = marks[slot];
                if (mark == Mark::Unseen) {
                    mark = later.count(object) || options.candidate(object) ? Mark::Candidate : Mark::Root;
                }
                return mark;
            }

            bool eliminated(const UObject* object)
            {
                uint32_t slot = objects::handleSlot(object->handle);
                grow(slot);
                int8_t& cached = eliminatedCache[slot];
                if (cached < 0) {
                    cached = static_cast<int8_t>(later.count(object) || (options.eliminate && options.eliminate(object)) ? 1 : 0);
                }
                return cached != 0;
            }

            void keep(const UObject* object)
            {
                Mark& mark = classify(object);
                if (mark == Mark::Candidate) {
                    mark = Mark::Kept;
                    work.push_back(const_cast<UObject*>(object));
                }
            }

            // A reference left behind by freeObjects points at freed memory; it is not followed.
            bool dangling(UObject*& reference)
            {
                if (!freedAny || !state().freed.count(reference)) {
                    return false;
                }
                reference = nullptr;
                cleared++;
                danglingCleared++;
                return true;
            }

            // A reference from something alive.
            void strong(UObject*& reference)
            {
                if (!reference || dangling(reference)) {
                    return;
                }
                if (!inTable(reference)) {
                    if (outside.insert(reference).second) {
                        work.push_back(reference);
                    }
                    return;
                }
                if (eliminated(reference)) {
                    reference = nullptr;
                    cleared++;
                    return;
                }
                keep(reference);
            }

            // A reference from a destroyed object, which keeps nothing but what it is made of.
            void weak(UObject*& reference, const std::array<const UObject*, 3>& own)
            {
                if (!reference || dangling(reference) || !inTable(reference)) {
                    return;
                }
                if (std::find(own.begin(), own.end(), reference) != own.end()) {
                    strong(reference);
                    return;
                }
                if (eliminated(reference) || classify(reference) != Mark::Root) {
                    reference = nullptr;
                    cleared++;
                }
            }

            struct Strong final : ReferenceVisitor {
                Marker& marker;

                explicit Strong(Marker& owner)
                    : marker(owner)
                {
                }

                void visit(UObject*& reference) override
                {
                    marker.strong(reference);
                }
            };

            struct Weak final : ReferenceVisitor {
                Marker& marker;
                // The destroyed object's class and the state and code its state frame is in.
                // An object keeps those for as long as it exists: its property values are torn
                // down through its class when it is deleted.
                std::array<const UObject*, 3> own;

                Weak(Marker& owner, const UObject* object)
                    : marker(owner)
                    , own { object->cls, object->stateFrame ? object->stateFrame->node : nullptr, object->stateFrame ? object->stateFrame->state : nullptr }
                {
                }

                void visit(UObject*& reference) override
                {
                    marker.weak(reference, own);
                }
            };

            void trace(UObject* object)
            {
                bool listed = inTable(object);
                if (listed && object->destroyed && classify(object) == Mark::Root) {
                    Weak visitor(*this, object);
                    object->addReferences(visitor);
                    return;
                }
                Strong visitor(*this);
                object->addReferences(visitor);
                // An object keeps its outer, as UObject::Serialize hands over Outer. An
                // eliminated outer is not kept; the object moves elsewhere if it survives.
                if (object->outer && inTable(object->outer) && !eliminated(object->outer)) {
                    keep(object->outer);
                }
            }

            void drain()
            {
                while (!work.empty()) {
                    UObject* object = work.back();
                    work.pop_back();
                    if (options.loadKept && !object->loaded && inTable(object)) {
                        objects::ensureLoaded(object);
                        objects::flushLoads();
                    }
                    trace(object);
                }
            }
        };

        // The roots when no candidate rule is given: native objects, objects of packages still
        // loaded, and those packages. SerializeRootSet keeps RF_Native objects, and a loaded
        // package's linker keeps what it has loaded.
        bool defaultCandidate(const UObject* object)
        {
            if (object->flags & RF_Native) {
                return false;
            }
            if (object->linker && objects::isLoadedLinker(object->linker)) {
                return false;
            }
            // Only a lookup: asking must not make a package named after the object.
            return object->outer || objects::findPackageObject(object->name) != object;
        }

        // Kept objects whose outer is going move to a new outer. Every live object's outer is
        // looked at, so this walks the whole table; only collections and release do it.
        void adoptOrphans(const ReleasedSet& dead, const std::function<UObject*(UObject*)>& adopt)
        {
            UObject* transient = nullptr;
            const std::vector<UObject*>& table = objects::all();
            auto going = [&](const UObject* object) {
                return dead.count(object) != 0;
            };
            for (size_t i = 0; i < table.size(); i++) {
                UObject* object = table[i];
                if (!object || dead.count(object) || !object->outer || !dead.count(object->outer)) {
                    continue;
                }
                UObject* replacement = adopt ? adopt(object->outer) : nullptr;
                if (!replacement || going(replacement)) {
                    if (!transient) {
                        transient = objects::packageObject(Name("Transient"));
                    }
                    replacement = transient;
                }
                object->outer = replacement;
                objects::syncObjectProperties(object);
            }
        }

        // Investigation (releaseTiming): where gc::release spends its time.
        struct ReleaseBreakdown {
            bool on = false;
            std::unordered_map<std::string, std::pair<uint64_t, int64_t>> parts;
            std::unordered_map<std::string, uint64_t> classes;

            void add(const std::string& part, int64_t ns)
            {
                auto& entry = parts[part];
                entry.first++;
                entry.second += ns;
            }
        };

        ReleaseBreakdown& breakdown()
        {
            static ReleaseBreakdown instance;
            return instance;
        }

        // Times a part of a release when the breakdown is on.
        class PartTimer {
        public:
            explicit PartTimer(const char* partName)
                : name(partName)
                , start(breakdown().on ? profile::nanoseconds() : 0)
            {
            }

            ~PartTimer()
            {
                if (breakdown().on) {
                    breakdown().add(name, profile::nanoseconds() - start);
                }
            }

            PartTimer(const PartTimer&) = delete;
            PartTimer& operator=(const PartTimer&) = delete;

        private:
            const char* name;
            int64_t start;
        };

        void finish(const std::vector<UObject*>& dead, const ReleasedSet& deadSet, const std::vector<UObject*>& packages)
        {
            // A holder's objectsReleased may unregister another holder.
            std::vector<Holder*> holders = state().holders;
            {
                PartTimer timer("holders (all objectsReleased)");
                for (Holder* holder : holders) {
                    if (std::find(state().holders.begin(), state().holders.end(), holder) != state().holders.end()) {
                        int64_t start = breakdown().on ? profile::nanoseconds() : 0;
                        holder->objectsReleased(deadSet);
                        if (breakdown().on) {
                            breakdown().add(std::string("holder ") + typeid(*holder).name(), profile::nanoseconds() - start);
                        }
                    }
                }
            }
            std::erase_if(state().later, [&](const UObject* object) { return deadSet.count(object) != 0; });
            if (breakdown().on) {
                for (const UObject* object : dead) {
                    breakdown().classes[object->cls ? object->cls->name.str() : std::string("(no class)")]++;
                }
            }
            // Slots are reused only while every handle in use is known; decided now, once.
            bool reuse = false;
            {
                PartTimer timer("everythingReported");
                reuse = everythingReported();
            }
            PartTimer timer("objects::releaseObjects");
            objects::releaseObjects(dead, packages, reuse);
        }

        struct CollectingScope {
            CollectingScope()
            {
                state().collecting = true;
            }

            ~CollectingScope()
            {
                state().collecting = false;
            }

            CollectingScope(const CollectingScope&) = delete;
            CollectingScope& operator=(const CollectingScope&) = delete;
        };
    }

    Holder::Holder()
    {
        state().holders.push_back(this);
    }

    Holder::~Holder()
    {
        std::erase(state().holders, this);
    }

    void Holder::addReferences(ReferenceVisitor&)
    {
    }

    void Holder::objectsReleased(const ReleasedSet&)
    {
    }

    void reportsReferences(const std::type_info& type)
    {
        reported().insert(type);
        referencesChanged();
    }

    void referencesChanged()
    {
        state().objectsReported = -1;
    }

    void objectRegistered(const UObject* object)
    {
        // A new object at a freed address is what a reference left there now names.
        if (!state().freed.empty()) {
            state().freed.erase(object);
        }
        // A registered object can only make the answer no.
        if (state().objectsReported == 1 && !reportsReferences(object)) {
            state().objectsReported = 0;
        }
    }

    void objectsReleased()
    {
        // A released object can only make the answer yes.
        if (state().objectsReported == 0) {
            state().objectsReported = -1;
        }
    }

    bool reportsReferences(const UObject* object)
    {
        if (!reported().count(typeid(*object))) {
            return false;
        }
        // Bytecode made in this process, by the script compiler, does not record where its
        // object references are.
        const UStruct* structType = cast<UStruct>(object);
        return !structType || structType->script.empty() || structType->scriptObjectsKnown;
    }

    std::vector<std::string> unreportedTypes()
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> result;
        for (UObject* object : objects::all()) {
            if (object && !reportsReferences(object)) {
                std::string type = typeid(*object).name();
                if (reported().count(typeid(*object))) {
                    type += " (compiled bytecode)";
                }
                if (seen.insert(type).second) {
                    result.push_back(type);
                }
            }
        }
        return result;
    }

    void libraryReported(std::string_view library)
    {
        state().librariesReported.emplace(library);
    }

    void forgetLibraryReported(std::string_view library)
    {
        state().librariesReported.erase(std::string(library));
    }

    bool everythingReported()
    {
        // The editor's own state is not covered.
        if (objects::editorMode()) {
            return false;
        }
        bool all = true;
        for (std::string_view library : Libraries) {
            if (!state().librariesReported.count(std::string(library))) {
                all = false;
            }
        }
        if (all && state().objectsReported < 0) {
            const std::vector<UObject*>& table = objects::all();
            state().objectsReported = std::none_of(table.begin(), table.end(), [](const UObject* object) { return object && !reportsReferences(object); }) ? 1 : 0;
        }
        all = all && state().objectsReported == 1;
        return all;
    }

    void setEnabled(bool enabled)
    {
        state().enabled = enabled;
    }

    bool enabled()
    {
        return state().enabled;
    }

    bool mayReleaseNow()
    {
        return state().enabled && !state().collecting && !vm::scriptRunning() && everythingReported();
    }

    bool isDefaultCandidate(const UObject* object)
    {
        return defaultCandidate(object);
    }

    bool isCollecting()
    {
        return state().collecting;
    }

    Stats stats()
    {
        return state().stats;
    }

    void addPropertyReferences(const UStruct* type, uint8_t* data, ReferenceVisitor& references)
    {
        addStruct(type, data, references, 0, false);
    }

    void addValueReferences(const UProperty* property, uint8_t* memory, ReferenceVisitor& references)
    {
        addValues(property, memory, references, 0);
    }

    Result collect(const Options& given)
    {
        Result result;
        // Off, nothing is ever deleted, whoever asks.
        if (state().collecting || !state().enabled) {
            return result;
        }
        DX_ZONE_NAMED("Garbage collection");
        CollectingScope scope;
        Timing timing(result, true);
        objects::flushLoads();
        Options options = given;
        if (!options.candidate) {
            options.candidate = defaultCandidate;
        }
        std::unordered_set<const UObject*> later(state().later.begin(), state().later.end());
        const std::vector<UObject*>& table = objects::all();

        // An outer that is not itself a registered object cannot be followed. One that freeObjects
        // deleted moves to the transient package; nothing else in the game makes one, so any
        // other is cut loose with a warning. The walks below then stay inside live objects.
        {
            std::unordered_set<const UObject*> registered(table.begin(), table.end());
            UObject* transient = nullptr;
            for (UObject* object : table) {
                if (object && object->outer && state().freed.count(object->outer)) {
                    if (!transient) {
                        transient = objects::packageObject(Name("Transient"));
                    }
                    object->outer = transient;
                    objects::syncObjectProperties(object);
                    continue;
                }
                if (object && object->outer && !registered.count(object->outer)) {
                    log::warn("DevGarbage", "{} ({}) has an outer that is not a registered object", object->name.str(), object->cls ? object->cls->name.str().c_str() : "no class");
                    object->outer = nullptr;
                }
            }
        }

        Marker marker(options, later);
        marker.grow(static_cast<uint32_t>(table.size()));
        // Every object that may not go is a root. Index-based: loading a kept object can add to
        // the table, and what it adds is sorted too.
        size_t scanned = 0;
        auto scanRoots = [&] {
            for (; scanned < table.size(); scanned++) {
                UObject* object = table[scanned];
                if (object && marker.classify(object) == Mark::Root) {
                    marker.trace(object);
                }
            }
        };
        scanRoots();
        {
            Marker::Strong visitor(marker);
            std::vector<Holder*> holders = state().holders;
            for (Holder* holder : holders) {
                holder->addReferences(visitor);
            }
        }
        for (;;) {
            marker.drain();
            scanRoots();
            marker.drain();
            bool added = false;
            if (options.keepInners) {
                for (size_t i = 0; i < table.size(); i++) {
                    UObject* object = table[i];
                    // A destroyed object is not something to find through its outer: a deleted
                    // flag stays inside its FlagBase but is gone unless something refers to it.
                    if (!object || !object->outer || object->destroyed || marker.classify(object) != Mark::Candidate || marker.eliminated(object)) {
                        continue;
                    }
                    int guard = 0;
                    for (UObject* outer = object->outer; outer && guard++ < 64; outer = outer->outer) {
                        if (inTable(outer) && marker.classify(outer) == Mark::Kept) {
                            marker.keep(object);
                            added = true;
                            break;
                        }
                    }
                }
            }
            if (!added && marker.work.empty() && scanned == table.size()) {
                break;
            }
        }

        std::vector<UObject*> dead;
        ReleasedSet deadSet;
        for (UObject* object : table) {
            if (!object) {
                continue;
            }
            Mark mark = marker.classify(object);
            if (mark == Mark::Candidate) {
                dead.push_back(object);
                deadSet.insert(object);
            } else if (mark == Mark::Kept) {
                result.kept++;
            }
        }
        result.cleared = marker.cleared;
        state().stats.lastDanglingCleared = marker.danglingCleared;
        // Every reference the mark reached is past; addresses freed before now may be reused.
        state().freed.clear();
        DX_PLOT("GC kept", result.kept);
        DX_PLOT("GC cleared", result.cleared);
        if (dead.empty() && options.packages.empty()) {
            DX_PLOT("GC freed", 0);
            return result;
        }
        adoptOrphans(deadSet, options.adoptOuter);
        result.freed = dead.size();
        DX_PLOT("GC freed", result.freed);
        finish(dead, deadSet, options.packages);
        return result;
    }

    Result release(const std::vector<UObject*>& given, bool clearReferences)
    {
        Result result;
        // Off, nothing is ever deleted, whoever asks.
        if (state().collecting || !state().enabled) {
            return result;
        }
        DX_ZONE_NAMED("Release objects");
        PartTimer whole("release (whole)");
        CollectingScope scope;
        std::vector<UObject*> list;
        ReleasedSet dead;
        for (UObject* object : given) {
            if (object && inTable(object) && dead.insert(object).second) {
                list.push_back(object);
            }
        }
        if (list.empty()) {
            return result;
        }
        Timing timing(result, false);
        if (clearReferences) {
            PartTimer timer("clear references (table scan)");

            struct Clear final : ReferenceVisitor {
                const ReleasedSet& dead;
                size_t cleared = 0;

                explicit Clear(const ReleasedSet& set)
                    : dead(set)
                {
                }

                void visit(UObject*& reference) override
                {
                    if (reference && dead.count(reference)) {
                        reference = nullptr;
                        cleared++;
                    }
                }
            } visitor(dead);

            const std::vector<UObject*>& table = objects::all();
            for (UObject* object : table) {
                if (object && !dead.count(object)) {
                    object->addReferences(visitor);
                }
            }
            std::vector<Holder*> holders = state().holders;
            for (Holder* holder : holders) {
                holder->addReferences(visitor);
            }
            result.cleared = visitor.cleared;
        }
        {
            PartTimer timer("adoptOrphans (table scan)");
            adoptOrphans(dead, nullptr);
        }
        if (breakdown().on) {
            breakdown().add(clearReferences ? "objects released (clearing)" : "objects released (not clearing)", static_cast<int64_t>(list.size()));
        }
        result.freed = list.size();
        DX_PLOT("Released objects", result.freed);
        finish(list, dead, {});
        return result;
    }

    Result freeObjects(const std::vector<UObject*>& given, bool clearHolders)
    {
        Result result;
        if (state().collecting || !state().enabled) {
            return result;
        }
        std::vector<UObject*> list;
        ReleasedSet dead;
        for (UObject* object : given) {
            if (object && !object->released && inTable(object) && dead.insert(object).second) {
                list.push_back(object);
            }
        }
        if (list.empty()) {
            return result;
        }
        DX_ZONE_NAMED("Free objects");
        PartTimer whole("freeObjects (whole)");
        auto start = std::chrono::steady_clock::now();
        CollectingScope scope;
        for (UObject* object : list) {
            std::string className = object->cls ? object->cls->name.str() : std::string("(no class)");
            uint64_t bytes = object->cls ? object->cls->size : 0;
            for (auto* counts : { &state().freedByClass, &state().levelFreedByClass }) {
                ClassCount& count = (*counts)[className];
                if (count.className.empty()) {
                    count.className = className;
                }
                count.freed++;
                count.freedBytes += bytes;
            }
        }
        if (state().keepFreed) {
            for (UObject* object : list) {
                object->released = true;
            }
            releaseLater(list);
        } else {
            // C++ state outside objects is bounded and may be read at any time, so it is cleared;
            // objects are not looked through.
            if (clearHolders) {
                PartTimer timer("freeObjects: clear holders");

                struct Clear final : ReferenceVisitor {
                    const ReleasedSet& dead;

                    explicit Clear(const ReleasedSet& set)
                        : dead(set)
                    {
                    }

                    void visit(UObject*& reference) override
                    {
                        if (reference && dead.count(reference)) {
                            reference = nullptr;
                        }
                    }
                } visitor(dead);

                std::vector<Holder*> holders = state().holders;
                for (Holder* holder : holders) {
                    if (std::find(state().holders.begin(), state().holders.end(), holder) != state().holders.end()) {
                        holder->addReferences(visitor);
                    }
                }
            }
            finish(list, dead, {});
            state().freed.insert(dead.begin(), dead.end());
        }
        result.freed = list.size();
        double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Stats& stats = state().stats;
        stats.frees++;
        stats.freedObjects += list.size();
        stats.freeMilliseconds += milliseconds;
        stats.longestFreeMilliseconds = std::max(stats.longestFreeMilliseconds, milliseconds);
        DX_PLOT("Freed objects", result.freed);
        return result;
    }

    bool wasFreed(const UObject* object)
    {
        return object && !state().freed.empty() && state().freed.count(object) != 0;
    }

    void setKeepFreed(bool on)
    {
        state().keepFreed = on;
    }

    bool keepFreed()
    {
        return state().keepFreed;
    }

    std::vector<ClassCount> freedByClassInLevel()
    {
        std::vector<ClassCount> result;
        for (const auto& [name, count] : state().levelFreedByClass) {
            result.push_back(count);
        }
        std::sort(result.begin(), result.end(), [](const ClassCount& a, const ClassCount& b) { return a.freedBytes != b.freedBytes ? a.freedBytes > b.freedBytes : a.className < b.className; });
        return result;
    }

    std::vector<ClassCount> freedByClass()
    {
        std::vector<ClassCount> result;
        for (const auto& [name, count] : state().freedByClass) {
            result.push_back(count);
        }
        std::sort(result.begin(), result.end(), [](const ClassCount& a, const ClassCount& b) { return a.freed > b.freed; });
        return result;
    }

    void recordLevelMemory(LevelMemory record)
    {
        record.seconds = memoryClock();
        state().levels.push_back(std::move(record));
    }

    void dataAllocated(size_t bytes)
    {
        DataTotals& data = state().data;
        data.allocatedBytes += bytes;
        data.allocations++;
    }

    void dataFreed(size_t bytes)
    {
        DataTotals& data = state().data;
        data.freedBytes += bytes;
        data.frees++;
    }

    DataTotals dataTotals()
    {
        return state().data;
    }

    double memoryClock()
    {
        State& s = state();
        auto now = std::chrono::steady_clock::now();
        if (!s.clockStarted) {
            s.clockStarted = true;
            s.clockStart = now;
        }
        return std::chrono::duration<double>(now - s.clockStart).count();
    }

    void sampleMemory()
    {
        // Four a second for ten minutes.
        constexpr double Interval = 0.25;
        constexpr size_t Kept = 2400;
        State& s = state();
        double now = memoryClock();
        if (s.lastSample >= 0 && now - s.lastSample < Interval) {
            return;
        }
        s.lastSample = now;
        MemorySample sample { now, s.data.allocatedBytes, s.data.freedBytes };
        if (s.samples.size() < Kept) {
            s.samples.push_back(sample);
        } else {
            s.samples[s.samplesStart] = sample;
            s.samplesStart = (s.samplesStart + 1) % Kept;
        }
        s.orderedValid = false;
    }

    const std::vector<MemorySample>& memorySamples()
    {
        State& s = state();
        if (!s.orderedValid) {
            s.ordered.assign(s.samples.begin() + static_cast<ptrdiff_t>(s.samplesStart), s.samples.end());
            s.ordered.insert(s.ordered.end(), s.samples.begin(), s.samples.begin() + static_cast<ptrdiff_t>(s.samplesStart));
            s.orderedValid = true;
        }
        return s.ordered;
    }

    const std::vector<LevelMemory>& levelMemory()
    {
        return state().levels;
    }

    void resetLevelFigures()
    {
        state().stats.longestFreeMilliseconds = 0;
        state().levelFreedByClass.clear();
    }

    size_t keptFreedObjects()
    {
        return static_cast<size_t>(std::count_if(state().later.begin(), state().later.end(), [](const UObject* object) { return object->released; }));
    }

    void setReleaseTiming(bool on)
    {
        breakdown() = ReleaseBreakdown();
        breakdown().on = on;
    }

    std::string releaseTimingReport()
    {
        std::vector<std::pair<std::string, std::pair<uint64_t, int64_t>>> parts(breakdown().parts.begin(), breakdown().parts.end());
        std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        std::string out = "release timing (part, calls, total ms, us per call; 'objects released' rows give objects, not time):\n";
        char line[512];
        for (const auto& [part, figures] : parts) {
            bool count = part.rfind("objects released", 0) == 0;
            std::snprintf(line, sizeof(line), "  %s, %llu, %.3f, %.2f\n", part.c_str(), static_cast<unsigned long long>(figures.first), count ? static_cast<double>(figures.second) : static_cast<double>(figures.second) / 1e6, figures.first ? static_cast<double>(figures.second) / 1e3 / static_cast<double>(figures.first) : 0.0);
            out += line;
        }
        out += "released classes:";
        for (const auto& [name, count] : breakdown().classes) {
            out += " " + name + "=" + std::to_string(count);
        }
        out += "\n";
        return out;
    }

    void releaseLater(const std::vector<UObject*>& objects)
    {
        for (UObject* object : objects) {
            if (object && std::find(state().later.begin(), state().later.end(), object) == state().later.end()) {
                state().later.push_back(object);
            }
        }
    }
}

namespace dx {
    // UObject::Serialize hands over the class, the state code being run and the property data.
    // The outer is handled by the collector, which keeps it or moves the object.
    void UObject::addReferences(ReferenceVisitor& references)
    {
        references(cls);
        if (stateFrame) {
            references(stateFrame->node);
            references(stateFrame->state);
            // The code being run points into the node's bytecode, so it stops with the node.
            if (!stateFrame->node) {
                stateFrame->code = nullptr;
            }
        }
        if (data && cls) {
            gc::addStruct(cls, data, references, 0, true);
        }
    }
}
