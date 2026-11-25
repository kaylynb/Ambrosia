#pragma once

#include <optional>
#include <string>
#include <vector>

// CLI11 (vendor/CLI11-*), included only where a program builds its command line.
namespace CLI {
    class App;
}

namespace dx::cli {
    // The options the game and the editor share, for automated runs and tests.
    struct RunOptions {
        // Stops after this many frames; -1 runs until the program is closed or a quit step.
        int frames = -1;
        int width = 0;
        int height = 0;
        // Whether --width or --height was given, rather than the program's default.
        bool sizeGiven = false;
        // No visible window, so a run does not disturb whoever is using the desktop; the game also
        // plays no sound and leaves the mouse alone. Screenshots still work.
        bool hidden = false;
        // The steps of --do and --script, in the order given, for a steps::Runner.
        std::vector<std::string> steps;

        // A run that ends itself, by --frames or a quit step: each frame is 1/30 s of game time,
        // run as fast as it goes without vsync or the frame limit, at the size the command line
        // gives rather than the one saved.
        bool automated() const;
    };

    // Adds --frames, --width, --height, --hidden, --do and --script to app, read into options.
    // Set options' width and height to the program's default size first.
    void addRunOptions(CLI::App& app, RunOptions& options);
    // Handle a standalone --version before opening (and rotating) the user's log.
    bool printVersion(int argc, char** argv);

    // Adds --console, then parses the command line. Nothing when the program should go on;
    // otherwise the code it exits with: 0 after --help has listed the options, 2 once the log has
    // said what is wrong.
    std::optional<int> parse(CLI::App& app, int argc, char** argv);
}
