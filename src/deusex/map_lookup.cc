#include "deusex/map_lookup.h"

#include "core/objects.h"

namespace dx::maplookup {
    std::string mapName(const std::string& url)
    {
        std::string map = url.substr(0, url.find_first_of("#?"));
        size_t dot = map.rfind('.');
        if (dot != std::string::npos) {
            map = map.substr(0, dot);
        }
        size_t slash = map.find_last_of("/\\");
        if (slash != std::string::npos) {
            map = map.substr(slash + 1);
        }
        return map;
    }

    bool exists(const std::string& map)
    {
        return !map.empty() && (objects::findLinker(Name(map)) || !objects::packageFile(Name(map)).empty());
    }
}
