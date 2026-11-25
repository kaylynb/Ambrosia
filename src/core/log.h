#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// The programs' log. Each line has a level and a category, named as the original names its log
// categories (Init, Log, Warning, ScriptWarning, Critical, Exit, Dev...), and goes to:
// - standard error, as "Category: text", the way the original's log file reads;
// - the log file when one is open, with the time since start, the level and the thread;
// - every Listener, such as the editor's log window.
// Safe from any thread: a line is written whole, never mixed with another thread's.
namespace dx::log {
    enum class Level : uint8_t {
        Debug,
        Info,
        Warning,
        Error,
    };

    // Writes text as one line, or as several when it has newlines inside; a newline at the end is
    // dropped. Errors are flushed to the file at once.
    void write(Level level, std::string_view category, std::string_view text);

    // Whether a line at this level is written at all (setLevel), so a caller can skip work that
    // only feeds the log.
    bool enabled(Level level);
    // Lines below this level are dropped. Info by default.
    void setLevel(Level level);

    template <typename... Args>
    void debug(std::string_view category, std::format_string<Args...> format, Args&&... args)
    {
        if (enabled(Level::Debug)) {
            write(Level::Debug, category, std::format(format, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void info(std::string_view category, std::format_string<Args...> format, Args&&... args)
    {
        if (enabled(Level::Info)) {
            write(Level::Info, category, std::format(format, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void warn(std::string_view category, std::format_string<Args...> format, Args&&... args)
    {
        if (enabled(Level::Warning)) {
            write(Level::Warning, category, std::format(format, std::forward<Args>(args)...));
        }
    }

    template <typename... Args>
    void error(std::string_view category, std::format_string<Args...> format, Args&&... args)
    {
        if (enabled(Level::Error)) {
            write(Level::Error, category, std::format(format, std::forward<Args>(args)...));
        }
    }

    // Standard error. On by default; a program with no console to write to (a Windows GUI
    // program started from Explorer) has none, and nothing goes there.
    void setConsole(bool on);

    // Starts the log file at path, afresh. The previous run's file is kept as <stem>.old<ext>.
    // Lines written before the file opened are written to it first, so it has the whole run.
    // False when it cannot be made; lines then go only to the other outputs.
    bool openFile(const std::filesystem::path& path);
    // The open file's path, or empty.
    std::filesystem::path filePath();
    void closeFile();
    // Writes out what the file has buffered.
    void flush();
    // Called once a frame: flushes when half a second has passed since the last flush, so a busy
    // log costs one write every half second.
    void tick();
    // From a crash handler: writes the line and flushes, without waiting on a lock the crashed
    // thread may hold.
    void writeCrash(std::string_view text);

    // Whether the command line has --console, asking a Windows GUI program for a console, or
    // --help, whose list of options is written there.
    bool consoleRequested(int argc, char** argv);
    // For a program's main, before anything else: connects standard error to a console when
    // asked, or to the file or pipe it was given (platform::connectConsole), turning the console
    // output off when there is none; opens the log file at path; and logs a crash or an
    // unhandled exception there before the program ends. False when the file could not be made.
    bool startProgram(const std::filesystem::path& path, bool console);
    // Whether standard error reaches a terminal, a file or a pipe. True until startProgram finds
    // otherwise.
    bool hasConsole();

    // A line as listeners receive it.
    struct Entry {
        Level level = Level::Info;
        std::string_view category;
        std::string_view text;
        // Seconds since the program's first line.
        double seconds = 0;
    };

    // Receives every line written while it lives, on the thread that wrote it, in order. The
    // log's lock is held. A line receive writes itself still goes to standard error and the
    // file, but to no listener; receive must not call the log's other functions, which take
    // the lock.
    class Listener {
    public:
        explicit Listener(std::function<void(const Entry&)> receiver);
        ~Listener();
        Listener(const Listener&) = delete;
        Listener& operator=(const Listener&) = delete;

        // Hands it a line; the log does this.
        void deliver(const Entry& entry) const
        {
            receive(entry);
        }

    private:
        std::function<void(const Entry&)> receive;
    };

    // A level's one-letter mark in the file: D, I, W or E.
    char levelMark(Level level);
}
