#include "core/profile.h"
#include "render/render_debug.h"

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace profile = dx::profile;

namespace {
    // The zones of that name under that parent (-1 for roots), as indices.
    std::vector<int> find(const char* name, int parent = -2)
    {
        std::vector<int> found;
        std::vector<profile::ZoneInfo> zones = profile::zones();
        for (size_t i = 0; i < zones.size(); i++) {
            if (std::strcmp(zones[i].name, name) == 0 && (parent == -2 || zones[i].parent == parent)) {
                found.push_back(static_cast<int>(i));
            }
        }
        return found;
    }

    int findValue(const char* name)
    {
        std::vector<profile::ValueInfo> values = profile::values();
        for (size_t i = 0; i < values.size(); i++) {
            if (std::strcmp(values[i].name, name) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void spin(std::chrono::microseconds length)
    {
        auto until = std::chrono::steady_clock::now() + length;
        while (std::chrono::steady_clock::now() < until) {
        }
    }

    // Recording, with a frame under way.
    void startRecording()
    {
        profile::setRecording(false);
        profile::endFrame();
        profile::reset();
        profile::setRecording(true);
        profile::endFrame();
    }

    void nested()
    {
        DX_ZONE_NAMED("Recorder test outer");
        spin(std::chrono::microseconds(200));
        for (int i = 0; i < 2; i++) {
            DX_ZONE_NAMED("Recorder test inner");
            spin(std::chrono::microseconds(100));
        }
    }
}

TEST_CASE("Frame recorder nests zones and resets them each frame", "[profile]")
{
    startRecording();
    nested();
    profile::endFrame();

    std::vector<int> outer = find("Recorder test outer", -1);
    REQUIRE(outer.size() == 1);
    std::vector<int> inner = find("Recorder test inner", outer[0]);
    REQUIRE(inner.size() == 1);
    CHECK(profile::zones()[static_cast<size_t>(inner[0])].depth == 1);
    REQUIRE(profile::historySize() == 1);

    const profile::FrameRecord& first = profile::historyFrame(0);
    CHECK(first.zoneCalls[outer[0]] == 1);
    CHECK(first.zoneCalls[inner[0]] == 2);
    CHECK(first.zoneMs[inner[0]] >= 0.19f);
    CHECK(first.zoneMs[outer[0]] >= first.zoneMs[inner[0]] + 0.19f);
    CHECK(first.frameMs >= first.zoneMs[outer[0]]);

    // A frame without the zones has none of their time.
    profile::endFrame();
    REQUIRE(profile::historySize() == 2);
    const profile::FrameRecord& second = profile::historyFrame(1);
    CHECK(second.zoneCalls[outer[0]] == 0);
    CHECK(second.zoneMs[outer[0]] == 0.0f);

    // The same zone met under another parent is another zone.
    {
        DX_ZONE_NAMED("Recorder test other parent");
        DX_ZONE_NAMED("Recorder test inner");
    }
    profile::endFrame();
    CHECK(find("Recorder test inner").size() == 2);

    profile::Summary summary = profile::summarize(false);
    CHECK(summary.frames == 3);
    CHECK(summary.zones[static_cast<size_t>(inner[0])].callsPerFrame == Catch::Approx(2.0 / 3.0));
    CHECK(summary.zones[static_cast<size_t>(outer[0])].worstMs == Catch::Approx(first.zoneMs[outer[0]]));
    CHECK(summary.onePercentLowMs >= summary.meanFrameMs);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder keeps nothing while off", "[profile]")
{
    profile::setRecording(false);
    profile::endFrame();
    profile::reset();
    {
        DX_ZONE_NAMED("Recorder test never recorded");
        DX_PLOT("Recorder test never plotted", 1);
    }
    profile::endFrame();
    CHECK(find("Recorder test never recorded").empty());
    CHECK(findValue("Recorder test never plotted") < 0);
    CHECK(profile::historySize() == 0);

    // Turned on, the frame under way is not whole and is left out.
    profile::setRecording(true);
    profile::endFrame();
    CHECK(profile::historySize() == 0);
    profile::endFrame();
    CHECK(profile::historySize() == 1);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder history keeps the latest frames", "[profile]")
{
    startRecording();
    for (size_t i = 0; i < profile::HistoryFrames + 50; i++) {
        DX_PLOT("Recorder test frame number", i);
        profile::endFrame();
    }
    CHECK(profile::historySize() == profile::HistoryFrames);
    int value = findValue("Recorder test frame number");
    REQUIRE(value >= 0);
    CHECK(profile::historyFrame(0).values[value] == 50.0f);
    CHECK(profile::historyFrame(profile::HistoryFrames - 1).values[value] == static_cast<float>(profile::HistoryFrames + 49));
    CHECK(profile::summarize(true).frames == profile::HistoryFrames + 50);
    CHECK(profile::summarize(false).frames == profile::HistoryFrames);

    profile::reset();
    CHECK(profile::historySize() == 0);
    CHECK(profile::summarize(true).frames == 0);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder takes timers and plots", "[profile]")
{
    startRecording();
    {
        DX_TIME("Recorder test timer");
        spin(std::chrono::microseconds(300));
        // Nested scopes of the same name count once.
        DX_TIME("Recorder test timer");
        spin(std::chrono::microseconds(100));
    }
    DX_PLOT("Recorder test plot", 7);
    profile::endFrame();
    int timer = findValue("Recorder test timer");
    int plotted = findValue("Recorder test plot");
    REQUIRE(timer >= 0);
    REQUIRE(plotted >= 0);
    CHECK(profile::values()[static_cast<size_t>(timer)].time);
    CHECK_FALSE(profile::values()[static_cast<size_t>(plotted)].time);
    const profile::FrameRecord& frame = profile::historyFrame(profile::historySize() - 1);
    CHECK(frame.values[timer] >= 0.39f);
    CHECK(frame.values[timer] < frame.frameMs + 0.01f);
    CHECK(frame.values[plotted] == 7.0f);
    // A plot keeps its value; a timer starts each frame from nothing.
    profile::endFrame();
    const profile::FrameRecord& next = profile::historyFrame(profile::historySize() - 1);
    CHECK(next.values[plotted] == 7.0f);
    CHECK(next.values[timer] == 0.0f);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder writes CSV", "[profile]")
{
    startRecording();
    nested();
    {
        DX_ZONE_NAMED("Recorder test, \"quoted\"");
    }
    DX_PLOT("Recorder test plot", 3);
    profile::endFrame();
    profile::endFrame();
    std::string text = profile::csv();
    std::istringstream lines(text);
    std::string line;
    REQUIRE(std::getline(lines, line));
    CHECK(line == "Ambrosia frame recording");
    REQUIRE(std::getline(lines, line));
    CHECK(line.rfind("frames,2,mean ms,", 0) == 0);
    CHECK(text.find("\nzone,thread,depth,mean ms,worst ms,share of frame,calls per frame\n") != std::string::npos);
    CHECK(text.find("\nRecorder test outer/Recorder test inner,") != std::string::npos);
    CHECK(text.find("\n\"Recorder test, \"\"quoted\"\"\",") != std::string::npos);
    CHECK(text.find("\nvalue,kind,mean,worst,last\n") != std::string::npos);
    CHECK(text.find("\nRecorder test plot,plot,3.0000,3.0000,3.0000\n") != std::string::npos);

    // The history: a header, then one row per frame with a column for each zone and value.
    size_t header = text.find("\nframe,frame ms,");
    REQUIRE(header != std::string::npos);
    std::istringstream history(text.substr(header + 1));
    std::vector<std::string> rows;
    while (std::getline(history, line)) {
        rows.push_back(line);
    }
    REQUIRE(rows.size() == 3);
    // Count fields outside quotes.
    auto fields = [](const std::string& row) {
        size_t count = 1;
        bool quoted = false;
        for (char c : row) {
            if (c == '"') {
                quoted = !quoted;
            } else if (c == ',' && !quoted) {
                count++;
            }
        }
        return count;
    };
    size_t columns = 2 + profile::zones().size() + profile::values().size();
    CHECK(fields(rows[0]) == columns);
    CHECK(fields(rows[1]) == columns);
    CHECK(rows[1].rfind("0,", 0) == 0);
    CHECK(rows[2].rfind("1,", 0) == 0);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder counts zones on other threads", "[profile]")
{
    startRecording();
    constexpr int Threads = 4;
    constexpr int Calls = 20000;
    std::atomic<bool> go { false };
    std::atomic<int> finished { 0 };
    std::vector<std::thread> workers;
    for (int t = 0; t < Threads; t++) {
        workers.emplace_back([&go, &finished] {
            DX_THREAD_NAME("Recorder test worker");
            while (!go.load()) {
            }
            for (int i = 0; i < Calls; i++) {
                DX_ZONE_NAMED("Recorder test thread outer");
                DX_ZONE_NAMED("Recorder test thread inner");
            }
            finished.fetch_add(1);
        });
    }
    go.store(true);
    // Frames end on this thread while the workers run.
    while (finished.load() < Threads) {
        profile::endFrame();
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    profile::endFrame();

    profile::Summary summary = profile::summarize(true);
    double frames = static_cast<double>(summary.frames);
    std::vector<int> roots = find("Recorder test thread outer", -1);
    REQUIRE(!roots.empty());
    double outerCalls = 0;
    double innerCalls = 0;
    for (int root : roots) {
        CHECK(profile::zones()[static_cast<size_t>(root)].thread == "Recorder test worker");
        outerCalls += summary.zones[static_cast<size_t>(root)].callsPerFrame * frames;
        for (int inner : find("Recorder test thread inner", root)) {
            innerCalls += summary.zones[static_cast<size_t>(inner)].callsPerFrame * frames;
        }
    }
    CHECK(outerCalls == Catch::Approx(Threads * Calls));
    CHECK(innerCalls == Catch::Approx(Threads * Calls));
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Frame recorder timed recordings stop by themselves", "[profile]")
{
    profile::setRecording(false);
    profile::endFrame();
    int done = 0;
    profile::recordFor(0, 3, [&done] {
        done++;
    });
    double progress = 0;
    CHECK(profile::timedRecording(progress));
    CHECK(profile::isRecording());
    // The first frame's end starts the recording; three whole frames follow.
    for (int i = 0; i < 3; i++) {
        profile::endFrame();
        CHECK(done == 0);
    }
    profile::endFrame();
    CHECK(done == 1);
    CHECK(profile::historySize() == 3);
    CHECK_FALSE(profile::timedRecording(progress));
    // It was off before, so it is off again.
    CHECK_FALSE(profile::isRecording());
    profile::endFrame();
    CHECK(done == 1);
}

// Not run by default: how long a zone takes with the recorder off and on, for comparing builds.
TEST_CASE("Frame recorder zone cost", "[.][benchmark]")
{
    constexpr int Count = 50000000;
    auto run = [](bool on, bool zone) {
        profile::setRecording(on);
        profile::endFrame();
        profile::endFrame();
        volatile int sink = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < Count; i++) {
            if (zone) {
                DX_ZONE_NAMED("Recorder benchmark zone");
                sink = sink + 1;
            } else {
                sink = sink + 1;
            }
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        profile::setRecording(false);
        profile::endFrame();
        return seconds * 1e9 / Count;
    };
    double empty = run(false, false);
    double off = run(false, true);
    double on = run(true, true);
    std::printf("ns per loop: no zone %.2f, zone with the recorder off %.2f, on %.2f\n", empty, off, on);
    std::printf("so a zone costs %.2f ns off and %.2f ns on\n", off - empty, on - empty);
}

TEST_CASE("Frame recorder turned on mid-frame starts at the next frame", "[profile]")
{
    profile::setRecording(false);
    profile::endFrame();
    profile::reset();
    auto frame = [](bool turnOn) {
        DX_FRAME_ZONE("Recorder test frame");
        {
            DX_ZONE_NAMED("Recorder test before");
        }
        if (turnOn) {
            profile::setRecording(true);
        }
        {
            DX_ZONE_NAMED("Recorder test after");
        }
        profile::endFrame();
        DX_ZONE_NAMED("Recorder test after the mark");
    };
    frame(true);
    // Nothing of the frame it was turned on in, whose own zone it never saw begin.
    CHECK(find("Recorder test after").empty());
    CHECK(find("Recorder test after the mark").empty());
    CHECK(profile::historySize() == 0);
    frame(false);
    frame(false);
    std::vector<int> root = find("Recorder test frame", -1);
    REQUIRE(root.size() == 1);
    CHECK(find("Recorder test before", root[0]).size() == 1);
    CHECK(find("Recorder test after", root[0]).size() == 1);
    CHECK(find("Recorder test after the mark", root[0]).size() == 1);
    CHECK(find("Recorder test after the mark", -1).empty());
    CHECK(profile::historySize() == 2);
    profile::setRecording(false);
    profile::endFrame();
}

TEST_CASE("Render debug overrides start on the normal path", "[profile]")
{
    dx::RenderDebug defaults;
    for (bool flag : { defaults.mirrors, defaults.sky, defaults.lightmapAnimation, defaults.proceduralTextures, defaults.actorMeshes, defaults.translucent, defaults.occlusionQueries, defaults.coronas, defaults.postChain, defaults.gamma, defaults.hudScripts }) {
        CHECK(flag);
    }
    // Every field is one of those flags, so a new one has to be added above.
    CHECK(sizeof(dx::RenderDebug) == 11 * sizeof(bool));
    dx::RenderCounters counters;
    CHECK(counters.drawCalls == 0);
    CHECK(counters.triangles == 0);
}
