#pragma once

#include <string>

namespace dx::gamestate {
    // Ambrosia's own state for the game being played, such as the gamepad quick slots and the
    // texts the player has read: named sections of keys, each with an optional value. It is
    // saved on the end of each savegame's SaveInfo.dxs and comes back on loading (see install).

    // A key's value, empty if it has none or is not there.
    std::string get(const std::string& section, const std::string& key);
    // Sets a key's value; an empty value removes the key.
    void set(const std::string& section, const std::string& key, const std::string& value);
    // Whether a section has a key, with or without a value.
    bool has(const std::string& section, const std::string& key);
    // Adds a key with no value. Returns whether it was not there before.
    bool add(const std::string& section, const std::string& key);

    // The state as text, "[Section]" headers each followed by "key" or "key=value" lines.
    std::string save();
    // Replaces the state with text from save(); an empty text starts afresh.
    void restore(const std::string& text);
    // Hands the state to the savegame code, which saves and restores it with each game.
    void install();
}
