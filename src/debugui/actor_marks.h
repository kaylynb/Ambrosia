#pragma once

#include "debugui/overlay.h"

#include <functional>
#include <string>
#include <vector>

namespace dx {
    class UObject;
    class World;
}

// Marks on chosen actors, drawn over the level while the overlay shows (passive is enough): a
// marker at the actor, its collision cylinder (the hitbox the engine traces actors against), and
// a label with its name, class, light and ambient sound. Marks are chosen by a selector, kept
// and matched again every frame, so actors spawned later are marked too:
//
//     <pattern> [<Property>=<value> ...]
//
// The pattern is an actor's name, a class it is or derives from, or * for any actor. Each
// Property=value must also hold, compared as the console's "get" shows the value (enum names
// such as LT_Flicker, True or False, an object's name), ignoring case. For example
// "Light21", "ScriptedPawn", or "* LightType=LT_Flicker".
//
// Console: "DebugOverlay mark <selector>", "DebugOverlay unmark <selector>|all" and
// "DebugOverlay marks list". The Marks tool under Game lists and edits them.
namespace dx::debugui {
    // currentWorld gives the level the console commands count matches in.
    void addActorMarks(Overlay& overlay, std::function<World*()> currentWorld);

    // Adds a mark; the reply says how many actors it matches now.
    std::string addMark(World* world, const std::string& selector);
    // Removes the mark with this selector (ignoring case), or every mark for "all".
    std::string removeMark(const std::string& selector);

    // The live actors a selector matches, in the level's order; empty for a bad selector.
    std::vector<UObject*> findActors(World* world, const std::string& selector);
}
