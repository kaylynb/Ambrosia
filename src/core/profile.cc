#include "core/profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace dx::profile {
    namespace {
        constexpr int MaxThreads = 16;

        struct Zone {
            const char* name = nullptr;
            int parent = -1;
            int depth = 0;
            int thread = 0;
            std::atomic<int64_t> time { 0 };
            std::atomic<uint32_t> calls { 0 };
        };

        struct Value {
            const char* name = nullptr;
            bool time = false;
            double current = 0;
        };

        // Written under tableMutex; zoneCount is stored last, so a reader that loads it sees the
        // zones below it whole.
        Zone zoneTable[MaxZones];
        std::atomic<int> zoneCount { 0 };
        Value valueTable[MaxValues];
        int valueCount = 0;
        std::mutex tableMutex;

        // When the recorder's first whole frame began. A zone that started before it (such as
        // the frame's own zone, open across the frame's end) counts only from then.
        std::atomic<int64_t> windowStart { 0 };
        // Whether zones are taken: the recorder is on and its first frame has begun.
        std::atomic<bool> armed { false };
        // Whether this frame's start was marked (beginFrame) since the last frame's end; main
        // thread only.
        bool frameBegun = false;

        std::atomic<const char*> threadNames[MaxThreads];
        std::atomic<int> threadCount { 0 };

        // Each thread's open recorded zones.
        thread_local int openZones[MaxDepth];
        thread_local int openCount = 0;
        thread_local int threadIndex = -1;

        int currentThread()
        {
            if (threadIndex < 0) {
                // Threads past the last share it.
                threadIndex = std::min(threadCount.fetch_add(1, std::memory_order_relaxed), MaxThreads - 1);
            }
            return threadIndex;
        }

        bool sameName(const char* a, const char* b)
        {
            return a == b || std::strcmp(a, b) == 0;
        }

        int findZone(const char* name, int parent, int thread)
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            int count = zoneCount.load(std::memory_order_relaxed);
            for (int i = 0; i < count; i++) {
                const Zone& zone = zoneTable[i];
                if (zone.parent == parent && (parent >= 0 || zone.thread == thread) && sameName(zone.name, name)) {
                    return i;
                }
            }
            if (count >= static_cast<int>(MaxZones)) {
                return -1;
            }
            Zone& made = zoneTable[count];
            made.name = name;
            made.parent = parent;
            made.depth = parent >= 0 ? zoneTable[parent].depth + 1 : 0;
            made.thread = thread;
            zoneCount.store(count + 1, std::memory_order_release);
            return count;
        }

        // Called with tableMutex held.
        int findValue(const char* name, bool time)
        {
            for (int i = 0; i < valueCount; i++) {
                if (valueTable[i].time == time && sameName(valueTable[i].name, name)) {
                    return i;
                }
            }
            if (valueCount >= static_cast<int>(MaxValues)) {
                return -1;
            }
            valueTable[valueCount] = { name, time, 0 };
            return valueCount++;
        }

        uint64_t packCache(int key, int zone)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(key)) << 32) | static_cast<uint32_t>(zone + 1);
        }

        std::vector<std::unique_ptr<Timer>>& timers()
        {
            static std::vector<std::unique_ptr<Timer>> list;
            return list;
        }

        // The main thread's side: the history and the totals since recording started.
        struct Recorder {
            // Recording, with the start of the frame under way known.
            bool active = false;
            int64_t frameStart = 0;
            std::vector<FrameRecord> ring;
            size_t head = 0;
            size_t count = 0;

            size_t frames = 0;
            double frameSum = 0;
            double frameWorst = 0;
            std::vector<float> frameTimes;
            double zoneSum[MaxZones] = {};
            double zoneWorst[MaxZones] = {};
            uint64_t zoneCalls[MaxZones] = {};
            double valueSum[MaxValues] = {};
            double valueWorst[MaxValues] = {};
            double valueLast[MaxValues] = {};

            bool timed = false;
            bool wasRecording = false;
            double timedSeconds = 0;
            int timedFrames = 0;
            std::function<void()> timedDone;
        };

        Recorder& recorder()
        {
            static Recorder state;
            return state;
        }

        // Frames since start that are kept one by one, for the 1% low; about 3 MB.
        constexpr size_t MaxFrameTimes = 800000;

        void drainZones()
        {
            int count = zoneCount.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++) {
                zoneTable[i].time.store(0, std::memory_order_relaxed);
                zoneTable[i].calls.store(0, std::memory_order_relaxed);
            }
        }

        double onePercentLow(std::vector<float> times)
        {
            if (times.empty()) {
                return 0;
            }
            size_t slowest = std::max<size_t>(1, times.size() / 100);
            std::partial_sort(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(slowest), times.end(), std::greater<float>());
            double sum = 0;
            for (size_t i = 0; i < slowest; i++) {
                sum += static_cast<double>(times[i]);
            }
            return sum / static_cast<double>(slowest);
        }

        void finishTimed(Recorder& state)
        {
            state.timed = false;
            recording.store(state.wasRecording, std::memory_order_relaxed);
            std::function<void()> done = std::move(state.timedDone);
            state.timedDone = nullptr;
            if (done) {
                done();
            }
        }

        // The recorder's first frame begins now.
        void start(Recorder& state, int64_t now)
        {
            state.active = true;
            state.frameStart = now;
            windowStart.store(now, std::memory_order_relaxed);
            drainZones();
            armed.store(true, std::memory_order_relaxed);
        }

        void recordFrame(Recorder& state, int64_t now)
        {
            if (state.ring.empty()) {
                state.ring.resize(HistoryFrames);
            }
            FrameRecord& record = state.ring[state.head];
            double frameMs = static_cast<double>(now - state.frameStart) / 1e6;
            state.frameStart = now;
            record.frameMs = static_cast<float>(frameMs);
            int count = zoneCount.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++) {
                double ms = static_cast<double>(zoneTable[i].time.exchange(0, std::memory_order_relaxed)) / 1e6;
                uint32_t calls = zoneTable[i].calls.exchange(0, std::memory_order_relaxed);
                record.zoneMs[i] = static_cast<float>(ms);
                record.zoneCalls[i] = calls;
                state.zoneSum[i] += ms;
                state.zoneWorst[i] = std::max(state.zoneWorst[i], ms);
                state.zoneCalls[i] += calls;
            }
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                for (int i = 0; i < valueCount; i++) {
                    double value = valueTable[i].current;
                    record.values[i] = static_cast<float>(value);
                    state.valueSum[i] += value;
                    state.valueWorst[i] = state.frames == 0 ? value : std::max(state.valueWorst[i], value);
                    state.valueLast[i] = value;
                }
            }
            state.head = (state.head + 1) % HistoryFrames;
            state.count = std::min(state.count + 1, HistoryFrames);
            state.frames++;
            state.frameSum += frameMs;
            state.frameWorst = std::max(state.frameWorst, frameMs);
            if (state.frameTimes.size() < MaxFrameTimes) {
                state.frameTimes.push_back(static_cast<float>(frameMs));
            }
        }

        std::string csvField(const std::string& text)
        {
            if (text.find_first_of(",\"\n") == std::string::npos) {
                return text;
            }
            std::string quoted = "\"";
            for (char c : text) {
                if (c == '"') {
                    quoted += '"';
                }
                quoted += c;
            }
            return quoted + "\"";
        }

        std::string number(double value)
        {
            char text[48];
            std::snprintf(text, sizeof(text), "%.4f", value);
            return text;
        }
    }

    int64_t nanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int enterZone(Site& site, int64_t& start)
    {
        if (!armed.load(std::memory_order_relaxed)) {
            return -1;
        }
        int thread = currentThread();
        if (openCount >= MaxDepth) {
            return -1;
        }
        int parent = openCount > 0 ? openZones[openCount - 1] : -1;
        // Roots are told apart by thread.
        int key = parent >= 0 ? parent : -1 - thread;
        uint64_t cached = site.cache.load(std::memory_order_relaxed);
        int zone = 0;
        if (cached != 0 && static_cast<int32_t>(cached >> 32) == key) {
            zone = static_cast<int>(static_cast<uint32_t>(cached)) - 1;
        } else {
            zone = findZone(site.name, parent, thread);
            if (zone < 0) {
                return -1;
            }
            site.cache.store(packCache(key, zone), std::memory_order_relaxed);
        }
        openZones[openCount++] = zone;
        start = nanoseconds();
        return zone;
    }

    void leaveZone(int zone, int64_t start)
    {
        int64_t elapsed = std::max<int64_t>(0, nanoseconds() - std::max(start, windowStart.load(std::memory_order_relaxed)));
        zoneTable[zone].time.fetch_add(elapsed, std::memory_order_relaxed);
        zoneTable[zone].calls.fetch_add(1, std::memory_order_relaxed);
        openCount--;
    }

    Timer& timer(const char* name)
    {
        for (const std::unique_ptr<Timer>& existing : timers()) {
            if (std::strcmp(existing->name, name) == 0) {
                return *existing;
            }
        }
        timers().push_back(std::make_unique<Timer>());
        Timer& made = *timers().back();
        made.name = name;
#if DX_PROFILE
        TracyPlotConfig(name, tracy::PlotFormatType::Number, false, true, 0);
#endif
        return made;
    }

    Timer& siteTimer(TimeSite& site)
    {
        if (!site.timer) {
            site.timer = &timer(site.name);
        }
        return *site.timer;
    }

    void plot(const char* name, double value)
    {
        std::lock_guard<std::mutex> lock(tableMutex);
        int index = findValue(name, false);
        if (index >= 0) {
            valueTable[index].current = value;
        }
    }

    void setThreadName(const char* name)
    {
        threadNames[currentThread()].store(name, std::memory_order_relaxed);
    }

    void endFrame()
    {
        Recorder& state = recorder();
        bool on = recording.load(std::memory_order_relaxed);
        bool keep = on && state.active;
        bool begun = std::exchange(frameBegun, false);
        int64_t now = nanoseconds();
        for (const std::unique_ptr<Timer>& running : timers()) {
            int64_t total = running->total;
            if (running->depth > 0) {
                // Still running at the frame's end: what has passed counts to this frame.
                total += now - running->start;
                running->start = now;
            }
#if DX_PROFILE
            TracyPlot(running->name, static_cast<double>(total) / 1e6);
#endif
            if (keep) {
                std::lock_guard<std::mutex> lock(tableMutex);
                if (running->value < 0) {
                    running->value = findValue(running->name, true);
                }
                if (running->value >= 0) {
                    valueTable[running->value].current = static_cast<double>(total) / 1e6;
                }
            }
            running->total = 0;
        }
        if (!on) {
            if (state.active) {
                state.active = false;
                armed.store(false, std::memory_order_relaxed);
                drainZones();
            }
            return;
        }
        if (!state.active) {
            // Turned on during this frame: what came before is not a whole frame. When the
            // frame's start was marked, the recorder begins at the next frame's start instead.
            if (!begun) {
                start(state, now);
            }
            return;
        }
        recordFrame(state, now);
        if (state.timed && ((state.timedFrames > 0 && state.frames >= static_cast<size_t>(state.timedFrames)) || (state.timedFrames <= 0 && state.frameSum >= state.timedSeconds * 1000.0))) {
            finishTimed(state);
        }
    }

    void beginFrame()
    {
        frameBegun = true;
        Recorder& state = recorder();
        if (!state.active && recording.load(std::memory_order_relaxed)) {
            int64_t now = nanoseconds();
            // Timers that ran before the frame's start are not part of it.
            for (const std::unique_ptr<Timer>& running : timers()) {
                running->total = 0;
                if (running->depth > 0) {
                    running->start = now;
                }
            }
            start(state, now);
        }
    }

    void setRecording(bool on)
    {
        recording.store(on, std::memory_order_relaxed);
    }

    bool isRecording()
    {
        return recording.load(std::memory_order_relaxed);
    }

    void reset()
    {
        Recorder& state = recorder();
        state.head = 0;
        state.count = 0;
        state.frames = 0;
        state.frameSum = 0;
        state.frameWorst = 0;
        state.frameTimes.clear();
        std::fill(std::begin(state.zoneSum), std::end(state.zoneSum), 0.0);
        std::fill(std::begin(state.zoneWorst), std::end(state.zoneWorst), 0.0);
        std::fill(std::begin(state.zoneCalls), std::end(state.zoneCalls), uint64_t { 0 });
        std::fill(std::begin(state.valueSum), std::end(state.valueSum), 0.0);
        std::fill(std::begin(state.valueWorst), std::end(state.valueWorst), 0.0);
        std::fill(std::begin(state.valueLast), std::end(state.valueLast), 0.0);
    }

    void recordFor(double seconds, int frames, std::function<void()> done)
    {
        Recorder& state = recorder();
        if (!state.timed) {
            state.wasRecording = isRecording();
        }
        reset();
        state.timed = true;
        state.timedSeconds = seconds;
        state.timedFrames = frames;
        state.timedDone = std::move(done);
        setRecording(true);
    }

    bool timedRecording(double& progress)
    {
        const Recorder& state = recorder();
        if (!state.timed) {
            progress = 0;
            return false;
        }
        if (state.timedFrames > 0) {
            progress = static_cast<double>(state.frames) / static_cast<double>(state.timedFrames);
        } else {
            progress = state.timedSeconds > 0 ? state.frameSum / (state.timedSeconds * 1000.0) : 1.0;
        }
        progress = std::clamp(progress, 0.0, 1.0);
        return true;
    }

    std::vector<ZoneInfo> zones()
    {
        int count = zoneCount.load(std::memory_order_acquire);
        std::vector<ZoneInfo> list(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) {
            const Zone& zone = zoneTable[i];
            ZoneInfo& info = list[static_cast<size_t>(i)];
            info.name = zone.name;
            info.parent = zone.parent;
            info.depth = zone.depth;
            const char* thread = threadNames[zone.thread].load(std::memory_order_relaxed);
            info.thread = thread ? thread : "";
        }
        return list;
    }

    std::vector<ValueInfo> values()
    {
        std::lock_guard<std::mutex> lock(tableMutex);
        std::vector<ValueInfo> list(static_cast<size_t>(valueCount));
        for (int i = 0; i < valueCount; i++) {
            list[static_cast<size_t>(i)] = { valueTable[i].name, valueTable[i].time };
        }
        return list;
    }

    size_t historySize()
    {
        return recorder().count;
    }

    const FrameRecord& historyFrame(size_t index)
    {
        const Recorder& state = recorder();
        size_t oldest = (state.head + HistoryFrames - state.count) % HistoryFrames;
        return state.ring[(oldest + index) % HistoryFrames];
    }

    Summary summarize(bool sinceStart)
    {
        const Recorder& state = recorder();
        size_t zoneTotal = static_cast<size_t>(zoneCount.load(std::memory_order_acquire));
        size_t valueTotal = values().size();
        Summary summary;
        summary.zones.resize(zoneTotal);
        summary.values.resize(valueTotal);
        if (sinceStart) {
            summary.frames = state.frames;
            if (state.frames == 0) {
                return summary;
            }
            double frames = static_cast<double>(state.frames);
            summary.meanFrameMs = state.frameSum / frames;
            summary.worstFrameMs = state.frameWorst;
            summary.onePercentLowMs = onePercentLow(state.frameTimes);
            for (size_t i = 0; i < zoneTotal; i++) {
                summary.zones[i].meanMs = state.zoneSum[i] / frames;
                summary.zones[i].worstMs = state.zoneWorst[i];
                summary.zones[i].callsPerFrame = static_cast<double>(state.zoneCalls[i]) / frames;
            }
            for (size_t i = 0; i < valueTotal; i++) {
                summary.values[i] = { state.valueSum[i] / frames, state.valueWorst[i], state.valueLast[i] };
            }
        } else {
            summary.frames = state.count;
            if (state.count == 0) {
                return summary;
            }
            std::vector<float> times;
            times.reserve(state.count);
            double frameSum = 0;
            std::vector<double> calls(zoneTotal);
            for (size_t f = 0; f < state.count; f++) {
                const FrameRecord& record = historyFrame(f);
                times.push_back(record.frameMs);
                frameSum += static_cast<double>(record.frameMs);
                summary.worstFrameMs = std::max(summary.worstFrameMs, static_cast<double>(record.frameMs));
                for (size_t i = 0; i < zoneTotal; i++) {
                    summary.zones[i].meanMs += static_cast<double>(record.zoneMs[i]);
                    summary.zones[i].worstMs = std::max(summary.zones[i].worstMs, static_cast<double>(record.zoneMs[i]));
                    calls[i] += record.zoneCalls[i];
                }
                for (size_t i = 0; i < valueTotal; i++) {
                    double value = record.values[i];
                    summary.values[i].mean += value;
                    summary.values[i].worst = f == 0 ? value : std::max(summary.values[i].worst, value);
                    summary.values[i].last = value;
                }
            }
            double frames = static_cast<double>(state.count);
            summary.meanFrameMs = frameSum / frames;
            summary.onePercentLowMs = onePercentLow(std::move(times));
            for (size_t i = 0; i < zoneTotal; i++) {
                summary.zones[i].meanMs /= frames;
                summary.zones[i].callsPerFrame = calls[i] / frames;
            }
            for (size_t i = 0; i < valueTotal; i++) {
                summary.values[i].mean /= frames;
            }
        }
        for (ZoneStats& zone : summary.zones) {
            zone.share = summary.meanFrameMs > 0 ? zone.meanMs / summary.meanFrameMs : 0;
        }
        return summary;
    }

    std::string zonePath(const std::vector<ZoneInfo>& list, int zone)
    {
        std::string path;
        for (int at = zone; at >= 0; at = list[static_cast<size_t>(at)].parent) {
            path = path.empty() ? std::string(list[static_cast<size_t>(at)].name) : std::string(list[static_cast<size_t>(at)].name) + "/" + path;
        }
        return path;
    }

    std::string csv()
    {
        std::vector<ZoneInfo> zoneList = zones();
        std::vector<ValueInfo> valueList = values();
        Summary summary = summarize(true);
        std::string out;
        out += "Ambrosia frame recording\n";
        out += "frames," + std::to_string(summary.frames) + ",mean ms," + number(summary.meanFrameMs) + ",worst ms," + number(summary.worstFrameMs) + ",1% low ms," + number(summary.onePercentLowMs) + "\n\n";
        out += "zone,thread,depth,mean ms,worst ms,share of frame,calls per frame\n";
        for (size_t i = 0; i < zoneList.size(); i++) {
            const ZoneStats& stats = summary.zones[i];
            out += csvField(zonePath(zoneList, static_cast<int>(i))) + "," + csvField(zoneList[i].thread) + "," + std::to_string(zoneList[i].depth) + "," + number(stats.meanMs) + "," + number(stats.worstMs) + "," + number(stats.share) + "," + number(stats.callsPerFrame) + "\n";
        }
        out += "\nvalue,kind,mean,worst,last\n";
        for (size_t i = 0; i < valueList.size(); i++) {
            const ValueStats& stats = summary.values[i];
            out += csvField(valueList[i].name) + "," + (valueList[i].time ? "time ms" : "plot") + "," + number(stats.mean) + "," + number(stats.worst) + "," + number(stats.last) + "\n";
        }
        out += "\nframe,frame ms";
        for (size_t i = 0; i < zoneList.size(); i++) {
            out += "," + csvField(zonePath(zoneList, static_cast<int>(i)));
        }
        for (const ValueInfo& value : valueList) {
            out += "," + csvField(value.name);
        }
        out += "\n";
        size_t kept = historySize();
        size_t first = summary.frames - kept;
        for (size_t f = 0; f < kept; f++) {
            const FrameRecord& record = historyFrame(f);
            out += std::to_string(first + f) + "," + number(record.frameMs);
            for (size_t i = 0; i < zoneList.size(); i++) {
                out += "," + number(record.zoneMs[i]);
            }
            for (size_t i = 0; i < valueList.size(); i++) {
                out += "," + number(record.values[i]);
            }
            out += "\n";
        }
        return out;
    }

    bool writeCsv(const std::string& path, std::string& error)
    {
        std::filesystem::path file(path);
        std::error_code code;
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path(), code);
            if (code) {
                error = "cannot create " + file.parent_path().string() + ": " + code.message();
                return false;
            }
        }
        std::ofstream out(file, std::ios::binary);
        if (!out) {
            error = "cannot write " + path;
            return false;
        }
        std::string text = csv();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = "cannot write " + path;
            return false;
        }
        return true;
    }
}
