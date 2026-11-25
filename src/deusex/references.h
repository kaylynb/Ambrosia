#pragma once

#include "core/gc.h"
#include "core/object.h"

#include <map>
#include <type_traits>
#include <unordered_map>

namespace dx::deusex {
    // Whether a cache key names an object that is going: a pointer key, or either half of a
    // pair. Keys that are not objects, such as sizes and names, never go.
    template <typename T>
    bool keyReleased(const T& key, const gc::ReleasedSet& released)
    {
        if constexpr (std::is_pointer_v<T>) {
            return released.count(key) != 0;
        } else if constexpr (requires { key.first; key.second; }) {
            return keyReleased(key.first, released) || keyReleased(key.second, released);
        } else {
            return false;
        }
    }

    // One object made at run time that only C++ code knows, such as a class found later by its
    // name. Held strongly, so a collection keeps it; None if something deletes it anyway.
    template <typename T>
    class HeldObject final : public gc::Holder {
    public:
        T* object = nullptr;

        void addReferences(ReferenceVisitor& references) override
        {
            references(object);
        }
    };

    // A cache whose values are objects made at run time, such as textures drawn from code.
    // Nothing else may refer to them, so the cache keeps them alive. An entry is dropped, to
    // be made again when next asked for, when the collector sets its value to None or when an
    // object in its key goes.
    template <typename Map>
    class CachedObjects final : public gc::Holder {
    public:
        // With filledAtOnce, every entry is dropped when one goes, so the whole cache is filled
        // again.
        explicit CachedObjects(bool filledAtOnce = false)
            : whole(filledAtOnce)
        {
        }

        Map entries;

        void addReferences(ReferenceVisitor& references) override
        {
            bool dropped = false;
            for (auto entry = entries.begin(); entry != entries.end();) {
                bool held = entry->second != nullptr;
                references(entry->second);
                if (held && !entry->second) {
                    dropped = true;
                    entry = entries.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (dropped && whole) {
                entries.clear();
            }
        }

        void objectsReleased(const gc::ReleasedSet& released) override
        {
            size_t before = entries.size();
            std::erase_if(entries, [&](const auto& entry) {
                return keyReleased(entry.first, released) || released.count(entry.second) != 0;
            });
            if (entries.size() != before && whole) {
                entries.clear();
            }
        }

    private:
        bool whole;
    };
}
