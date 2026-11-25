#pragma once

#include <filesystem>
#include <string>

// Where the tests find the game data, the checkout and ucc, all worked out at run time so a build
// keeps working when the checkout is somewhere else, such as a container that mounts it elsewhere.
namespace dx::test {
    // The game data, found as the programs find it (config::findGameRoot): $DEUSEX_GAME_DATA, then
    // game-data beside unit-tests or in any folder above it. With forward slashes, so a test can
    // append "/System/..." on every platform. Throws, naming the places it looked, when there is
    // none, so every [data] test fails saying the data is missing before touching the engine.
    const std::string& gameData();

    // The checkout: the first folder above unit-tests that holds CMakePresets.json and src.
    // Throws when there is none.
    const std::filesystem::path& sourceDir();

    // The ucc built beside unit-tests.
    const std::filesystem::path& uccPath();
}
