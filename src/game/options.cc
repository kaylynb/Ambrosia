#include "game/options.h"

#include <CLI11.hpp>

namespace dx::game {
    std::optional<int> parseOptions(int argc, char** argv, Options& options)
    {
        CLI::App app("Ambrosia, the Deus Ex engine reimplemented. The options are for development; see BUILD.md.", "ambrosia");
        app.add_option("map", options.map, "Map to start on, such as 00_Training");
        dx::cli::addRunOptions(app, options);
        app.add_option("--seed", options.seed, "Seed every random stream with N instead of the clock, for comparing runs")
            ->type_name("N")
            ->check(CLI::NonNegativeNumber)
            ->group("Automated runs");
        const char* inspection = "Level inspection";
        app.add_flag("--no-mission-script", options.noMissionScript, "Load each level without its mission script and cutscene camera points")->group(inspection);
        app.add_flag("--no-triggers", options.noTriggers, "Load each level without its triggers, and give no actor conversations")->group(inspection);
        const char* diagnostics = "Diagnostics";
        app.add_flag("--debug-overlay", options.debugOverlay, "Open the developer overlay from the first frame")->group(diagnostics);
        app.add_flag("--null-sound", options.nullSound, "Mix sound to a device that discards it, for profiling the mixer")->group(diagnostics);
        app.add_flag("--clear-shader-cache", options.clearShaderCache, "Remove the GL driver's shader cache before the window is made, where the game can")->group(diagnostics);
        app.add_option("--dump-menu-layouts", options.dumpMenuLayouts, "Write each options screen's layout and generated backdrop into DIR")
            ->type_name("DIR")
            ->group(diagnostics);
        return dx::cli::parse(app, argc, argv);
    }
}
