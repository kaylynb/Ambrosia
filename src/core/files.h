#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace dx::files {
    // Moves a file onto another path, replacing a file already there, as rename does on Windows
    // and POSIX systems. The Switch's file system refuses to rename onto an existing file, so
    // there, when that fails, the old file is removed and the rename tried again.
    void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error);
    // Copies a file's bytes, replacing any file at the destination. On the Switch,
    // std::filesystem::copy_file leaves an empty file behind.
    bool copy(const std::filesystem::path& from, const std::filesystem::path& to);
    // A whole file's bytes, read in binary mode, or nothing when it cannot be opened. A file
    // that opens but holds nothing reads as an empty string.
    std::optional<std::string> readFile(const std::filesystem::path& path);
    // Writes a whole file in binary mode, replacing any file there and making the folders it is
    // in; false when it cannot be written.
    bool writeFile(const std::filesystem::path& path, std::string_view bytes);
    bool writeFile(const std::filesystem::path& path, std::span<const uint8_t> bytes);
}
