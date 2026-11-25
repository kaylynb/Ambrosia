#pragma once

#include <string>
#include <vector>

namespace dx::knownlogins {
    // Which texts and conversations give away which logins, PINs and door codes, from the
    // table in assets/deusex/known_logins.tsv, and which of them the player has read or
    // heard in the game being played. Sources are named "text:<name>" for datacubes, books,
    // newspapers, emails and bulletins, and "con:<name>" for conversations.

    // Records a source as read or heard (reading.h watches for them). The set is part of the
    // game state (game_state.h), so it is saved and loaded with the game.
    void markRead(const std::string& source);
    // The accounts on a machine that the player has learnt, in table order; for a keypad, an
    // empty string when its code is known. map is the map's file name, machine the actor name.
    std::vector<std::string> knownAccounts(const std::string& map, const std::string& machine);
}
