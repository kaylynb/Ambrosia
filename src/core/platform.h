#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

struct SDL_Window;

// What differs between the platforms the programs run on, for the core and the engine. Each
// platform's definitions are in core/desktop or core/switch, of which the build compiles one.
namespace dx::platform {
    // The platforms, for the settings whose default differs between them (core/settings.h).
    enum class Kind { Desktop,
        Switch };

    // The platform the build is for.
    Kind kind();
    // The places to look for the game data, in order, each a directory holding System/DeusEx.u.
    std::vector<std::filesystem::path> gameDataCandidates();
    // The directory for the player's settings and saves when XDG_DATA_HOME gives none.
    std::filesystem::path userDataDirectory();
    // Renames a file onto a path, replacing a file already there.
    void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error);
    // The computer's name as the system gives it, or empty when it has none.
    std::string hostName();
    // Whether the controller's two main face buttons follow Nintendo's convention (see placeOf in
    // engine/gamepad.h) rather than Xbox's.
    bool nintendoFaceButtons();
    // How many threads background work may use when no setting says (workers::threadCap).
    int32_t backgroundThreads();
    // Whether a text field of the debug overlay starts SDL's text input when it is focused.
    bool overlayTextInput();
    // Gives a window the program's icon (assets/icon/<program>): the 32 pixel image for 100
    // percent scale, with 64, 128 and 256 for SDL to pick at higher scales. Not fatal if the
    // window system will not take it, as a Wayland compositor without the xdg_toplevel_icon_v1
    // protocol will not: the log says why, and the window keeps the system's icon. Nothing on the
    // Switch, where the .nro carries the icon.
    void setWindowIcon(SDL_Window* window);

    // Whether standard error has somewhere to go. A file or pipe it was given is always used. A
    // Windows GUI program started from a terminal gets no handles of its own: with attach it
    // takes the terminal's console, or opens a console window of its own when started from
    // Explorer; without, it writes to none, as any GUI program. Elsewhere attach changes nothing.
    bool connectConsole(bool attach);
    // Whether standard error goes to a file or a pipe, as when a script or test harness runs the
    // program, rather than to a terminal or nowhere.
    bool consoleRedirected();
    // Has report called with one line describing a crash, such as a bad memory access, before the
    // program ends. Nothing on the Switch, whose game reports crashes itself (console_report).
    void onCrash(void (*report)(const char* description));
}
