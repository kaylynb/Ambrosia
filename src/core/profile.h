#pragma once

// The profiler's hooks. Engine code uses these and never Tracy's own headers. They feed two
// things: Tracy, in profiling builds (AMBROSIA_TRACY, which sets DX_PROFILE to 1), and the built-in
// frame recorder below, in every build. With DX_PROFILE 0, Mutex is std::mutex and
// ConditionVariable is std::condition_variable_any, and the macros that only Tracy uses are
// empty.
//
//   DX_ZONE()                     a zone named after the enclosing function, to the end of scope
//   DX_ZONE_NAMED("name")         a zone with that name (a string literal)
//   DX_FRAME_ZONE("name")         the zone of a whole frame, first in the main loop's step
//   DX_FRAME_MARK()               the end of a frame, at the swap: also plots the DX_TIME totals
//                                 and closes the recorder's frame
//   DX_THREAD_NAME("name")        names the calling thread, the first time it gets there
//   DX_PLOT("name", value)        a value on a named plot
//   DX_MESSAGE(text)              a message in the trace's log, from a std::string_view
//   DX_LOCKABLE(name, "desc")     declares a Mutex whose waits and holds the trace shows
//   DX_LOCK_GUARD(g, m, "zone")   std::lock_guard g on m, with the wait for it timed as a zone
//   DX_ALLOC(ptr, size)           an allocation, for memory not from the global operator new,
//   DX_FREE(ptr)                  and its release; operator new itself is hooked by
//                                 core/profile_memory.cc in profiling builds
//   DX_TIME("name")               adds the time to the end of scope to a total plotted once a
//                                 frame (in milliseconds), for work done in many small pieces
//                                 such as each actor's script or physics. Nested or repeated
//                                 scopes with the same name count once. Main thread only.
//
// The recorder keeps, for each zone, its time and calls in each frame, with a history of recent
// frames, for the Performance tool and for CSV files read on a PC afterwards; it needs no
// profiler attached. It is off unless something turns it on (setRecording). Off, each zone and
// DX_TIME costs one load and one predictable branch on entry and one on exit; DX_PLOT one load
// and branch. Zones may be on any thread; DX_TIME and DX_PLOT are for the main thread, and
// endFrame (DX_FRAME_MARK) must be called from the main thread.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dx::profile {
    // Whether the recorder takes zones, timers and plots. Read on every zone, so a plain flag.
    inline std::atomic<bool> recording { false };

    // One zone's place in the code. Constant-initialised, so a function-local static of it costs
    // no guard.
    struct Site {
        const char* name;
        // The recorder's zone last entered from here, with the parent it was found under.
        std::atomic<uint64_t> cache { 0 };

        constexpr explicit Site(const char* siteName)
            : name(siteName)
        {
        }
    };

    // The recorder's side of a zone; both out of line, so the inline part stays two branches.
    // enterZone gives the zone's index and its start time, or -1 when it is not recorded.
    int enterZone(Site& site, int64_t& start);
    void leaveZone(int zone, int64_t start);

    class ZoneScope {
    public:
        explicit ZoneScope(Site& site)
        {
            if (recording.load(std::memory_order_relaxed)) [[unlikely]] {
                zone = enterZone(site, start);
            }
        }

        ~ZoneScope()
        {
            if (zone >= 0) [[unlikely]] {
                leaveZone(zone, start);
            }
        }

        ZoneScope(const ZoneScope&) = delete;
        ZoneScope& operator=(const ZoneScope&) = delete;

    private:
        int zone = -1;
        int64_t start = 0;
    };

    // A DX_TIME total, shared by every scope of the same name.
    struct Timer {
        const char* name = nullptr;
        int64_t total = 0;
        int64_t start = 0;
        int depth = 0;
        // Its column in the recorder's values, or -1 before the recorder first saw it.
        int value = -1;
    };

    // The total for name, made on first use. The same name gives the same total everywhere.
    Timer& timer(const char* name);
    int64_t nanoseconds();

    inline void startTimer(Timer& running)
    {
        if (running.depth++ == 0) {
            running.start = nanoseconds();
        }
    }

    inline void stopTimer(Timer& running)
    {
        if (--running.depth == 0) {
            running.total += nanoseconds() - running.start;
        }
    }

    // A DX_TIME scope that always times, for Tracy's plots.
    class TimerScope {
    public:
        explicit TimerScope(Timer& timer)
            : running(timer)
        {
            startTimer(running);
        }

        ~TimerScope()
        {
            stopTimer(running);
        }

        TimerScope(const TimerScope&) = delete;
        TimerScope& operator=(const TimerScope&) = delete;

    private:
        Timer& running;
    };

    // A DX_TIME scope that times only while the recorder runs, for builds without Tracy.
    struct TimeSite {
        const char* name;
        Timer* timer = nullptr;

        constexpr explicit TimeSite(const char* siteName)
            : name(siteName)
        {
        }
    };

    Timer& siteTimer(TimeSite& site);

    class TimeScope {
    public:
        explicit TimeScope(TimeSite& site)
        {
            if (recording.load(std::memory_order_relaxed)) [[unlikely]] {
                running = &siteTimer(site);
                startTimer(*running);
            }
        }

        ~TimeScope()
        {
            if (running) [[unlikely]] {
                stopTimer(*running);
            }
        }

        TimeScope(const TimeScope&) = delete;
        TimeScope& operator=(const TimeScope&) = delete;

    private:
        Timer* running = nullptr;
    };

    // A DX_PLOT value for the recorder.
    void plot(const char* name, double value);
    // Names the calling thread for the recorder; DX_THREAD_NAME calls it.
    void setThreadName(const char* name);
    // Ends a frame: Tracy's plots of the DX_TIME totals, and the recorder's frame. DX_FRAME_MARK
    // calls it.
    void endFrame();
    // Begins a frame, before any of its zones; DX_FRAME_ZONE calls it. Turned on in a frame
    // that began this way, the recorder starts at the next frame's beginning, so a frame's zones
    // are never taken without the zone they nest in; otherwise it starts at the frame's end.
    void beginFrame();

    // The recorder.
    //
    // A zone is a name under a parent zone, so a name met under two parents is two zones; a zone
    // entered with no zone open on its thread is a root of that thread. Turning the recorder on
    // starts its history at the next frame's end; the frames before that are not kept.
    constexpr size_t MaxZones = 256;
    constexpr size_t MaxValues = 96;
    constexpr size_t HistoryFrames = 600;
    constexpr int MaxDepth = 48;

    void setRecording(bool on);
    bool isRecording();
    // Forgets the history and the totals since recording started; the zones stay known.
    void reset();
    // Records from the next frame for this long (seconds, when frames is 0) or this many frames,
    // turning the recorder on, then turns it off as it was and calls done at a frame's end, on
    // the main thread. A recording already timed is replaced.
    void recordFor(double seconds, int frames, std::function<void()> done);
    // Whether a timed recording is under way, and how far it has got (0 to 1).
    bool timedRecording(double& progress);

    struct ZoneInfo {
        const char* name = nullptr;
        // The zone it nests in, or -1 for a root.
        int parent = -1;
        int depth = 0;
        // The name of the thread it ran on (DX_THREAD_NAME), or empty.
        std::string thread;
    };

    struct ValueInfo {
        const char* name = nullptr;
        // A DX_TIME total in milliseconds, or a DX_PLOT value.
        bool time = false;
    };

    std::vector<ZoneInfo> zones();
    std::vector<ValueInfo> values();

    // One frame of the history: its time from the previous frame's end, each zone's time and
    // calls, and each value (a plot keeps its last value until set again).
    struct FrameRecord {
        float frameMs = 0;
        float zoneMs[MaxZones] = {};
        uint32_t zoneCalls[MaxZones] = {};
        float values[MaxValues] = {};
    };

    // The frames kept, oldest first; at most HistoryFrames.
    size_t historySize();
    const FrameRecord& historyFrame(size_t index);

    struct ZoneStats {
        double meanMs = 0;
        double worstMs = 0;
        // Of the mean frame time.
        double share = 0;
        double callsPerFrame = 0;
    };

    struct ValueStats {
        double mean = 0;
        double worst = 0;
        double last = 0;
    };

    struct Summary {
        size_t frames = 0;
        double meanFrameMs = 0;
        double worstFrameMs = 0;
        // The mean of the slowest 1% of frames (at least one frame).
        double onePercentLowMs = 0;
        std::vector<ZoneStats> zones;
        std::vector<ValueStats> values;
    };

    // Over the history kept, or over every frame since recording started (or reset).
    Summary summarize(bool sinceStart);
    // The summary since start, then the history frame by frame, as CSV.
    std::string csv();
    bool writeCsv(const std::string& path, std::string& error);
    // Zone names with their parents', "Frame/Render/Scene".
    std::string zonePath(const std::vector<ZoneInfo>& zones, int zone);
}

