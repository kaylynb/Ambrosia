#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The steps an automated run takes (--do and --script, core/command_line.h): the program's own
// test actions, run in order after each frame, with these of their own between them:
//   wait <N>       the next step runs N frames later
//   wait <N>s      N seconds later, at 30 frames a second, the automated runs' fixed step
//   wait loaded    once the program has its level loaded, after a level change
//   quit           ends the run
namespace dx::steps {
    // The steps in a --do value: separated by semicolons, with the spaces around each trimmed.
    std::vector<std::string> splitInline(std::string_view text);
    // The steps in a --script file: one a line, leaving out blank lines and # comments.
    std::vector<std::string> splitLines(std::string_view text);
    // What is wrong with a step that waits or quits (a wait for something unknown, say), or
    // nothing. Every other step is the program's to understand.
    std::optional<std::string> check(std::string_view step);
    // Whether a step is quit.
    bool isQuit(std::string_view step);

    class Runner {
    public:
        explicit Runner(std::vector<std::string> list);

        // Called once after each frame: runs each step that is due through run, until one waits
        // or quits. loaded says whether the level is loaded, for wait loaded.
        void tick(const std::function<bool()>& loaded, const std::function<void(const std::string&)>& run);

        // A quit step has run.
        bool quitting() const
        {
            return quit;
        }

    private:
        std::vector<std::string> steps;
        size_t next = 0;
        int framesLeft = 0;
        bool waitingForLoad = false;
        bool quit = false;
    };
}
