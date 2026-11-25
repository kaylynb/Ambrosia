#include "core/random.h"

#include <catch_amalgamated.hpp>

namespace {
    // The game seeds its random streams from the clock; the tests use a fixed seed so every run
    // draws the same numbers.
    class TestSetup : public Catch::EventListenerBase {
    public:
        using Catch::EventListenerBase::EventListenerBase;

        void testRunStarting(const Catch::TestRunInfo&) override
        {
            dx::random::seed(1);
        }
    };
}

CATCH_REGISTER_LISTENER(TestSetup)
