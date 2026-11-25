#include "editor/log_capture.h"

namespace dx::editor {
    namespace {
        constexpr size_t MaxLines = 10000;
    }

    LogCapture::~LogCapture()
    {
        stop();
    }

    void LogCapture::start()
    {
        if (listener) {
            return;
        }
        listener = std::make_unique<log::Listener>([this](const log::Entry& entry) {
            std::string line = entry.category.empty() ? std::string(entry.text) : std::string(entry.category) + ": " + std::string(entry.text);
            add(line);
        });
    }

    void LogCapture::stop()
    {
        listener.reset();
    }

    void LogCapture::add(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        captured.push_back(line);
        if (captured.size() > MaxLines) {
            captured.erase(captured.begin(), captured.begin() + static_cast<std::ptrdiff_t>(captured.size() - MaxLines));
        }
    }

    std::vector<std::string> LogCapture::lines()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return captured;
    }

    void LogCapture::clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        captured.clear();
    }
}
