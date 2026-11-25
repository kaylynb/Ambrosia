#include "core/log.h"

#include "core/platform.h"
#include "core/scoped_value.h"
#include "core/version.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace dx::log {
    namespace {
        using Clock = std::chrono::steady_clock;

        // Lines kept from before the file opened, up to this many bytes; later ones are not kept.
        constexpr size_t EarlyLimit = 256 * 1024;
        constexpr size_t FileBuffer = 64 * 1024;
        constexpr auto FlushInterval = std::chrono::milliseconds(500);

        struct State {
            std::mutex mutex;
            Clock::time_point start = Clock::now();
            std::atomic<Level> level { Level::Info };
            bool console = true;
            std::FILE* file = nullptr;
            std::filesystem::path path;
            bool unflushed = false;
            Clock::time_point lastFlush = Clock::now();
            bool keepEarly = true;
            std::string early;
            std::vector<Listener*> listeners;
            int threads = 0;
        };

        // Never freed, so lines written while statics are destroyed at exit still have somewhere
        // to go.
        State& state()
        {
            static State* instance = new State;
            return *instance;
        }

        // A small number per thread, in the order threads first write, for the file.
        int threadNumber(State& s)
        {
            thread_local int number = -1;
            if (number < 0) {
                number = s.threads++;
            }
            return number;
        }

        void flushLocked(State& s)
        {
            if (s.file && s.unflushed) {
                std::fflush(s.file);
            }
            s.unflushed = false;
            s.lastFlush = Clock::now();
        }

        // Set while this thread hands a line to the listeners, the mutex held. A listener that
        // logs (which it should not) would otherwise wait on the lock its own thread holds.
        thread_local bool delivering = false;

        // One line, with the mutex held. Listeners are given it unless one of them wrote it.
        void writeLine(State& s, Level level, std::string_view category, std::string_view text, double seconds)
        {
            if (s.console) {
                if (category.empty()) {
                    std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
                } else {
                    std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(category.size()), category.data(), static_cast<int>(text.size()), text.data());
                }
            }
            if (s.file || s.keepEarly) {
                std::string line = std::format("{:9.3f} {} t{:<2} {}{}{}\n", seconds, levelMark(level), threadNumber(s), category, category.empty() ? "" : ": ", text);
                if (s.file) {
                    std::fwrite(line.data(), 1, line.size(), s.file);
                    s.unflushed = true;
                } else if (s.early.size() + line.size() <= EarlyLimit) {
                    s.early += line;
                } else {
                    s.keepEarly = false;
                    s.early.clear();
                    s.early.shrink_to_fit();
                }
            }
            if (delivering) {
                return;
            }
            ScopedValue<bool> handing(delivering, true);
            Entry entry { level, category, text, seconds };
            for (Listener* listener : s.listeners) {
                listener->deliver(entry);
            }
        }

        // Text as lines, with the mutex held.
        void writeLines(State& s, Level level, std::string_view category, std::string_view text, double seconds)
        {
            size_t begin = 0;
            while (true) {
                size_t end = text.find('\n', begin);
                std::string_view part = text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
                if (!part.empty() && part.back() == '\r') {
                    part.remove_suffix(1);
                }
                writeLine(s, level, category, part, seconds);
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            if (level == Level::Error) {
                flushLocked(s);
            }
        }

        void flushAtExit()
        {
            flush();
        }

        void fromSDL(void*, int, SDL_LogPriority priority, const char* message)
        {
            Level level = priority >= SDL_LOG_PRIORITY_ERROR ? Level::Error : priority >= SDL_LOG_PRIORITY_WARN ? Level::Warning
                : priority >= SDL_LOG_PRIORITY_INFO                                                             ? Level::Info
                                                                                                                : Level::Debug;
            write(level, "SDL", message ? message : "");
        }
    }

    char levelMark(Level level)
    {
        switch (level) {
        case Level::Debug:
            return 'D';
        case Level::Info:
            return 'I';
        case Level::Warning:
            return 'W';
        case Level::Error:
            return 'E';
        }
        return '?';
    }

    void write(Level level, std::string_view category, std::string_view text)
    {
        State& s = state();
        if (level < s.level.load(std::memory_order_relaxed)) {
            return;
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        double seconds = std::chrono::duration<double>(Clock::now() - s.start).count();
        if (delivering) {
            // From a listener: this thread holds the lock already.
            writeLines(s, level, category, text, seconds);
            return;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        writeLines(s, level, category, text, seconds);
    }

    bool enabled(Level level)
    {
        return level >= state().level.load(std::memory_order_relaxed);
    }

    void setLevel(Level level)
    {
        state().level.store(level, std::memory_order_relaxed);
    }

    void setConsole(bool on)
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.console = on;
    }

    bool openFile(const std::filesystem::path& path)
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.file) {
            flushLocked(s);
            std::fclose(s.file);
            s.file = nullptr;
            s.path.clear();
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (std::filesystem::exists(path, error)) {
            std::filesystem::path old = path;
            old.replace_filename(path.stem().string() + ".old" + path.extension().string());
            std::filesystem::rename(path, old, error);
        }
#ifdef _WIN32
        std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
        std::FILE* file = std::fopen(path.c_str(), "wb");
#endif
        if (!file) {
            return false;
        }
        std::setvbuf(file, nullptr, _IOFBF, FileBuffer);
        s.file = file;
        s.path = path;
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string header = std::format("Log started {:%Y-%m-%d %H:%M:%S} UTC\n", now);
        std::fwrite(header.data(), 1, header.size(), file);
        std::fwrite(s.early.data(), 1, s.early.size(), file);
        s.early.clear();
        s.early.shrink_to_fit();
        s.keepEarly = false;
        flushLocked(s);
        return true;
    }

    std::filesystem::path filePath()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.path;
    }

    void closeFile()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.file) {
            flushLocked(s);
            std::fclose(s.file);
            s.file = nullptr;
            s.path.clear();
        }
    }

    void flush()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        flushLocked(s);
    }

    void tick()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.unflushed && Clock::now() - s.lastFlush >= FlushInterval) {
            flushLocked(s);
        }
    }

    void writeCrash(std::string_view text)
    {
        State& s = state();
        // The crashed thread may hold the lock, and will never let it go; the line is written
        // anyway, as the process is about to end.
        std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
        if (s.console) {
            std::fprintf(stderr, "Critical: %.*s\n", static_cast<int>(text.size()), text.data());
        }
        if (s.file) {
            std::fprintf(s.file, "%9s E     Critical: %.*s\n", "", static_cast<int>(text.size()), text.data());
            std::fflush(s.file);
        }
    }

    bool hasConsole()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.console;
    }

    bool consoleRequested(int argc, char** argv)
    {
        for (int i = 1; i < argc; i++) {
            // --help too, so the options it lists show in the terminal it was asked from.
            std::string_view arg(argv[i]);
            if (arg == "--console" || arg == "--help" || arg == "-h" || arg == "--version") {
                return true;
            }
        }
        return false;
    }

    bool startProgram(const std::filesystem::path& path, bool console)
    {
        setConsole(platform::connectConsole(console));
        SDL_SetLogOutputFunction(fromSDL, nullptr);
        platform::onCrash([](const char* description) {
            writeCrash(description);
        });
        static std::terminate_handler previous = std::set_terminate([] {
            std::string text = "unhandled exception";
            if (std::exception_ptr current = std::current_exception()) {
                try {
                    std::rethrow_exception(current);
                } catch (const std::exception& failure) {
                    text += std::string(": ") + failure.what();
                } catch (...) {
                }
            }
            writeCrash(text);
            if (previous) {
                previous();
            }
            std::abort();
        });
        std::atexit(flushAtExit);
        bool opened = openFile(path);
        info("Init", "{}", version::build());
        return opened;
    }

    Listener::Listener(std::function<void(const Entry&)> receiver)
        : receive(std::move(receiver))
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.listeners.push_back(this);
    }

    Listener::~Listener()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::erase(s.listeners, this);
    }
}
