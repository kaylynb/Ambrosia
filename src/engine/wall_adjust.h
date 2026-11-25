#pragma once

#include "core/math.h"

namespace dx {
    // Turns what is left of a blocked move into a slide. When the wall just hit and the one hit
    // before face apart, the move runs along the crease between them, turned to agree with the
    // desired direction; otherwise it slides along the new wall and stops if that would go
    // backwards.
    inline void twoWallAdjust(const Vector& desired, Vector& delta, const Vector& hitNormal, const Vector& oldNormal, float hitTime)
    {
        if (oldNormal.dot(hitNormal) <= 0) {
            Vector direction = hitNormal.cross(oldNormal).normal();
            delta = direction * (delta.dot(direction) * (1.0f - hitTime));
            if (desired.dot(delta) < 0) {
                delta = delta * -1.0f;
            }
        } else {
            delta = (delta - hitNormal * delta.dot(hitNormal)) * (1.0f - hitTime);
            if (delta.dot(desired) <= 0) {
                delta = {};
            }
        }
    }
}
