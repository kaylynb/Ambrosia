#include "core/command_line.h"

#include "core/files.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/run_steps.h"
#include "core/version.h"

#include <CLI11.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>

namespace dx::cli {
    namespace {
        CLI::Validator quiet(CLI::Validator validator)
        {
            // The option's own description says what it takes; the check's would follow its N.
            return validator.description("");
        }
    }

    bool RunOptions::automated() const
    {
        return frames >= 0 || std::ranges::any_of(steps, [](const std::string& step) { return dx::steps::isQuit(step); });
    }

    void addRunOptions(CLI::App& app, RunOptions& options)
    {
        const char* group = "Automated runs";
        app.add_option("--frames", options.frames, "Stop after N frames, each 1/30 s of game time, run as fast as they go")->type_name("N")->check(quiet(CLI::NonNegativeNumber))->group(group);
        app.add_option_function<int>("--width", [&options](int width) { options.width = width; options.sizeGiven = true; }, "Window width")->type_name("N")->check(quiet(CLI::PositiveNumber))->group(group);
        app.add_option_function<int>("--height", [&options](int height) { options.height = height; options.sizeGiven = true; }, "Window height")->type_name("N")->check(quiet(CLI::PositiveNumber))->group(group);
        app.add_flag("--hidden", options.hidden, "Run without showing the window; the game also plays no sound and leaves the mouse alone")->group(group);

        // Each value as given, the files read once parsing is done: the steps keep the order of
        // the --do and --script options on the command line, which CLI11 knows then.
        struct Sources {
            std::vector<std::string> inline_;
            std::vector<std::string> files;
        };

        auto sources = std::make_shared<Sources>();
        // One value each time either is given, so a map name after one is not taken as another.
        CLI::Option* inlineOption = app.add_option("--do", sources->inline_, "Steps to run, separated by semicolons, such as \"wait 30; shot out.bmp; quit\"")
                                        ->type_name("STEPS")
                                        ->allow_extra_args(false)
                                        ->group(group);
        CLI::Option* fileOption = app.add_option("--script", sources->files, "A file of steps to run, one a line, with # comments")->type_name("FILE")->allow_extra_args(false)->group(group);
        app.final_callback([&app, &options, sources, inlineOption, fileOption] {
            size_t nextInline = 0;
            size_t nextFile = 0;
            for (CLI::Option* option : app.parse_order()) {
                std::vector<std::string> steps;
                if (option == inlineOption) {
                    steps = dx::steps::splitInline(sources->inline_.at(nextInline++));
                } else if (option == fileOption) {
                    const std::string& file = sources->files.at(nextFile++);
                    // The arguments are UTF-8 (SDL_main gives them so on Windows too).
                    std::optional<std::string> text = dx::files::readFile(std::filesystem::path(std::u8string(file.begin(), file.end())));
                    if (!text) {
                        throw CLI::ValidationError("--script", "cannot read " + file);
                    }
                    steps = dx::steps::splitLines(*text);
                } else {
                    continue;
                }
                for (std::string& step : steps) {
                    if (std::optional<std::string> problem = dx::steps::check(step)) {
                        throw CLI::ValidationError(option->get_name(), *problem);
                    }
                    options.steps.push_back(std::move(step));
                }
            }
        });
    }

    bool printVersion(int argc, char** argv)
    {
        if (argc != 2 || std::string_view(argv[1]) != "--version") {
            return false;
        }
        platform::connectConsole(true);
        std::puts(version::build());
        return true;
    }

    std::optional<int> parse(CLI::App& app, int argc, char** argv)
    {
        app.set_version_flag("--version", version::build());
        // Read before this, when the log starts (log::consoleRequested); added last, so it is at
        // the end of the list with the program's own diagnostics.
        app.add_flag("--console", "On Windows, also write the log to the terminal the program was started from, or a console window of its own")
            ->group("Diagnostics");
        try {
            app.parse(argc, argv);
            return std::nullopt;
        } catch (const CLI::ParseError& error) {
            // --help is a ParseError too, with exit code 0. Its list is written as it is rather
            // than as log lines, to the terminal --help has connected (log::consoleRequested).
            if (error.get_exit_code() == 0) {
                app.exit(error);
                std::fflush(stdout);
                return 0;
            }
            log::error("Init", "{}\nRun with --help to list the options.", error.what());
            return 2;
        }
    }
}
