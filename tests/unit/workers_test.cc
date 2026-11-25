#include "core/config.h"
#include "core/platform.h"
#include "core/workers.h"

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using dx::workers::Worker;

namespace {
    // Waits up to two seconds for a condition another thread makes true.
    template <typename Condition>
    bool eventually(Condition condition)
    {
        auto end = std::chrono::steady_clock::now() + 2s;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    bool listed(const std::string& name)
    {
        for (const std::string& running : dx::workers::runningNames()) {
            if (running == name) {
                return true;
            }
        }
        return false;
    }

    // A body's own wake: pending wakes, and the condition it sleeps on until woken or stopped,
    // as the acoustics worker sleeps on its inputs.
    struct Wakes {
        std::mutex mutex;
        std::condition_variable_any signal;
        int pending = 0;

        void wake()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending++;
            }
            signal.notify_all();
        }

        // False once the stop is requested.
        bool wait(const std::stop_token& stop)
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!signal.wait(lock, stop, [this] { return pending > 0; })) {
                return false;
            }
            pending--;
            return true;
        }
    };

    // A body that sleeps until stopped.
    void sleepUntilStopped(const std::stop_token& stop)
    {
        std::mutex mutex;
        std::condition_variable_any never;
        std::unique_lock<std::mutex> lock(mutex);
        never.wait(lock, stop, [] { return false; });
    }
}

TEST_CASE("a worker runs its body on its own thread until stopped", "[workers]")
{
    std::atomic<int> passes { 0 };
    std::thread::id body;
    Wakes wakes;
    Worker worker;
    CHECK_FALSE(worker.running());
    worker.start("Test worker", [&](std::stop_token stop) {
        body = std::this_thread::get_id();
        while (wakes.wait(stop)) {
            passes++;
        }
    });
    CHECK(worker.running());
    CHECK(worker.name() == "Test worker");
    CHECK(listed("Test worker"));
    wakes.wake();
    REQUIRE(eventually([&] { return passes == 1; }));
    wakes.wake();
    REQUIRE(eventually([&] { return passes == 2; }));
    worker.stop();
    CHECK_FALSE(worker.running());
    CHECK_FALSE(listed("Test worker"));
    CHECK(body != std::this_thread::get_id());
    CHECK(passes == 2);
    // Stopping again, or a worker never started, does nothing.
    worker.stop();
    Worker idle;
    idle.stop();
    idle.join();
}

TEST_CASE("stopping a worker ends a timed wait on its token at once", "[workers]")
{
    std::atomic<bool> waiting { false };
    std::atomic<bool> timedOut { true };
    Worker worker;
    worker.start("Timed worker", [&](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any signal;
        std::unique_lock<std::mutex> lock(mutex);
        waiting = true;
        // An hour, which only the stop ends.
        signal.wait_for(lock, stop, 1h, [] { return false; });
        timedOut = !stop.stop_requested();
    });
    REQUIRE(eventually([&] { return waiting.load(); }));
    auto start = std::chrono::steady_clock::now();
    worker.stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK_FALSE(timedOut);
}

TEST_CASE("stopping a busy worker waits for its pass to end", "[workers]")
{
    std::atomic<bool> busy { false };
    std::atomic<bool> finished { false };
    Worker worker;
    worker.start("Busy worker", [&](std::stop_token stop) {
        busy = true;
        // A long pass that only looks at the stop between steps.
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(20ms);
        finished = true;
    });
    REQUIRE(eventually([&] { return busy.load(); }));
    worker.stop();
    CHECK(finished);
    CHECK_FALSE(worker.running());
}

TEST_CASE("a worker sleeping on a condition of its own is woken by a stop callback", "[workers]")
{
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    int passes = 0;
    std::vector<std::string> order;
    Worker worker;
    worker.start("Own condition", [&](std::stop_token stop) {
        // The callback runs on the stopping thread, after the stop is requested.
        std::stop_callback interrupt(stop, [&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                order.push_back("callback");
            }
            wake.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            wake.wait(lock, [&] { return stopping || passes > 0; });
            if (passes > 0) {
                passes--;
            }
        }
        order.push_back(stop.stop_requested() ? "stopped after request" : "stopped before request");
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        passes = 1;
    }
    wake.notify_all();
    worker.stop();
    CHECK(order == std::vector<std::string> { "callback", "stopped after request" });
}

TEST_CASE("join waits for a body that ends by itself, and a worker starts again after", "[workers]")
{
    std::atomic<int> runs { 0 };
    Worker worker;
    for (int i = 0; i < 3; i++) {
        worker.start("Short job", [&](std::stop_token stop) {
            CHECK_FALSE(stop.stop_requested());
            runs++;
        });
        worker.join();
        CHECK_FALSE(worker.running());
        CHECK(runs == i + 1);
    }
    // Starting a running worker stops the first run before the second.
    std::atomic<int> stopped { 0 };
    auto body = [&](std::stop_token stop) {
        sleepUntilStopped(stop);
        stopped++;
    };
    worker.start("Restarted", body);
    worker.start("Restarted", body);
    CHECK(stopped == 1);
    worker.stop();
    CHECK(stopped == 2);
}

TEST_CASE("a worker is stopped before the rest of its owner goes", "[workers]")
{
    std::vector<std::string> order;
    std::mutex mutex;

    struct Resource {
        std::vector<std::string>& order;
        std::mutex& mutex;

        ~Resource()
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back("resource freed");
        }
    };

    struct Owner {
        std::unique_ptr<Resource> resource;
        // Last, as the header says, so it is destroyed first.
        Worker worker;
    };

    {
        Owner owner { std::unique_ptr<Resource>(new Resource { order, mutex }), {} };
        Resource* used = owner.resource.get();
        owner.worker.start("Owned worker", [used, &order, &mutex](std::stop_token stop) {
            sleepUntilStopped(stop);
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(used ? "worker done" : "no resource");
        });
    }
    CHECK(order == std::vector<std::string> { "worker done", "resource freed" });
}

TEST_CASE("stopAll stops the workers still running, newest first", "[workers]")
{
    std::vector<std::string> order;
    std::mutex mutex;
    auto body = [&](const std::string& name) {
        return [&order, &mutex, name](std::stop_token stop) {
            sleepUntilStopped(stop);
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(name);
        };
    };
    Worker first;
    Worker second;
    Worker third;
    first.start("First", body("first"));
    second.start("Second", body("second"));
    third.start("Third", body("third"));
    second.stop();
    CHECK(dx::workers::stopAll() == 2);
    CHECK(order == std::vector<std::string> { "second", "third", "first" });
    CHECK_FALSE(first.running());
    CHECK_FALSE(third.running());
    CHECK(dx::workers::stopAll() == 0);
}

TEST_CASE("the background thread cap comes from the setting or the platform", "[workers]")
{
    dx::Ini& ini = dx::config::system();
    std::optional<std::string> saved = ini.get("Engine.Engine", "AmbrosiaBackgroundThreads");
    ini.set("Engine.Engine", "AmbrosiaBackgroundThreads", "");
    CHECK(dx::workers::threadCap() == std::max(1, dx::platform::backgroundThreads()));
    CHECK(dx::workers::threadCap() >= 1);
    ini.set("Engine.Engine", "AmbrosiaBackgroundThreads", "3");
    CHECK(dx::workers::threadCap() == 3);
    ini.set("Engine.Engine", "AmbrosiaBackgroundThreads", "0");
    CHECK(dx::workers::threadCap() == std::max(1, dx::platform::backgroundThreads()));
    ini.set("Engine.Engine", "AmbrosiaBackgroundThreads", saved.value_or(""));
}
