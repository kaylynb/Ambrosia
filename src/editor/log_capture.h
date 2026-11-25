#pragma once

#include "core/log.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dx::editor {
    // The lines the log has written since start, for the editor's log window: the last 10000,
    // each as "Category: text".
    class LogCapture {
    public:
        LogCapture() = default;
        ~LogCapture();
        LogCapture(const LogCapture&) = delete;
        LogCapture& operator=(const LogCapture&) = delete;

        void start();
        void stop();
        // A line of the window's own, such as a command typed into it, which the log does not get.
        void add(const std::string& line);
        std::vector<std::string> lines();
        void clear();

    private:
        std::mutex mutex;
        std::vector<std::string> captured;
        std::unique_ptr<log::Listener> listener;
    };
}
