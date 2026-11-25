#include "game/touch_cursor.h"

#include <algorithm>
#include <utility>

namespace dx::game {
    TouchCursor::TouchCursor(Host callbacks)
        : host(std::move(callbacks))
    {
    }

    void TouchCursor::update(std::span<const Finger> fingers)
    {
        lastFingers.assign(fingers.begin(), fingers.end());
        auto down = [&](int64_t id) {
            return std::find_if(fingers.begin(), fingers.end(), [id](const Finger& finger) {
                return finger.id == id;
            });
        };
        std::erase_if(ignored, [&](int64_t id) {
            return down(id) == fingers.end();
        });
        if (tracking) {
            auto finger = down(*tracking);
            if (finger != fingers.end()) {
                if (finger->x != lastX || finger->y != lastY) {
                    lastX = finger->x;
                    lastY = finger->y;
                    host.move(lastX, lastY);
                }
                return;
            }
            // Let go even if the press closed the screen, so the button does not stick.
            tracking.reset();
            host.button(false);
            host.released();
        }
        for (const Finger& finger : fingers) {
            if (ignored.count(finger.id)) {
                continue;
            }
            if (!host.wantsCursor()) {
                ignored.insert(finger.id);
                continue;
            }
            tracking = finger.id;
            lastX = finger.x;
            lastY = finger.y;
            host.move(lastX, lastY);
            host.button(true);
            // Other fingers on the screen meanwhile do nothing.
            for (const Finger& other : fingers) {
                if (other.id != finger.id) {
                    ignored.insert(other.id);
                }
            }
            return;
        }
    }
}