// The zone of a whole frame, at the start of the main loop's step: DX_ZONE_NAMED, after telling
// the recorder a frame begins.
#define DX_FRAME_ZONE(name)    \
    dx::profile::beginFrame(); \
    DX_ZONE_NAMED(name)

#define DX_PROFILE_CONCAT_INNER(a, b) a##b
#define DX_PROFILE_CONCAT(a, b) DX_PROFILE_CONCAT_INNER(a, b)

// The recorder's zone, with its own variables, so several can share a scope and nest.
#define DX_RECORD_ZONE(name)                                                               \
    static constinit dx::profile::Site DX_PROFILE_CONCAT(dxRecordSite, __LINE__) { name }; \
    dx::profile::ZoneScope DX_PROFILE_CONCAT(dxRecordZone, __LINE__)(DX_PROFILE_CONCAT(dxRecordSite, __LINE__))

#if DX_PROFILE

#include <tracy/Tracy.hpp>

#include <string_view>

namespace dx::profile {
    using Mutex = tracy::Lockable<std::mutex>;
    // A Lockable is not a std::mutex, so a condition variable waiting on it is the general kind,
    // which also has the waits that a std::stop_token ends.
    using ConditionVariable = std::condition_variable_any;
}

// Each zone has its own variable, so several can share a scope and nest without shadowing.
#define DX_ZONE()                                                \
    ZoneNamed(DX_PROFILE_CONCAT(dxProfileZone, __LINE__), true); \
    DX_RECORD_ZONE(__func__)
