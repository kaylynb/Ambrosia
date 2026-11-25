#include "game/platform.h"

#include "core/config.h"
#include "game/console_report.h"
#include "game/switch/runner.h"

#include <switch.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace dx::game::platform {
    namespace {
        // The runner's path when the Ambrosia runner started the game, and empty otherwise.
        std::string runnerPath;
    }

    std::optional<int> start(int& argc, char**& argv)
    {
        // Standard output and errors go to the log file and the debug log (see console_report).
        report::start(false);
        static std::vector<std::string> fileArgs;
        static std::vector<char*> switchArgv;
        // Started by the Ambrosia runner (src/switch-runner), which passes the options itself, after
        // --ambrosia-runner and its own path: the loader is told to start the runner again when the
        // game exits, which it does however the game exits short of a crash, and the console is
        // kept awake while nobody touches it.
        if (argc >= 3 && std::string(argv[1]) == "--ambrosia-runner") {
            runnerPath = argv[2];
            std::string next = "\"" + runnerPath + "\" --after-job";
            Result rc = envSetNextLoad(runnerPath.c_str(), next.c_str());
            report::stage("started by the runner, which %s", R_SUCCEEDED(rc) ? ("starts again on exit: " + runnerPath).c_str() : "cannot be started again (envSetNextLoad failed)");
            report::setUnattended();
            appletSetAutoSleepDisabled(true);
            switchArgv.assign(argv, argv + 1);
            switchArgv.insert(switchArgv.end(), argv + 3, argv + argc);
        } else {
            // The homebrew menu passes no options, so they are read from a file instead,
            // separated by white space, with quotes around one that has spaces. A loader may also
            // pass no program name. The new argv points into these, which last until the program
            // ends.
            if (argc == 0) {
                fileArgs.emplace_back("ambrosia");
            }
            std::ifstream argsFile("sdmc:/switch/ambrosia/args.txt");
            for (std::string word; argsFile >> std::quoted(word);) {
                fileArgs.push_back(word);
            }
            switchArgv.assign(argv, argv + argc);
        }
        for (std::string& word : fileArgs) {
            switchArgv.push_back(word.data());
        }
        switchArgv.push_back(nullptr);
        argc = static_cast<int>(switchArgv.size()) - 1;
        argv = switchArgv.data();
        std::string shown;
        for (int i = 0; i < argc; i++) {
            shown += std::string(i ? " " : "") + argv[i];
        }
        report::stage("arguments: %s", shown.c_str());
        if (report::appletMode()) {
            report::fatal("Ambrosia was started from the album, which leaves it too little memory. Start it from a game instead: hold R while starting any game, then choose Ambrosia in the homebrew menu.");
            return 1;
        }
        std::filesystem::path root = config::findGameRoot();
        bool found = !root.empty();
        report::stage("game data: sdmc:/switch/ambrosia/game-data/System/DeusEx.u %s", found ? "found" : "not found");
        if (!found) {
            // As main's other early exits: in the log and on screen.
            std::string message = "The game data was not found. Copy the Deus Ex folder's contents (System, Maps, Textures and the others) to sdmc:/switch/ambrosia/game-data, so that sdmc:/switch/ambrosia/game-data/System/DeusEx.u exists.";
            report::fatal(message);
            return 1;
        }
        report::stage("user data: %s", config::userDirectory().string().c_str());
        return std::nullopt;
    }

    void reportInvalidOptions()
    {
        if (!runnerPath.empty()) {
            report::fatal("The options the runner passed are not valid; see sdmc:/switch/ambrosia/log.txt.");
            return;
        }
        report::fatal("The options in sdmc:/switch/ambrosia/args.txt are not valid; see sdmc:/switch/ambrosia/log.txt.");
    }

    // SDL's text input would open the on-screen keyboard; text fields open the system keyboard
    // instead (system_keyboard.h).
    void startTextInput(SDL_Window*)
    {
    }

    bool onScreenKeyboard()
    {
        return false;
    }

    void editTextAt(SDL_Window*, const SDL_Rect*)
    {
    }

    // The window is the whole screen at the size it was created with, which the system scales
    // to the display; resizing it leaves the frame off-centre in docked mode.
    bool setVideoMode(SDL_Window*, bool, bool, int, int)
    {
        return false;
    }

    // Read through libnx because the SDL3 fork's touch events never report a finger landing.
    // In window pixels, which are the screen's 1280x720; none when docked.
    void readTouch(TouchCursor& touch)
    {
        static bool initialized = false;
        if (!initialized) {
            hidInitializeTouchScreen();
            initialized = true;
        }
        std::vector<Finger> down;
        HidTouchScreenState state {};
        if (hidGetTouchScreenStates(&state, 1)) {
            for (s32 i = 0; i < state.count && i < 16; i++) {
                const HidTouchState& finger = state.touches[i];
                down.push_back({ static_cast<int64_t>(finger.finger_id), static_cast<float>(finger.x), static_cast<float>(finger.y) });
            }
        }
        touch.update(down);
    }

    namespace {
        // Removes a folder and everything in it, counting what it removed; on failure, failure
        // names the path and the error. Not std::filesystem::remove_all, which removes entries
        // while their folder is still open for reading, after which the Switch's file system
        // fails the folder's next read with an I/O error: here each folder's entries are read in
        // full and the folder closed before any of them is removed.
        std::uintmax_t removeTree(const std::filesystem::path& folder, std::string& failure)
        {
            std::error_code error;
            std::vector<std::filesystem::path> entries;
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                entries.push_back(it->path());
            }
            if (error) {
                failure = folder.string() + ": " + error.message();
                return 0;
            }
            std::uintmax_t removed = 0;
            for (const std::filesystem::path& entry : entries) {
                if (std::filesystem::is_directory(entry, error)) {
                    removed += removeTree(entry, failure);
                    if (!failure.empty()) {
                        return removed;
                    }
                } else if (std::filesystem::remove(entry, error)) {
                    removed++;
                } else {
                    failure = entry.string() + ": " + (error ? error.message() : std::string("not removed"));
                    return removed;
                }
            }
            if (!std::filesystem::remove(folder, error)) {
                failure = folder.string() + ": " + (error ? error.message() : std::string("not removed"));
                return removed;
            }
            return removed + 1;
        }
    }

    void clearShaderCache()
    {
        // The newer Mesa's cache (BUILD.md, "A newer Mesa on the Switch"); devkitPro's Mesa keeps
        // none. Done before the GL driver starts, so none of its files is open.
        const std::filesystem::path cache = "sdmc:/.mesa";
        std::error_code error;
        if (!std::filesystem::exists(cache, error)) {
            report::stage("shader cache: sdmc:/.mesa not there");
            return;
        }
        std::string failure;
        std::uintmax_t removed = removeTree(cache, failure);
        if (failure.empty()) {
            report::stage("shader cache: sdmc:/.mesa removed, %llu entries", static_cast<unsigned long long>(removed));
        } else {
            report::stage("shader cache: sdmc:/.mesa not removed (%llu entries were): %s", static_cast<unsigned long long>(removed), failure.c_str());
        }
    }

    const MenuOptions& menuOptions()
    {
        return menuOptionsFor(Platform::Switch);
    }
}
