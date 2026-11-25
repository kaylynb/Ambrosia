#include "game/console_report.h"

#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"

#include <SDL3/SDL_messagebox.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// The desktop builds log to Ambrosia.log in the user directory, and to the console they were
// started from, if any (core/log.h).
namespace dx::game::report {
    void start(bool console)
    {
        log::startProgram(config::userDirectory() / "Ambrosia.log", console);
    }

    void stage(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        va_list copy;
        va_copy(copy, arguments);
        int length = std::vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        std::string text(length > 0 ? static_cast<size_t>(length) : 0, '\0');
        if (length > 0) {
            std::vsnprintf(text.data(), text.size() + 1, format, arguments);
        }
        va_end(arguments);
        log::info("Init", "{}", text);
    }

    // A message box too, unless a script or harness reads standard error: a player who started
    // the game from Explorer has no console to see the error in.
    void fatal(const std::string& message)
    {
        log::error("Critical", "{}", message);
        log::flush();
        if (platform::consoleRedirected()) {
            return;
        }
        std::string text = message;
        std::filesystem::path file = log::filePath();
        if (!file.empty()) {
            text += "\n\nThe log is in " + file.string();
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ambrosia", text.c_str(), nullptr);
    }

    bool appletMode()
    {
        return false;
    }

    void frameDone()
    {
    }
}
