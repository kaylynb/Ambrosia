#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

// Finding a Steam copy of the game, for the places a shipped build looks for the game data
// (platform::gameDataCandidates).
namespace dx::steam {
    // Deus Ex: Game of the Year Edition's folder in a Steam library: <library>/steamapps/common/Deus Ex.
    std::filesystem::path gameFolder(const std::filesystem::path& library);

    // The libraries a steamapps/libraryfolders.vdf lists, in its order: each "path" value, with
    // the file's \\ and \" escapes undone. Steam writes one quoted key and value per line, so
    // this reads only those lines and not the file's nesting.
    std::vector<std::filesystem::path> libraryFolders(std::string_view vdf);

    // The libraries of a Steam install: the install's own folder, then those its
    // steamapps/libraryfolders.vdf lists, if it has one.
    std::vector<std::filesystem::path> libraries(const std::filesystem::path& steamRoot);
}
