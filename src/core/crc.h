#pragma once

#include <array>
#include <cstdint>

namespace dx {
    // Unreal's CRC table (GCRCTable): the CRC-32 polynomial 0x04C11DB7 worked most significant
    // bit first, which name hashes and script CRCs are built from.
    inline constexpr std::array<uint32_t, 256> crcTable = [] {
        std::array<uint32_t, 256> table {};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t value = i << 24;
            for (int bit = 0; bit < 8; bit++) {
                value = (value & 0x80000000u) ? (value << 1) ^ 0x04C11DB7u : value << 1;
            }
            table[i] = value;
        }
        return table;
    }();
}