#define DX_ZONE_NAMED(name)                                             \
    ZoneNamedN(DX_PROFILE_CONCAT(dxProfileZone, __LINE__), name, true); \
    DX_RECORD_ZONE(name)
#define DX_FRAME_MARK()          \
    do {                         \
        dx::profile::endFrame(); \
        FrameMark;               \
    } while (false)
#define DX_THREAD_NAME(name)                                                                                                                                 \
    static thread_local const bool DX_PROFILE_CONCAT(dxProfileThreadNamed, __LINE__) = (tracy::SetThreadName(name), dx::profile::setThreadName(name), true); \
    static_cast<void>(DX_PROFILE_CONCAT(dxProfileThreadNamed, __LINE__))
#define DX_PLOT(name, value)                                          \
    do {                                                              \
        double dxProfilePlotValue = static_cast<double>(value);       \
        TracyPlot(name, dxProfilePlotValue);                          \
        if (dx::profile::recording.load(std::memory_order_relaxed)) { \
            dx::profile::plot(name, dxProfilePlotValue);              \
        }                                                             \
    } while (false)
#define DX_MESSAGE(text)                                                \
    do {                                                                \
        std::string_view dxProfileMessage = (text);                     \
        TracyMessage(dxProfileMessage.data(), dxProfileMessage.size()); \
    } while (false)
#define DX_LOCKABLE(name, description) TracyLockableN(std::mutex, name, description)
#define DX_LOCK_GUARD(guard, lockable, zone)                                    \
    std::unique_lock<dx::profile::Mutex> guard((lockable), std::defer_lock);    \
    {                                                                           \
        ZoneNamedN(DX_PROFILE_CONCAT(dxProfileLockWait, __LINE__), zone, true); \
        guard.lock();                                                           \
    }
