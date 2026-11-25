#pragma once

#include <utility>

namespace dx {
    // Sets a value for the length of a scope, then puts the old one back however the scope ends,
    // a throw included.
    template <typename T>
    class ScopedValue {
    public:
        ScopedValue(T& target, T value)
            : slot(target)
            , previous(std::exchange(target, std::move(value)))
        {
        }

        ~ScopedValue()
        {
            slot = std::move(previous);
        }

        ScopedValue(const ScopedValue&) = delete;
        ScopedValue& operator=(const ScopedValue&) = delete;

    private:
        T& slot;
        T previous;
    };
}
