#include "game/console_report.h"
#include "core/log.h"
#include "game/switch/runner.h"

#include <cstdarg>
#include <cstdio>

#include <arpa/inet.h>
#include <switch.h>

#include <exception>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <unistd.h>

namespace dx::game::report {
    namespace {
        constexpr const char* logPath = "sdmc:/switch/ambrosia/log.txt";
        int logFile = -1;
        int nxlinkSocket = -1;
        bool firstFrameSeen = false;
        bool unattended = false;
        u64 lastSync = 0;
        u64 startTick = 0;

        // Every write to stdout or stderr: to the debug log, the log file and nxlink. Until the
        // first frame the file is synced at each line, so a crash or abort still leaves it;
        // after that at most every half second.
        ssize_t teeWrite(struct _reent*, void*, const char* text, size_t length)
        {
            svcOutputDebugString(text, length);
            if (logFile >= 0) {
                write(logFile, text, length);
                u64 now = armGetSystemTick();
                bool line = length > 0 && text[length - 1] == '\n';
                if (line && (!firstFrameSeen || armTicksToNs(now - lastSync) > 500'000'000)) {
                    fsync(logFile);
                    lastSync = now;
                }
            }
            if (nxlinkSocket >= 0) {
                write(nxlinkSocket, text, length);
            }
            return static_cast<ssize_t>(length);
        }

        devoptab_t teeDevice {};

        void terminated()
        {
            std::fprintf(stderr, "Critical: unhandled exception");
            if (std::exception_ptr current = std::current_exception()) {
                try {
                    std::rethrow_exception(current);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, ": %s", error.what());
                } catch (...) {
                }
            }
            std::fprintf(stderr, "\n");
            fatal("Ambrosia stopped on an unhandled error. Details are in sdmc:/switch/ambrosia/log.txt.");
            std::_Exit(1);
        }

        // A text screen for when the error applet cannot be shown, left with +.
        void consoleMessage(const std::string& message)
        {
            consoleInit(nullptr);
            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            PadState pad;
            padInitializeDefault(&pad);
            std::printf("\n  Ambrosia cannot continue\n\n  %s\n\n  Press + to return to the homebrew menu.\n", message.c_str());
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                    break;
                }
                consoleUpdate(nullptr);
            }
            consoleExit(nullptr);
        }
    }

    void start(bool)
    {
        startTick = armGetSystemTick();
        logFile = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (__nxlink_host.s_addr != 0) {
            // Profiling builds have brought the network up already, before main
            // (core/switch/tracy).
            Result sockets = socketInitializeDefault();
            if (R_SUCCEEDED(sockets) || sockets == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
                nxlinkSocket = nxlinkConnectToHost(false, false);
            }
        }
        teeDevice.name = "ambrosia-log";
        teeDevice.write_r = teeWrite;
        devoptab_list[STD_OUT] = &teeDevice;
        devoptab_list[STD_ERR] = &teeDevice;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::set_terminate(terminated);
        u64 total = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        stage("log %s, nxlink %s", logFile >= 0 ? "open" : "not open", nxlinkSocket >= 0 ? "connected" : "off");
        stage("started as %s, with %llu MB of memory", appletMode() ? "an applet (album)" : "an application (title override)", static_cast<unsigned long long>(total >> 20));
    }

    void stage(const char* format, ...)
    {
        char text[1024];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        log::info("Init", "{}", text);
    }

    void fatal(const std::string& message)
    {
        log::error("Critical", "{}", message);
        if (logFile >= 0) {
            fsync(logFile);
        }
        if (unattended) {
            return;
        }
        ErrorApplicationConfig config;
        if (R_FAILED(errorApplicationCreate(&config, message.c_str(), message.c_str())) || R_FAILED(errorApplicationShow(&config))) {
            consoleMessage(message);
        }
    }

    void setUnattended()
    {
        unattended = true;
    }

    bool appletMode()
    {
        AppletType type = appletGetAppletType();
        return type != AppletType_Application && type != AppletType_SystemApplication;
    }

    void frameDone()
    {
        if (!firstFrameSeen) {
            // The time since start, which the PC's runner script checks against its deadline.
            stage("first frame drawn, %.1f s after start", static_cast<double>(armTicksToNs(armGetSystemTick() - startTick)) / 1e9);
            firstFrameSeen = true;
            if (logFile >= 0) {
                fsync(logFile);
            }
        }
    }
}

// A crash (such as a bad memory access) logs where it happened before the program ends.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* context)
{
    char text[256];
    int length = std::snprintf(text, sizeof(text), "Critical: crash: error %u at pc 0x%llx, lr 0x%llx, far 0x%llx, sp 0x%llx\n", context->error_desc, static_cast<unsigned long long>(context->pc.x), static_cast<unsigned long long>(context->lr.x), static_cast<unsigned long long>(context->far.x), static_cast<unsigned long long>(context->sp.x));
    if (length > 0) {
        dx::game::report::teeWrite(nullptr, nullptr, text, static_cast<size_t>(length));
    }
    if (dx::game::report::logFile >= 0) {
        fsync(dx::game::report::logFile);
    }
}
}
