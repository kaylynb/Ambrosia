#include "core/derived_cache.h"

#include "core/config.h"
#include "core/files.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dx::derivedcache {
    namespace {
        struct Envelope {
            char magic[8];
            uint32_t version;
            uint32_t library;
            uint64_t level;
            uint64_t settings[2];
        };

        static_assert(sizeof(Envelope) == 40);
    }

    std::string staleReason(const Consumer& consumer, const Key& found, const Key& wanted)
    {
        if (found.version != wanted.version) {
            return consumer.otherVersion;
        }
        if (found.library != wanted.library) {
            return consumer.otherLibrary;
        }
        if (found.level != wanted.level) {
            return consumer.otherLevel;
        }
        for (size_t i = 0; i < found.settings.size(); i++) {
            if (found.settings[i] != wanted.settings[i]) {
                return consumer.otherSettings[i];
            }
        }
        return {};
    }

    std::filesystem::path directory(const Consumer& consumer)
    {
        return config::userDirectory() / (std::string(consumer.name) + "Cache");
    }

    std::filesystem::path path(const Consumer& consumer, std::string_view entry)
    {
        std::string name;
        for (char c : entry) {
            name += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
        }
        return directory(consumer) / (name + consumer.extension);
    }

    const char* stateName(State state)
    {
        switch (state) {
        case State::Missing:
            return "missing";
        case State::Stale:
            return "stale";
        case State::Damaged:
            return "damaged";
        case State::Valid:
            return "valid";
        }
        return "";
    }

    Read read(const Consumer& consumer, const std::filesystem::path& file, const Key& wanted)
    {
        Read read;
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) {
            return read;
        }
        auto damaged = [&](const char* why) {
            read.state = State::Damaged;
            read.reason = why;
            return read;
        };
        std::string contents = files::readFile(file).value_or(std::string());
        std::vector<uint8_t> bytes(contents.begin(), contents.end());
        if (bytes.size() < sizeof(Envelope) + sizeof(uint64_t)) {
            return damaged("it is too short");
        }
        Envelope envelope {};
        std::memcpy(&envelope, bytes.data(), sizeof(envelope));
        if (std::memcmp(envelope.magic, consumer.magic.data(), sizeof(envelope.magic)) != 0) {
            return damaged(consumer.notOurs);
        }
        Key found;
        found.version = envelope.version;
        found.library = envelope.library;
        found.level = envelope.level;
        found.settings = { envelope.settings[0], envelope.settings[1] };
        if (std::string reason = staleReason(consumer, found, wanted); !reason.empty()) {
            read.state = State::Stale;
            read.reason = reason;
            return read;
        }
        Hash check;
        check.bytes(bytes.data(), bytes.size() - sizeof(uint64_t));
        uint64_t stored = 0;
        std::memcpy(&stored, bytes.data() + bytes.size() - sizeof(uint64_t), sizeof(stored));
        if (stored != check.value) {
            return damaged("its checksum does not match");
        }
        read.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(Envelope)), bytes.end() - static_cast<std::ptrdiff_t>(sizeof(uint64_t)));
        read.state = State::Valid;
        return read;
    }

    bool write(const Consumer& consumer, const std::filesystem::path& file, const Key& key, const std::vector<uint8_t>& payload, std::string* error)
    {
        Envelope envelope {};
        std::memcpy(envelope.magic, consumer.magic.data(), sizeof(envelope.magic));
        envelope.version = key.version;
        envelope.library = key.library;
        envelope.level = key.level;
        envelope.settings[0] = key.settings[0];
        envelope.settings[1] = key.settings[1];
        std::vector<uint8_t> bytes(sizeof(envelope));
        std::memcpy(bytes.data(), &envelope, sizeof(envelope));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        Hash check;
        check.bytes(bytes.data(), bytes.size());
        const auto* sum = reinterpret_cast<const uint8_t*>(&check.value);
        bytes.insert(bytes.end(), sum, sum + sizeof(check.value));

        std::error_code code;
        std::filesystem::create_directories(file.parent_path(), code);
        std::filesystem::path partial = file;
        partial += ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                if (error) {
                    *error = "it could not be written to " + partial.string();
                }
                return false;
            }
        }
        files::replace(partial, file, code);
        if (code) {
            std::filesystem::remove(partial, code);
            if (error) {
                *error = "it could not be moved into place at " + file.string();
            }
            return false;
        }
        return true;
    }

    bool invalidate(const std::filesystem::path& file)
    {
        std::error_code error;
        return std::filesystem::remove(file, error);
    }

    size_t invalidateAll(const Consumer& consumer)
    {
        size_t removed = 0;
        std::error_code error;
        std::vector<std::filesystem::path> found;
        for (const auto& item : std::filesystem::directory_iterator(directory(consumer), error)) {
            if (item.is_regular_file(error) && item.path().extension() == consumer.extension) {
                found.push_back(item.path());
            }
        }
        for (const std::filesystem::path& file : found) {
            if (invalidate(file)) {
                removed++;
            }
        }
        return removed;
    }
}
