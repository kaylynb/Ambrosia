#pragma once

#include "core/subscription.h"

#include <cstdint>
#include <functional>

namespace dx {
    class World;
}

// Notices that a world has loaded a level and that it is about to let go of it, for code that
// keeps something of its own per level.
//
// A world gives the loaded notice at the end of a World::loadMap that succeeded, once its actors
// have begun play and the player has logged in. It gives the unloading notice for that level
// when the world is deleted, or when loadMap is called on it again, while the level and its
// actors are still there to read. A level that failed to load gives neither. The level's objects
// are freed later, by the collector; what has to let go of them then still does so in
// gc::Holder::objectsReleased.
//
// Listeners are told in a fixed order, by stage, whatever order they subscribed in: a loaded
// level from Engine to Game, a level about to unload in the reverse order, from Game to Engine,
// so that what was set up last for a level is let go of first. Within one stage, listeners hear
// of a loaded level in the order they subscribed and of an unloading one in the reverse.
//
// Notices are given and subscriptions made on the game thread only.
namespace dx::levelevents {
    enum class Stage : uint8_t {
        Engine,
        Audio,
        Render,
        Game,
    };

    struct Listener {
        // Either may be empty.
        std::function<void(World& world)> loaded;
        std::function<void(World& world)> unloading;
    };

    using dx::Subscription;

    // The listener stays on the list while the subscription is kept. A listener may
    // unsubscribe itself, or another, while it is being told.
    [[nodiscard]] Subscription subscribe(Stage stage, Listener listener);

    // Given by World.
    void levelLoaded(World& world);
    void levelUnloading(World& world);
}