#define DX_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define DX_FREE(ptr) TracyFree(ptr)
#define DX_TIME(name)                                                                                  \
    static dx::profile::Timer& DX_PROFILE_CONCAT(dxProfileTimer, __LINE__) = dx::profile::timer(name); \
    dx::profile::TimerScope DX_PROFILE_CONCAT(dxProfileTimerScope, __LINE__)(DX_PROFILE_CONCAT(dxProfileTimer, __LINE__))
// DX_TIME into one of two totals, chosen by a condition evaluated once.
#define DX_TIME_EITHER(condition, whenTrue, whenFalse)                                                       \
    static dx::profile::Timer& DX_PROFILE_CONCAT(dxProfileTimerA, __LINE__) = dx::profile::timer(whenTrue);  \
    static dx::profile::Timer& DX_PROFILE_CONCAT(dxProfileTimerB, __LINE__) = dx::profile::timer(whenFalse); \
    dx::profile::TimerScope DX_PROFILE_CONCAT(dxProfileTimerScope, __LINE__)((condition) ? DX_PROFILE_CONCAT(dxProfileTimerA, __LINE__) : DX_PROFILE_CONCAT(dxProfileTimerB, __LINE__))

#else

namespace dx::profile {
    using Mutex = std::mutex;
    // The general kind, as with Tracy, for its waits that a std::stop_token ends.
    using ConditionVariable = std::condition_variable_any;
}

#define DX_ZONE() DX_RECORD_ZONE(__func__)
#define DX_ZONE_NAMED(name) DX_RECORD_ZONE(name)
#define DX_FRAME_MARK() dx::profile::endFrame()
#define DX_THREAD_NAME(name)                                                                                                     \
    static thread_local const bool DX_PROFILE_CONCAT(dxProfileThreadNamed, __LINE__) = (dx::profile::setThreadName(name), true); \
    static_cast<void>(DX_PROFILE_CONCAT(dxProfileThreadNamed, __LINE__))
#define DX_PLOT(name, value)                                          \
    do {                                                              \
        if (dx::profile::recording.load(std::memory_order_relaxed)) { \
            dx::profile::plot(name, static_cast<double>(value));      \
        }                                                             \
    } while (false)
#define DX_MESSAGE(text) static_cast<void>(sizeof(text))
#define DX_LOCKABLE(name, description) std::mutex name
#define DX_LOCK_GUARD(guard, lockable, zone) std::lock_guard<std::mutex> guard(lockable)
#define DX_ALLOC(ptr, size) static_cast<void>(sizeof(ptr) + sizeof(size))
#define DX_FREE(ptr) static_cast<void>(sizeof(ptr))
#define DX_TIME(name)                                                                               \
    static constinit dx::profile::TimeSite DX_PROFILE_CONCAT(dxProfileTimeSite, __LINE__) { name }; \
    dx::profile::TimeScope DX_PROFILE_CONCAT(dxProfileTimeScope, __LINE__)(DX_PROFILE_CONCAT(dxProfileTimeSite, __LINE__))
// DX_TIME into one of two totals, chosen by a condition evaluated once.
#define DX_TIME_EITHER(condition, whenTrue, whenFalse)                                                    \
    static constinit dx::profile::TimeSite DX_PROFILE_CONCAT(dxProfileTimeSiteA, __LINE__) { whenTrue };  \
    static constinit dx::profile::TimeSite DX_PROFILE_CONCAT(dxProfileTimeSiteB, __LINE__) { whenFalse }; \
    dx::profile::TimeScope DX_PROFILE_CONCAT(dxProfileTimeScope, __LINE__)((condition) ? DX_PROFILE_CONCAT(dxProfileTimeSiteA, __LINE__) : DX_PROFILE_CONCAT(dxProfileTimeSiteB, __LINE__))

#endif
