#include "core/files.h"

#include "core/platform.h"

#include <fstream>
#include <iterator>
#include <vector>

namespace dx::files {
    void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
    {
        platform::replaceFile(from, to, error);
    }

    bool copy(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        std::ifstream in(from, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        std::vector<char> chunk(1 << 16);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            out.write(chunk.data(), in.gcount());
        }
        out.close();
        return !in.bad() && !out.fail();
    }

    std::optional<std::string> readFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool writeFile(const std::filesystem::path& path, std::string_view bytes)
    {
        if (path.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }

    bool writeFile(const std::filesystem::path& path, std::span<const uint8_t> bytes)
    {
        return writeFile(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
}
