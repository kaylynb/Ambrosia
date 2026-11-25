#include "core/workers.h"

#include "core/settings.h"
#include "core/platform.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace dx::workers {
    struct Worker::State {
        std::string name;
        std::jthread thread;
    };

    namespace {
        // Running workers, oldest first. Never freed, so a worker owned by a static can still
        // leave the list at exit whatever order statics go in.
        struct Registry {
            DX_LOCKABLE(mutex, "Worker list");
            std::vector<Worker::State*> running;
        };

        Registry& registry()
        {
            static Registry* instance = new Registry;
            return *instance;
        }

        // Joins the thread and takes it off the list, ready to start again.
        void joinState(Worker::State& state)
        {
            state.thread.join();
            std::lock_guard<dx::profile::Mutex> lock(registry().mutex);
            std::erase(registry().running, &state);
        }

        void stopState(Worker::State& state)
        {
            if (!state.thread.joinable()) {
                return;
            }
            state.thread.request_stop();
            joinState(state);
        }
    }

    int32_t threadCap()
    {
        int32_t threads = settings::get(settings::backgroundThreads);
        return threads >= 1 ? threads : std::max(1, platform::backgroundThreads());
    }

    Worker::Worker()
        : state(std::make_unique<State>())
    {
    }

    Worker::~Worker()
    {
        stop();
    }

    void Worker::start(std::string name, std::function<void(std::stop_token)> body)
    {
        stop();
        state->name = std::move(name);
        {
            std::lock_guard<dx::profile::Mutex> lock(registry().mutex);
            registry().running.push_back(state.get());
        }
        const std::string* threadName = &state->name;
        state->thread = std::jthread([threadName, body = std::move(body)](std::stop_token token) {
            DX_THREAD_NAME(threadName->c_str());
            body(std::move(token));
        });
    }

    void Worker::stop()
    {
        stopState(*state);
    }

    void Worker::join()
    {
        if (state->thread.joinable()) {
            joinState(*state);
        }
    }

    bool Worker::running() const
    {
        return state->thread.joinable();
    }

    const std::string& Worker::name() const
    {
        return state->name;
    }

    std::vector<std::string> runningNames()
    {
        std::lock_guard<dx::profile::Mutex> lock(registry().mutex);
        std::vector<std::string> names;
        for (const Worker::State* state : registry().running) {
            names.push_back(state->name);
        }
        return names;
    }

    size_t stopAll()
    {
        std::vector<Worker::State*> running;
        {
            std::lock_guard<dx::profile::Mutex> lock(registry().mutex);
            running = registry().running;
        }
        for (auto it = running.rbegin(); it != running.rend(); ++it) {
            stopState(**it);
        }
        return running.size();
    }
}
