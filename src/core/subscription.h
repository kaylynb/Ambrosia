#pragma once

#include <cstdint>
#include <utility>

namespace dx {
    // A listener's place on some list of listeners, given up when this goes or is reset. The list
    // hands it out with the function that takes an id off it.
    class Subscription {
    public:
        using Cancel = void (*)(uint64_t id);

        Subscription() = default;

        Subscription(Cancel canceller, uint64_t listener)
            : cancel(canceller)
            , id(listener)
        {
        }

        Subscription(Subscription&& other) noexcept
            : cancel(std::exchange(other.cancel, nullptr))
            , id(std::exchange(other.id, 0))
        {
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other) {
                reset();
                cancel = std::exchange(other.cancel, nullptr);
                id = std::exchange(other.id, 0);
            }
            return *this;
        }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        ~Subscription()
        {
            reset();
        }

        void reset()
        {
            if (cancel) {
                std::exchange(cancel, nullptr)(std::exchange(id, 0));
            }
        }

    private:
        Cancel cancel = nullptr;
        uint64_t id = 0;
    };
}
