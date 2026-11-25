#include "engine/level_events.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace dx::levelevents {
    namespace {
        struct Entry {
            uint64_t id = 0;
            Stage stage = Stage::Engine;
            Listener listener;
        };

        struct Registry {
            // In the order they are told of a loaded level: by stage, then by subscription.
            std::vector<Entry> entries;
            uint64_t nextId = 1;
        };

        // Never freed, so that a subscription held by an object destroyed at exit can still give
        // up its place whatever order statics go in.
        Registry& registry()
        {
            static Registry* instance = new Registry;
            return *instance;
        }

        // Tells each listener there was when the notice began, in order, skipping any that have
        // unsubscribed meanwhile.
        template <typename Pick>
        void notify(World& world, bool reverse, Pick pick)
        {
            std::vector<uint64_t> ids;
            for (const Entry& entry : registry().entries) {
                ids.push_back(entry.id);
            }
            if (reverse) {
                std::reverse(ids.begin(), ids.end());
            }
            for (uint64_t id : ids) {
                std::vector<Entry>& entries = registry().entries;
                auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.id == id; });
                if (found == entries.end()) {
                    continue;
                }
                // A copy, as the listener may unsubscribe and so free its own entry.
                std::function<void(World&)> call = pick(found->listener);
                if (call) {
                    call(world);
                }
            }
        }
    }

    Subscription subscribe(Stage stage, Listener listener)
    {
        Registry& r = registry();
        Entry entry { r.nextId++, stage, std::move(listener) };
        auto after = std::upper_bound(r.entries.begin(), r.entries.end(), stage, [](Stage wanted, const Entry& existing) { return wanted < existing.stage; });
        uint64_t id = entry.id;
        r.entries.insert(after, std::move(entry));
        return Subscription([](uint64_t gone) { std::erase_if(registry().entries, [&](const Entry& existing) { return existing.id == gone; }); }, id);
    }

    void levelLoaded(World& world)
    {
        notify(world, false, [](const Listener& listener) { return listener.loaded; });
    }

    void levelUnloading(World& world)
    {
        notify(world, true, [](const Listener& listener) { return listener.unloading; });
    }
}
