#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace dx::compiler {
    // Generates src/engine/props_<file>.h and .cc from the binding list
    // (src/engine/props_bindings.txt) and the loaded script packages. The
    // packages named by the list's classes must be loadable. Returns the
    // generated files keyed by file name, or an empty map with errors filled.
    std::map<std::string, std::string> generateProps(const std::string& listText, std::vector<std::string>& errors);

    // Reads the list, generates and writes every file into outputDirectory.
    bool writeProps(const std::filesystem::path& listFile, const std::filesystem::path& outputDirectory, std::vector<std::string>& errors);
}
