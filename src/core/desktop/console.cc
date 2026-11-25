#include "core/platform.h"

#include <csignal>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dx::platform {
    namespace {
        void (*crashReport)(const char*) = nullptr;

#ifdef _WIN32
        bool usable(HANDLE handle)
        {
            return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
        }

        LONG WINAPI unhandled(EXCEPTION_POINTERS* info)
        {
            if (crashReport && info && info->ExceptionRecord) {
                char text[128];
                std::snprintf(text, sizeof(text), "crash: exception 0x%08lx at %p", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), info->ExceptionRecord->ExceptionAddress);
                crashReport(text);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
#else
        void crashed(int signal)
        {
            if (crashReport) {
                char text[64];
                std::snprintf(text, sizeof(text), "crash: signal %d", signal);
                crashReport(text);
            }
            std::signal(signal, SIG_DFL);
            std::raise(signal);
        }
#endif
    }

    bool connectConsole(bool attach)
    {
#ifdef _WIN32
        if (usable(GetStdHandle(STD_ERROR_HANDLE)) && _fileno(stderr) >= 0) {
            return true;
        }
        // A Windows GUI program has no console unless it takes one, and one shared with the
        // terminal it was started from writes over its prompt, as the terminal does not wait.
        if (!attach) {
            return false;
        }
        if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) {
            return false;
        }
        std::FILE* ignored = nullptr;
        freopen_s(&ignored, "CONOUT$", "w", stderr);
        if (!usable(GetStdHandle(STD_OUTPUT_HANDLE)) || _fileno(stdout) < 0) {
            freopen_s(&ignored, "CONOUT$", "w", stdout);
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        return _fileno(stderr) >= 0;
#else
        (void)attach;
        return fcntl(2, F_GETFD) != -1;
#endif
    }

    bool consoleRedirected()
    {
#ifdef _WIN32
        HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        if (!usable(handle)) {
            return false;
        }
        DWORD type = GetFileType(handle);
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
#else
        return !isatty(2) && fcntl(2, F_GETFD) != -1;
#endif
    }

    void onCrash(void (*report)(const char* description))
    {
        crashReport = report;
#ifdef _WIN32
        SetUnhandledExceptionFilter(unhandled);
#else
        for (int signal : { SIGSEGV, SIGBUS, SIGFPE, SIGILL }) {
            std::signal(signal, crashed);
        }
#endif
    }
}
