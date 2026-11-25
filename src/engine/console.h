#pragma once

#include "core/object.h"

#include <cstdint>
#include <functional>
#include <string>

namespace dx {
    namespace console {
        std::string execute(UObject* context, const std::string& command);
        void loadBindings();
        bool key(uint8_t key, uint8_t action, float delta);
        // Current position of an analog input such as a gamepad stick; readInput feeds it to
        // the key's binding each tick as value * deltaSeconds.
        void analog(uint8_t key, float value);
        // An amount for one of the player's input axes (such as "aTurn"), given once at the next
        // tick that reads input, whatever its length: an analog key's value times the tick's
        // seconds, as a bound key would give, without needing a binding. Amounts for the same axis
        // add up until a tick reads them, so a turn made over frames the world gathers into one
        // tick is neither lost nor counted twice.
        void heldAxis(const std::string& variable, float amount);
        void readInput(UObject* player, float deltaSeconds);
        void resetInput(UObject* player);
        std::string binding(uint8_t key);
        std::string keyName(uint8_t key);
        uint8_t keyFromName(const std::string& name);

        extern std::function<void(const std::string&, int32_t)> openMapHook;
        extern std::function<void()> quitHook;
        extern std::function<std::string()> currentResHook;
        extern std::function<std::string()> resolutionsHook;
        extern std::function<void(int, int)> setResHook;
        extern std::function<void()> toggleFullscreenHook;
        extern std::function<void()> screenshotHook;
        // The game's developer overlay: given "on" or "interactive", "passive", "off", or "" to open it
        // or switch it between interactive and passive, returns what it did.
        extern std::function<std::string(const std::string&)> debugOverlayHook;
    }
}
