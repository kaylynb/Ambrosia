#pragma once

#include <string>

// Start-up for everything the programs link. It registers the natives of every layer, Engine's
// through Extension's to DeusEx's, so it sits above all of them.
namespace dx::engine {
    void initialize(const std::string& gameRoot);
    void initializeCore(const std::string& gameRoot);
    void registerDeusExNatives();
}
