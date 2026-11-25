#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Data derived from the game's levels and kept between runs, such as the raytraced sound mode's
// bakes. It is made from the game's own files, so it cannot ship with Ambrosia: it is made on the
// player's machine and kept in the user data folder, one file per entry (usually a map), in a
// folder per consumer:
//
//     <user data folder>/<consumer name>Cache/<entry><extension>
//
// with the entry's name lowercased and anything but letters, digits, '_' and '-' made '_'.
//
// Each file holds the key it was made for, then the consumer's payload, then a checksum of all
// before it. A file is used only when its key is the one wanted and it is whole; one that is
// stale or damaged is reported and never used, and is replaced when the data is made again.
// Files are written whole beside the entry and moved into place (files::replace), so a file
// that is there is never half written.
//
// The file starts:  char magic[8]; uint32 version; uint32 library; uint64 level;
// uint64 settings[2]; and ends with the uint64 FNV-1a hash of every byte before it.
namespace dx::derivedcache {
    // FNV-1a, 64-bit: the same on every machine for the same bytes.
    struct Hash {
        uint64_t value = 14695981039346656037ull;

        void bytes(const void* data, size_t size)
        {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++) {
                value ^= p[i];
                value *= 1099511628211ull;
            }
        }

        template <typename T>
        void add(const T& v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            bytes(&v, sizeof(v));
        }

        void text(std::string_view s)
        {
            add(static_cast<uint64_t>(s.size()));
            bytes(s.data(), s.size());
        }
    };

    // What a consumer's files are, given once by the consumer.
    struct Consumer {
        // Names its folder, <name>Cache.
        const char* name = "";
        const char* extension = "";
        // The first bytes of each of its files.
        std::array<char, 8> magic {};
        // Why a file is not used, as the reports say it: made by another version of the
        // consumer, or of the library the data comes from; for another level; for other
        // settings, by part; or not one of its files at all.
        const char* otherVersion = "it was made by another version";
        const char* otherLibrary = "it was made with another library version";
        const char* otherLevel = "the level has changed";
        std::array<const char*, 2> otherSettings { "the settings have changed", "the settings have changed" };
        const char* notOurs = "it is not one of these files";
    };

    // Everything a file's data depends on.
    struct Key {
        // The consumer's own version, bumped whenever its layout, or what it makes from the same
        // inputs, changes.
        uint32_t version = 0;
        // The version of a library the data is made by, or 0.
        uint32_t library = 0;
        // A hash of the level's content as the consumer reads it.
        uint64_t level = 0;
        // Hashes of the consumer's settings, in two parts it reports apart (0 for one unused).
        std::array<uint64_t, 2> settings {};

        bool operator==(const Key&) const = default;
    };

    // Why a file with key found is stale for wanted, or empty when it is not.
    std::string staleReason(const Consumer& consumer, const Key& found, const Key& wanted);

    std::filesystem::path directory(const Consumer& consumer);
    std::filesystem::path path(const Consumer& consumer, std::string_view entry);

    enum class State : uint8_t {
        Missing,
        Stale,
        Damaged,
        Valid,
    };

    const char* stateName(State state);

    struct Read {
        State state = State::Missing;
        // Why it is stale or damaged.
        std::string reason;
        // The consumer's payload, when valid.
        std::vector<uint8_t> payload;
    };

    // Reads a file and checks it against the key it should have. The key is checked before the
    // rest, so a file of an older version is stale rather than damaged.
    Read read(const Consumer& consumer, const std::filesystem::path& file, const Key& wanted);
    // Writes a file whole, replacing any before only once the new one is complete.
    bool write(const Consumer& consumer, const std::filesystem::path& file, const Key& key, const std::vector<uint8_t>& payload, std::string* error);
    // Removes a file, true if there was one.
    bool invalidate(const std::filesystem::path& file);
    // Removes every file of the consumer's, and says how many.
    size_t invalidateAll(const Consumer& consumer);
}
