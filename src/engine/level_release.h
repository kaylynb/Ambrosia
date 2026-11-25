#pragma once

#include <cstddef>

namespace dx {
    class World;

    // The garbage collection UGameEngine::LoadMap runs once the new level is
    // up, limited to what is known to be safe to free: the level packages unloaded since the
    // last call (the old map, its saved copy), and objects handed to gc::releaseLater. Packages
    // that hold classes, meshes or fonts are kept whole until every library reports its
    // references (gc::everythingReported).
    namespace levelrelease {
        struct Result {
            // Packages whose objects and file image were released.
            size_t packages = 0;
            size_t freed = 0;
            // Objects of those packages kept because something still refers to them.
            size_t kept = 0;
            // References to the old level's actors and level objects that were set to None.
            size_t cleared = 0;
            // Packages left alone because they hold classes, meshes or fonts.
            size_t pinned = 0;
        };

        // Off keeps every object for the life of the process: GarbageCollection=False.
        void setEnabled(bool enabled);
        bool enabled();

        // Call once the new level has loaded and no script is running. `current` is the world
        // now being played, or null when none is.
        Result collect(World* current);
    }
}
