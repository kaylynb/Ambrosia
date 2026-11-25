#include "core/platform.h"

namespace dx::platform {
    // One fixed place on the SD card, beside the program in sdmc:/switch/ambrosia.
    std::vector<std::filesystem::path> gameDataCandidates()
    {
        return { "sdmc:/switch/ambrosia/game-data" };
    }

    std::filesystem::path userDataDirectory()
    {
        return "sdmc:/switch/ambrosia/user";
    }

    // The Switch's file system refuses to rename onto an existing file, so when that fails the
    // old file is removed and the rename tried again.
    void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
    {
        std::filesystem::rename(from, to, error);
        std::error_code existsError;
        if (!error || !std::filesystem::is_regular_file(from, existsError) || !std::filesystem::is_regular_file(to, existsError)) {
            return;
        }
        std::error_code removeError;
        if (std::filesystem::remove(to, removeError)) {
            std::filesystem::rename(from, to, error);
        }
    }

    Kind kind()
    {
        return Kind::Switch;
    }

    // libnx has no host name.
    std::string hostName()
    {
        return {};
    }

    bool nintendoFaceButtons()
    {
        return true;
    }

    // Of the Switch's four cores, the one the game's main thread is left.
    int32_t backgroundThreads()
    {
        return 3;
    }

    // The SDL fork's Switch driver opens the system's inline keyboard over the game whenever
    // text input starts, which ImGui does each time one of its fields is focused, such as a
    // filter touched on the way to a button.
    bool overlayTextInput()
    {
        return false;
    }

    // The homebrew menu shows the .nro's own icon (src/CMakeLists.txt).
    void setWindowIcon(SDL_Window*)
    {
    }

    // Standard error goes to the log file, the debug log and nxlink (game/switch/console_report).
    bool connectConsole(bool)
    {
        return true;
    }

    bool consoleRedirected()
    {
        return false;
    }

    void onCrash(void (*)(const char*))
    {
    }
}
