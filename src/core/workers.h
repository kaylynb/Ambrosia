#pragma once

#include "core/profile.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

// Long-lived named threads for background work, and how many threads such work may use.
//
// A Worker owns one std::jthread running a body until the body returns. The body is given the
// thread's std::stop_token: it looks at stop_requested() between steps, sleeps on a
// std::condition_variable_any with the token (wait(lock, token, pred), which a stop wakes), or
// wakes whatever else it sleeps on from a std::stop_callback. stop() requests the stop and joins.
//
// What the Worker adds to the jthread: the thread's name in the profiler, and a list of the
// running workers. A worker is stopped and joined by stop() or by its destructor, so an object
// owning one stops it when it goes; a member Worker declared last in its owner is stopped before
// the owner's other members go. stopAll() stops those still running, newest first, for a
// program's exit after its own owners have gone.
//
// No thread pool, task queue or priorities: each worker is one thread doing one job.
namespace dx::workers {
    // The most threads background work may use at once, at least 1: AmbrosiaBackgroundThreads
    // under [Engine.Engine] in the system ini when it is 1 or more, else the platform's own
    // number (platform::backgroundThreads()).
    int32_t threadCap();

    class Worker {
    public:
        Worker();
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        ~Worker();

        // Starts the thread, named name in the profiler, running body. A worker already running
        // is stopped first.
        void start(std::string name, std::function<void(std::stop_token)> body);
        // Requests the body's stop and waits for its thread to end. Nothing when not started.
        // Never from the worker's own thread.
        void stop();
        // Waits for the body to return of itself, without requesting a stop. Nothing when not
        // started.
        void join();
        // Whether the thread has been started and not yet joined.
        bool running() const;
        const std::string& name() const;

        // The thread and its name, in workers.cc.
        struct State;

    private:
        std::unique_ptr<State> state;
    };

    // The names of the workers started and not yet stopped or joined, oldest first.
    std::vector<std::string> runningNames();
    // Stops every worker still running, newest first, and says how many there were.
    size_t stopAll();
}
