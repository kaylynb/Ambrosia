#include "engine/world.h"

#include "core/gc.h"
#include "core/objects.h"
#include "engine/actor.h"
#include "engine/ai.h"
#include "engine/resources.h"

#include <algorithm>
#include <unordered_map>

namespace dx {
    namespace {
        // Where a class or struct keeps object references: its own object properties and the
        // struct properties it holds, as UStruct::CleanupDestroyed walks them.
        struct ReferenceLayout {
            std::vector<size_t> objects;
            std::vector<std::pair<size_t, UStruct*>> structs;
        };

        // Keyed on the struct, so a deleted struct's entry goes before its address is reused.
        struct LayoutTable final : gc::Holder {
            std::unordered_map<const UStruct*, ReferenceLayout> entries;

            void objectsReleased(const gc::ReleasedSet& released) override
            {
                std::erase_if(entries, [&](const auto& entry) { return released.count(entry.first) != 0; });
            }
        };

        const ReferenceLayout& referenceLayout(UStruct* type)
        {
            static LayoutTable layouts;
            auto& table = layouts.entries;
            auto it = table.find(type);
            if (it != table.end()) {
                return it->second;
            }
            ReferenceLayout layout;
            for (UProperty* property : type->properties) {
                if (property->name == Name("Outer") || property->name == Name("Class")) {
                    continue;
                }
                for (int32_t i = 0; i < property->arrayDim; i++) {
                    size_t offset = property->offset + static_cast<size_t>(i) * property->elementSize();
                    if (property->type == PropType::Object) {
                        layout.objects.push_back(offset);
                    } else if (property->type == PropType::Struct && property->structType) {
                        layout.structs.emplace_back(offset, property->structType);
                    }
                }
            }
            return table.emplace(type, std::move(layout)).first->second;
        }

        void scrubReferences(UStruct* type, uint8_t* data, int depth = 0)
        {
            if (depth > 32) {
                return;
            }
            const ReferenceLayout& layout = referenceLayout(type);
            for (size_t offset : layout.objects) {
                UObject*& reference = *reinterpret_cast<UObject**>(data + offset);
                // A reference to something freeObjects deleted (a window, say) is not followed.
                if (reference && (gc::wasFreed(reference) || reference->destroyed)) {
                    reference = nullptr;
                }
            }
            for (const auto& [offset, structType] : layout.structs) {
                scrubReferences(structType, data + offset, depth + 1);
            }
        }
    }

    // ULevel::CleanupDestroyed. The actor list is packed every tick, but
    // references to destroyed actors are only cleared once 128 are waiting, or when forced
    // (UGameEngine::LoadMap and the save pass 1; ULevel::Tick
    // passes 0). Until then script still sees a destroyed actor, with bDeleteMe set,
    // through any variable that held it. As in the original, only the actors in the level's list
    // are cleared (struct members included), and then the destroyed actors are freed, leaving any
    // other object that held one pointing at freed memory until the next level change.
    void World::cleanupDestroyed(bool force)
    {
        if (pendingCleanup) {
            pendingCleanup = false;
            actors.erase(std::remove(actors.begin() + std::min<ptrdiff_t>(2, static_cast<ptrdiff_t>(actors.size())), actors.end(), nullptr), actors.end());
            actorsVersion++;
            if (level) {
                level->actors = actors;
            }
        }
        if (destroyedActors.empty() || (destroyedActors.size() < 128 && !force)) {
            return;
        }
        UEventManager* manager = levelInfo ? cast<UEventManager>(levelinfo::EventManager(levelInfo)) : nullptr;
        for (UObject* a : destroyedActors) {
            if (manager) {
                manager->destroyActor(a);
            }
        }
        for (UObject* object : actors) {
            if (object && !object->destroyed && object->data && object->cls) {
                scrubReferences(object->cls, object->data);
            }
        }
        // The original deletes the destroyed actors here; Ambrosia does at the end of the tick,
        // once no script is running, and only once the level-change collection can tell freed
        // memory from an object (everything reports its references).
        if (gc::enabled() && gc::everythingReported()) {
            releasingActors.insert(releasingActors.end(), destroyedActors.begin(), destroyedActors.end());
        }
        destroyedActors.clear();
        releaseDestroyed();
    }

    void World::releaseDestroyed()
    {
        if (releasingActors.empty() || !gc::mayReleaseNow()) {
            return;
        }
        std::vector<UObject*> actorsToRelease = std::move(releasingActors);
        releasingActors.clear();
        gc::freeObjects(actorsToRelease);
    }
}
