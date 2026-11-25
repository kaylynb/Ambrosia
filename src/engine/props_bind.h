#pragma once

#include "core/object.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace dx {
    // Offset of a property that is not bound: an optional property the loaded
    // package lacks, or any property before binding. The generated accessor
    // of an optional property returns nullptr for it.
    inline constexpr size_t PropAbsent = std::numeric_limits<size_t>::max();

    // Reads an optional property through its pointer accessor, or fallback
    // when the loaded package lacks it.
    template <typename T>
    T propOr(const T* value, std::type_identity_t<T> fallback)
    {
        return value ? *value : fallback;
    }

    // One generated bind entry: what the C++ accessor assumes about the
    // property. Binding compares it with the property actually loaded.
    struct PropBinding {
        const char* name;
        size_t* offset;
        PropType type;
        size_t elementSize; // 0 when the accessor does not depend on it (raw struct)
        int32_t arrayDim;
        const char* structName; // struct properties only
        bool optional;
    };

    struct ClassBinding {
        const char* path;
        const PropBinding* properties;
        size_t count;
    };

    // Looks up every property and writes its offset (PropAbsent for a missing
    // optional one). On any missing class, missing required property or
    // mismatch it appends one line per failure, writes no offsets and
    // returns false, so earlier offsets stay as they were. Re-runnable.
    bool bindClasses(const ClassBinding* classes, size_t count, std::vector<std::string>& failures);

    const char* propTypeName(PropType type);

    // Binds the generated tables of every props_*.cc file, all or nothing.
    bool bindAllProps(std::vector<std::string>& failures);

    // True once bindAllProps has succeeded (engine::initialize does it).
    bool propsBound();
}
