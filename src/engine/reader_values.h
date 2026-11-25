#pragma once

#include "core/reader.h"
#include "engine/resources.h"

#include <stdexcept>
#include <string>

// Readers for the values mesh and resource exports share. For the engine's own .cc files.
namespace dx::readers {
    // An array's length, as a compact index; a negative one throws, naming what was read when
    // the caller gives it. So does one with more elements than bytes left: every element these
    // arrays hold takes at least a byte, so a corrupt count fails here rather than after its
    // caller has sized a vector for up to 2^31 elements.
    inline size_t readCount(Reader& reader, const char* what = nullptr)
    {
        int32_t count = reader.index();
        if (count < 0) {
            throw std::runtime_error(what ? std::string("negative array count in ") + what : std::string("negative array count"));
        }
        if (static_cast<size_t>(count) > reader.remaining()) {
            throw std::runtime_error(what ? std::string("array count past the end of ") + what : std::string("array count past the end of the data"));
        }
        return static_cast<size_t>(count);
    }

    inline Vector readVector(Reader& reader)
    {
        Vector value;
        value.x = reader.f32();
        value.y = reader.f32();
        value.z = reader.f32();
        return value;
    }

    inline BoundingBox readBox(Reader& reader)
    {
        BoundingBox box;
        box.min = readVector(reader);
        box.max = readVector(reader);
        box.valid = reader.u8() != 0;
        return box;
    }
}
