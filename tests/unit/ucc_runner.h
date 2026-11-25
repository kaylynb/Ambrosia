#pragma once

#include "test_paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>

// Runs the ucc built alongside the tests, from any test file, on Windows and POSIX alike.
namespace ucctest {
    inline void setDataHome(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s("XDG_DATA_HOME", value.c_str());
#else
        setenv("XDG_DATA_HOME", value.c_str(), 1);
#endif
    }

    // Runs ucc with the current user directory, its output going to the file named or nowhere.
    inline int runUcc(const std::string& arguments, const std::filesystem::path& output = {})
    {
#ifdef _WIN32
        std::string sink = output.empty() ? std::string("NUL") : "\"" + output.string() + "\"";
#else
        std::string sink = output.empty() ? std::string("/dev/null") : "\"" + output.string() + "\"";
#endif
        std::string command = "\"" + dx::test::uccPath().string() + "\" " + arguments + " > " + sink + " 2>&1";
#ifdef _WIN32
        // cmd.exe drops the first and last quote of a command that starts with one.
        command = "\"" + command + "\"";
#endif
        return std::system(command.c_str());
    }

    // Runs ucc with its user directory under home. The variable is set here for ucc to inherit,
    // rather than with a "VAR=value command" prefix that only a POSIX shell understands.
    inline int runUcc(const std::filesystem::path& home, const std::string& arguments, const std::filesystem::path& output = {})
    {
        const char* before = std::getenv("XDG_DATA_HOME");
        std::string saved = before ? before : "";
        setDataHome(home.string());
        int result = runUcc(arguments, output);
        setDataHome(saved);
        return result;
    }
}
