#pragma once

#include <string>

namespace dx::maplookup {
    // The map a travel URL names, as World::loadMap reads it: "Map.dx#Portal?Option" is "Map".
    std::string mapName(const std::string& url);
    // Whether a package of that name is loaded or one loadPackage would open, so travel can
    // check it before leaving the current level.
    bool exists(const std::string& map);
}
