#include "core/run_steps.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace dx::steps {
    namespace {
        constexpr int FramesPerSecond = 30;

        std::string_view trimmed(std::string_view text)
        {
            size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string_view::npos) {
                return {};
            }
            size_t last = text.find_last_not_of(" \t\r");
            return text.substr(first, last - first + 1);
        }

        // What follows "wait": a frame count, nothing when it is something else.
        std::optional<int> waitFrames(std::string_view what)
        {
            bool seconds = what.ends_with('s');
            if (seconds) {
                what.remove_suffix(1);
            }
            int count = 0;
            auto [end, error] = std::from_chars(what.data(), what.data() + what.size(), count);
            if (what.empty() || error != std::errc() || end != what.data() + what.size() || count < 0) {
                return std::nullopt;
            }
            return seconds ? count * FramesPerSecond : count;
        }

        // The rest of a "wait" step, or nothing when the step is not one.
        std::optional<std::string_view> waitArgument(std::string_view step)
        {
            if (step != "wait" && !step.starts_with("wait ")) {
                return std::nullopt;
            }
            return trimmed(step.substr(4));
        }
    }

    std::vector<std::string> splitInline(std::string_view text)
    {
        std::vector<std::string> result;
        while (true) {
            size_t semicolon = text.find(';');
            std::string_view step = trimmed(text.substr(0, semicolon));
            if (!step.empty()) {
                result.emplace_back(step);
            }
            if (semicolon == std::string_view::npos) {
                return result;
            }
            text.remove_prefix(semicolon + 1);
        }
    }

    std::vector<std::string> splitLines(std::string_view text)
    {
        std::vector<std::string> result;
        while (!text.empty()) {
            size_t newline = text.find('\n');
            std::string_view step = trimmed(text.substr(0, newline));
            if (!step.empty() && !step.starts_with('#')) {
                result.emplace_back(step);
            }
            text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        }
        return result;
    }

    std::optional<std::string> check(std::string_view step)
    {
        std::optional<std::string_view> what = waitArgument(step);
        if (!what || *what == "loaded" || waitFrames(*what)) {
            return std::nullopt;
        }
        return "wait needs a number of frames, a number of seconds such as 2s, or loaded, not \"" + std::string(*what) + "\"";
    }

    bool isQuit(std::string_view step)
    {
        return step == "quit";
    }

    Runner::Runner(std::vector<std::string> list)
        : steps(std::move(list))
    {
    }

    void Runner::tick(const std::function<bool()>& loaded, const std::function<void(const std::string&)>& run)
    {
        if (framesLeft > 0 && --framesLeft > 0) {
            return;
        }
        while (!quit && next < steps.size()) {
            if (waitingForLoad) {
                if (!loaded()) {
                    return;
                }
                waitingForLoad = false;
            }
            const std::string& step = steps[next++];
            if (std::optional<std::string_view> what = waitArgument(step)) {
                if (*what == "loaded") {
                    waitingForLoad = true;
                    continue;
                }
                framesLeft = waitFrames(*what).value_or(0);
                if (framesLeft > 0) {
                    return;
                }
            } else if (isQuit(step)) {
                quit = true;
            } else {
                run(step);
            }
        }
    }
}
