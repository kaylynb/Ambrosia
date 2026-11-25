#pragma once

#include <string>

#ifdef __GNUC__
#define DX_PRINTF_FORMAT __attribute__((format(printf, 1, 2)))
#else
#define DX_PRINTF_FORMAT
#endif

// Start-up reporting on the Switch, where standard error is otherwise invisible. Elsewhere
// these do nothing, and the desktop builds report as they always have.
namespace dx::game::report {
    // Sends standard output and errors to sdmc:/switch/ambrosia/log.txt (started afresh), to the
    // debug log, and to the PC when started through nxlink; logs a crash's registers and an
    // unhandled exception; and notes the memory the program has and how it was started. On the
    // desktop, logs to Ambrosia.log, and to a console only with console (--console, core/log.h).
    void start(bool console);
    // One line in the log for a start-up step, such as "SDL initialised".
    void stage(const char* format, ...) DX_PRINTF_FORMAT;
    // Logs the message and shows it on screen, returning once the player has seen it.
    void fatal(const std::string& message);
    // Started as an applet (from the album), with too little memory to run the game.
    bool appletMode();
    // Called after each frame; the first one is logged, and after it the log is written out less
    // often so a busy log does not slow the game.
    void frameDone();
}
