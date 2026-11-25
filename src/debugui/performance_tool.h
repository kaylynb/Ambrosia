#pragma once

#include "debugui/overlay.h"

#include <string>

namespace dx::debugui {
    // The Performance tool: the built-in frame recorder's frame times, zones, timers and plots,
    // and CSV files of them in the user data folder. Its console switch and commands:
    //   DebugOverlay perf-record on|off       records, with or without the tool open
    //   DebugOverlay perf-reset now           forgets the history and totals
    //   DebugOverlay perf-csv <file|default>  writes what has been recorded
    //   DebugOverlay perf-record-for <N|Nf> [name]
    //                                         records N seconds (or N frames), then writes a file,
    //                                         name.csv if given (the console lowercases it)
    //   DebugOverlay gpu-timers on|off        GPU timer queries per stage as plots (render/gpu_timers.h)
    // Files without a folder go in the user data folder's Profiles folder.
    void addPerformanceTool(Overlay& overlay);

    // Where a CSV named name goes: an absolute path as it is, else the Profiles folder; with no
    // name, the next free FramesNNNN.csv there.
    std::string performanceFile(const std::string& name);
}
