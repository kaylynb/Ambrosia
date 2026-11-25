#pragma once

#include "core/object.h"

namespace dx {
    class World;
}

namespace dx::aimassist {
    // Aim assist for the right stick, as console shooters have it. The game's own auto-aim
    // never acts while mouse look is on, which it always is, so this works on the view instead.
    // With a weapon in hand and an enemy close to the crosshair, the look stick turns more
    // slowly over the enemy; and while the player moves or looks, the view follows part of how
    // the enemy moves across it (its movement and the player's) and drifts gently toward it.
    // It aims at the head, for a pawn that takes headshots, when that is nearer the crosshair
    // than the chest, and keeps the last frame's enemy until it leaves a slightly wider cone or
    // another is much nearer the crosshair. look and move are how far each stick is pushed, 0
    // to 1. Returns the factor for the look stick, 1 when nothing is near. world is the world
    // in play, or null when there is none; called once a frame, as it remembers the last one.
    float apply(World* world, UObject* player, float look, float move, float deltaSeconds);
}
