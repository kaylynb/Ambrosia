#include "engine/light_effects.h"

#include "core/object.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <cmath>
#include <cstdint>

namespace dx {
    bool strobeDark(UObject* light, float time)
    {
        static float lastUpdateTime = 0;
        static int32_t toggle = 0;
        static bool steadyDark = false;
        UObject* level = light && light->data ? actor::Level(light) : nullptr;
        bool paused = false;
        if (level && level->data) {
            time = levelinfo::TimeSeconds(level);
            paused = !levelinfo::Pauser(level).empty();
        }
        if (steadyTiming()) {
            // Level time runs on while the game is paused, as behind a menu, so the original
            // goes on strobing; Steady holds the light as it was.
            if (!paused) {
                steadyDark = static_cast<int64_t>(std::floor(static_cast<double>(time) * SteadyStrobeRate)) % 2 != 0;
            }
            return steadyDark;
        }
        if (lastUpdateTime != time) {
            lastUpdateTime = time;
            toggle ^= 1;
        }
        return toggle != 0;
    }
}
