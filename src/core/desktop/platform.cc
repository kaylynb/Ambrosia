#include "core/platform.h"

#ifndef AMBROSIA_DEV_BUILD
#include "core/steam.h"
#endif

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dx::platform {
    namespace {
#ifndef AMBROSIA_DEV_BUILD
        // An environment variable as a path, or an empty path when it is unset or empty.
        std::filesystem::path environmentPath(const char* name)
        {
#ifdef _WIN32
            std::wstring wide(name, name + std::char_traits<char>::length(name));
            const wchar_t* value = _wgetenv(wide.c_str());
            return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#else
            const char* value = std::getenv(name);
            return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#endif
        }

        // The game-data folders in the system's places for program data: on Windows
        // %LOCALAPPDATA%\Ambrosia and then %PROGRAMDATA%\Ambrosia; elsewhere
        // $XDG_DATA_HOME/ambrosia (~/.local/share/ambrosia when unset) and then
        // <dir>/ambrosia for each folder of $XDG_DATA_DIRS (/usr/local/share:/usr/share when unset).
        void addDataFolders(std::vector<std::filesystem::path>& candidates)
        {
#ifdef _WIN32
            for (const char* name : { "LOCALAPPDATA", "PROGRAMDATA" }) {
                std::filesystem::path base = environmentPath(name);
                if (!base.empty()) {
                    candidates.push_back(base / "Ambrosia" / "game-data");
                }
            }
#else
            std::filesystem::path home = environmentPath("XDG_DATA_HOME");
            if (home.empty()) {
                std::filesystem::path user = environmentPath("HOME");
                if (!user.empty()) {
                    home = user / ".local" / "share";
                }
            }
            if (!home.empty()) {
                candidates.push_back(home / "ambrosia" / "game-data");
            }
            const char* dirs = std::getenv("XDG_DATA_DIRS");
            std::string_view list = dirs && *dirs ? dirs : "/usr/local/share:/usr/share";
            while (!list.empty()) {
                size_t colon = list.find(':');
                std::string_view dir = list.substr(0, colon);
                // The spec says a relative entry is to be ignored.
                if (!dir.empty() && dir.front() == '/') {
                    candidates.push_back(std::filesystem::path(dir) / "ambrosia" / "game-data");
                }
                list = colon == std::string_view::npos ? std::string_view() : list.substr(colon + 1);
            }
#endif
        }

        // Where Steam itself may be installed: on Windows the folder its SteamPath registry value
        // names, then its default folder; elsewhere its usual folders, the Flatpak's last.
        std::vector<std::filesystem::path> steamRoots()
        {
            std::vector<std::filesystem::path> roots;
#ifdef _WIN32
            wchar_t value[MAX_PATH * 2] = {};
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0]) {
                roots.push_back(std::filesystem::path(value).lexically_normal());
            }
            std::filesystem::path programs = environmentPath("ProgramFiles(x86)");
            if (!programs.empty()) {
                roots.push_back(programs / "Steam");
            }
#else
            std::filesystem::path home = environmentPath("HOME");
            std::filesystem::path data = environmentPath("XDG_DATA_HOME");
            if (data.empty() && !home.empty()) {
                data = home / ".local" / "share";
            }
            if (!data.empty()) {
                roots.push_back(data / "Steam");
            }
            if (!home.empty()) {
                roots.push_back(home / ".steam" / "steam");
                roots.push_back(home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam");
            }
#endif
            return roots;
        }

        // The game's folder in each library of each Steam install, each once.
        void addSteamFolders(std::vector<std::filesystem::path>& candidates)
        {
            std::vector<std::filesystem::path> seen;
            for (const std::filesystem::path& root : steamRoots()) {
                for (const std::filesystem::path& library : steam::libraries(root)) {
                    std::filesystem::path folder = steam::gameFolder(library).lexically_normal();
                    if (std::find(seen.begin(), seen.end(), folder) == seen.end()) {
                        seen.push_back(folder);
                        candidates.push_back(folder);
                    }
                }
            }
        }
#endif
    }

    // DEUSEX_GAME_DATA, then a game-data folder beside the program. A development build then
    // looks in every folder above the program, so a build under build/<preset>/bin finds the
    // checkout's game-data without a path compiled in, and nowhere else: not the system's data
    // folders nor Steam, so a missing or broken checkout copy is an error rather than a quiet
    // switch to an installed (and perhaps modded) copy. A shipped build looks in the system's
    // data folders (addDataFolders) and then in Steam's libraries.
    std::vector<std::filesystem::path> gameDataCandidates()
    {
        std::vector<std::filesystem::path> candidates;
        if (const char* value = std::getenv("DEUSEX_GAME_DATA")) {
            candidates.emplace_back(value);
        }
        if (const char* base = SDL_GetBasePath()) {
            std::filesystem::path directory = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base))).lexically_normal();
            if (!directory.has_filename()) {
                directory = directory.parent_path();
            }
#ifndef AMBROSIA_DEV_BUILD
            candidates.push_back(directory / "game-data");
#else
            for (;;) {
                candidates.push_back(directory / "game-data");
                std::filesystem::path parent = directory.parent_path();
                if (parent == directory || parent.empty()) {
                    break;
                }
                directory = parent;
            }
#endif
        }
#ifndef AMBROSIA_DEV_BUILD
        addDataFolders(candidates);
        addSteamFolders(candidates);
#endif
        return candidates;
    }

    // %LOCALAPPDATA%\Ambrosia on Windows, ~/.local/share/ambrosia elsewhere.
    std::filesystem::path userDataDirectory()
    {
        std::error_code error;
#ifdef _WIN32
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        return local && *local ? std::filesystem::path(local) / "Ambrosia" : std::filesystem::temp_directory_path(error) / "Ambrosia";
#else
        const char* home = std::getenv("HOME");
        return (home && *home ? std::filesystem::path(home) / ".local" / "share" : std::filesystem::temp_directory_path(error)) / "ambrosia";
#endif
    }

    // A failed rename stays failed: on Windows it means the file is in use.
    void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
    {
        std::filesystem::rename(from, to, error);
    }

    Kind kind()
    {
        return Kind::Desktop;
    }

    std::string hostName()
    {
        char host[256] = {};
#ifdef _WIN32
        if (const char* name = std::getenv("COMPUTERNAME")) {
            std::snprintf(host, sizeof(host), "%s", name);
        }
#else
        gethostname(host, sizeof(host) - 1);
#endif
        return host;
    }

    bool nintendoFaceButtons()
    {
        return false;
    }

    // All but one of the machine's threads, which the game's main thread keeps.
    int32_t backgroundThreads()
    {
        return std::max(1, static_cast<int32_t>(std::thread::hardware_concurrency()) - 1);
    }

    // Typing reaches the overlay's fields only while SDL's text input is on.
    bool overlayTextInput()
    {
        return true;
    }
}
