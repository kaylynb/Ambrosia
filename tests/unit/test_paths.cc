#include "test_paths.h"

#include "core/config.h"
#include "core/platform.h"

#include <SDL3/SDL_filesystem.h>

#include <stdexcept>
#include <system_error>

namespace dx::test {
    namespace {
        std::filesystem::path programDirectory()
        {
            const char* base = SDL_GetBasePath();
            if (!base) {
                throw std::runtime_error("cannot find the folder unit-tests is in");
            }
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base)));
        }

        std::string missingGameData()
        {
            std::string message = "game data not found: the tests tagged [data] need the original Deus Ex files. Set DEUSEX_GAME_DATA to the "
                                  "Deus Ex folder (the one holding System/DeusEx.u), or copy its contents to game-data in the checkout. Looked in:";
            for (const std::filesystem::path& candidate : platform::gameDataCandidates()) {
                message += "\n  " + candidate.generic_string();
            }
            return message;
        }
    }

    const std::string& gameData()
    {
        static const std::string root = config::findGameRoot().generic_string();
        if (root.empty()) {
            throw std::runtime_error(missingGameData());
        }
        return root;
    }

    const std::filesystem::path& sourceDir()
    {
        static const std::filesystem::path root = [] {
            std::filesystem::path start = programDirectory().lexically_normal();
            for (std::filesystem::path directory = start;; directory = directory.parent_path()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(directory / "CMakePresets.json", error) && std::filesystem::is_directory(directory / "src", error)) {
                    return directory;
                }
                if (directory.parent_path() == directory || directory.empty()) {
                    return std::filesystem::path();
                }
            }
        }();
        if (root.empty()) {
            throw std::runtime_error("source checkout not found: no folder above " + programDirectory().generic_string() + " holds CMakePresets.json and src");
        }
        return root;
    }

    const std::filesystem::path& uccPath()
    {
#ifdef _WIN32
        static const std::filesystem::path path = programDirectory() / "ucc.exe";
#else
        static const std::filesystem::path path = programDirectory() / "ucc";
#endif
        return path;
    }
}
