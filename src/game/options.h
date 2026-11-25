#pragma once

#include "core/command_line.h"

#include <optional>
#include <string>

namespace dx::game {
    struct Options : dx::cli::RunOptions {
        Options()
        {
            width = 1280;
            height = 720;
        }

        // The map to start on, instead of the game's own start.
        std::string map;
        // Plays sound to a device that discards it, so the mixer runs on its own thread as it
        // does with a sound card but nothing is heard; with hidden too, instead of not mixing.
        // For profiling the mixer in automated runs.
        bool nullSound = false;
        // Opens the developer overlay (DebugOverlay in the console) from the first frame.
        bool debugOverlay = false;
        // Investigation: seeds every random stream with this instead of the clock, so two runs
        // (or two builds) can be compared with the worldhash test action.
        std::optional<int> seed;
        // For measuring: removes the GL driver's shader cache before the window is made, where
        // the platform has one the game can clear (platform::clearShaderCache).
        bool clearShaderCache = false;
        // For inspecting levels: loads every level without its mission script
        // (deusex::skipMissionScripts), so 00_Intro and the 99_Endgame maps play no cutscene.
        bool noMissionScript = false;
        // Loads every level without its triggers (deusex::skipTriggers).
        bool noTriggers = false;
        // Writes each options screen's layout, as it ends up after the startup changes to the
        // menus, and the backdrop generated for it into this directory (deusex::dumpMenuLayouts).
        std::string dumpMenuLayouts;
    };

    // Reads the command line into options. Nothing when the game should start; otherwise the code
    // to exit with (cli::parse).
    std::optional<int> parseOptions(int argc, char** argv, Options& options);
}
