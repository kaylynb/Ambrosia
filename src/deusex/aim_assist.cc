#include "deusex/aim_assist.h"

#include "core/math.h"
#include "core/vm.h"
#include "deusex/script.h"
#include "engine/ai.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <algorithm>
#include <cmath>

namespace dx::aimassist {
    namespace {
        constexpr float maxRange = 3000.0f;
        constexpr uint8_t allianceHostile = 2;
        // How far, as a share of the current target's cone, a target held from the last frame
        // keeps counting, and how much nearer the crosshair (as a share of its cone) another must
        // be to take over, so the assist does not flick between two enemies side by side.
        constexpr float holdConeScale = 1.25f;
        constexpr float switchMargin = 0.5f;
        // The share of a target's movement across the view that the view follows, right on it.
        constexpr float trackingShare = 0.6f;
        // The most the view follows in one frame, in rotator units (about 8 degrees), and the
        // longest gap between two frames that still counts as following, in seconds.
        constexpr float trackingLimit = 1500.0f;
        constexpr float trackingGap = 0.25f;
        // How fast the view drifts toward the aim point, per second at full stick.
        constexpr float pullRate = 1.5f;

        // The signed difference between two rotator angles, the short way round.
        int32_t angleDifference(int32_t to, int32_t from)
        {
            return static_cast<int16_t>(static_cast<uint16_t>((to - from) & 65535));
        }

        float angleDifference(float to, float from)
        {
            float d = std::fmod(to - from, 65536.0f);
            if (d > 32768.0f) {
                d -= 65536.0f;
            } else if (d < -32768.0f) {
                d += 65536.0f;
            }
            return d;
        }

        // The direction of v as yaw and pitch in rotator units, unrounded.
        struct Direction {
            float yaw = 0;
            float pitch = 0;
        };

        Direction direction(const Vector& v)
        {
            return { std::atan2(v.y, v.x) * 32768.0f / Pi, std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) * 32768.0f / Pi };
        }

        bool hostile(UObject* target, UObject* player)
        {
            return script::isA(target, "ScriptedPawn") && vm::Call(target, Name("GetPawnAllianceType")).obj(player).result<uint8_t>() == allianceHostile;
        }

        // Whether a hit high on the pawn is a headshot: ScriptedPawn.HandleDamage counts a hit
        // above 0.7 of the collision height, near the middle, as one (eight times the damage).
        // Animal overrides HandleDamage and Robot takes damage in TakeDamageBase, without one.
        bool hasHead(UObject* pawn)
        {
            return !script::isA(pawn, "Robot") && !script::isA(pawn, "Animal");
        }

        Vector chestPoint(UObject* p)
        {
            // Where the game's own auto-aim points.
            Vector point = actor::Location(p);
            point.z += 0.3f * actor::CollisionHeight(p);
            return point;
        }

        Vector headPoint(UObject* p)
        {
            // The middle of HandleDamage's head zone, 0.7 to 1 of the collision height. That is
            // the collision cylinder's and so follows crouching and sitting, as the zone does.
            Vector point = actor::Location(p);
            point.z += 0.85f * actor::CollisionHeight(p);
            return point;
        }

        struct Target {
            UObject* pawn = nullptr;
            Vector aimPoint;
            // How far off the crosshair the aim point is, and how far off it may be to count,
            // both in radians.
            float angle = 0;
            float cone = 0;

            float share() const
            {
                return angle / cone;
            }
        };

        float angleTo(const Vector& point, const Vector& eye, const Vector& forward, float& distance)
        {
            Vector delta = point - eye;
            distance = std::sqrt(delta.sizeSquared());
            if (distance < 1.0f) {
                return Pi;
            }
            return std::acos(std::clamp(delta.dot(forward) / distance, -1.0f, 1.0f));
        }

        // p as a target, aimed at its chest or, when it has one and it is nearer the crosshair,
        // its head; with an empty pawn when it is out of reach. coneScale widens its cone.
        Target measure(UObject* p, const Vector& eye, const Vector& forward, float coneScale)
        {
            float distance = 0;
            Vector aimPoint = chestPoint(p);
            float angle = angleTo(aimPoint, eye, forward, distance);
            if (distance < 1.0f || distance > maxRange) {
                return {};
            }
            if (hasHead(p)) {
                Vector head = headPoint(p);
                float headDistance = 0;
                float headAngle = angleTo(head, eye, forward, headDistance);
                if (headAngle < angle) {
                    aimPoint = head;
                    angle = headAngle;
                }
            }
            // A target's pull reaches a little past its body, so a far enemy still has some.
            float cone = coneScale * std::clamp(std::atan((actor::CollisionRadius(p) * 2.0f + 16.0f) / distance), 2.5f * Pi / 180.0f, 12.0f * Pi / 180.0f);
            if (angle >= cone) {
                return {};
            }
            return { p, aimPoint, angle, cone };
        }

        bool candidate(UObject* p, UObject* player)
        {
            return p && p != player && !p->destroyed && pawn::Health(p) > 0 && actor::bProjTarget(p) && !actor::bHidden(p);
        }

        bool reachable(UObject* p, UObject* player)
        {
            return hostile(p, player) && lineOfSightTo(player, p, false, false);
        }

        Target nearestTarget(const World& world, UObject* player, const Vector& eye, const Vector& forward)
        {
            Target best;
            for (UObject* p = levelinfo::PawnList(world.levelInfo); p; p = pawn::nextPawn(p)) {
                if (!candidate(p, player)) {
                    continue;
                }
                Target target = measure(p, eye, forward, 1.0f);
                if (!target.pawn || (best.pawn && target.share() >= best.share())) {
                    continue;
                }
                if (!reachable(p, player)) {
                    continue;
                }
                best = target;
            }
            return best;
        }

        // What the last frame's assist was on, so it can keep the same target and follow how
        // that target moved across the view since.
        struct Memory {
            World* world = nullptr;
            UObject* player = nullptr;
            UObject* pawn = nullptr;
            Direction seen;
            float time = 0;
        };

        Memory& memory()
        {
            static Memory m;
            return m;
        }

        // The last frame's target while it still counts, in a cone a little wider than a new
        // one's; or an empty pawn.
        Target heldTarget(const Memory& last, World* world, UObject* player, const Vector& eye, const Vector& forward)
        {
            if (last.world != world || last.player != player || !last.pawn) {
                return {};
            }
            // The pawn may have been freed since; it is used only if it is still in the level.
            for (UObject* p = levelinfo::PawnList(world->levelInfo); p; p = pawn::nextPawn(p)) {
                if (p == last.pawn) {
                    if (!candidate(p, player)) {
                        return {};
                    }
                    Target target = measure(p, eye, forward, holdConeScale);
                    return target.pawn && reachable(p, player) ? target : Target {};
                }
            }
            return {};
        }

        Target chooseTarget(const Memory& last, World* world, UObject* player, const Vector& eye, const Vector& forward)
        {
            Target held = heldTarget(last, world, player, eye, forward);
            Target best = nearestTarget(*world, player, eye, forward);
            if (!held.pawn) {
                return best;
            }
            if (best.pawn && best.pawn != held.pawn && best.share() < switchMargin * held.share()) {
                return best;
            }
            return held;
        }
    }

    float apply(World* world, UObject* player, float look, float move, float deltaSeconds)
    {
        Memory& last = memory();
        if (!player || !world || !world->levelInfo || pawn::Health(player) <= 0 || !pawn::Weapon(player)) {
            last = {};
            return 1.0f;
        }
        Rotator& view = pawn::ViewRotation(player);
        Vector eye = actor::Location(player);
        eye.z += pawn::EyeHeight(player);
        Target target = chooseTarget(last, world, player, eye, rotationVector(view));
        float now = levelinfo::TimeSeconds(world->levelInfo);
        if (!target.pawn) {
            last = { world, player, nullptr, {}, now };
            return 1.0f;
        }
        // Followed by the chest whichever point is aimed at, so moving between the head and
        // the chest is not taken for the target moving.
        Direction seen = direction(chestPoint(target.pawn) - eye);
        bool following = last.world == world && last.player == player && last.pawn == target.pawn && now >= last.time && now - last.time <= trackingGap;
        Direction moved;
        if (following) {
            moved = { angleDifference(seen.yaw, last.seen.yaw), angleDifference(seen.pitch, last.seen.pitch) };
        }
        last = { world, player, target.pawn, seen, now };

        // Only while the player is aiming or moving, so a still stick never steers the view.
        float strength = std::min(1.0f, look * 0.7f + move * 0.5f);
        if (strength <= 0) {
            return 0.5f + 0.5f * target.share();
        }
        float yaw = static_cast<float>(view.yaw);
        float pitch = static_cast<float>(angleDifference(view.pitch, 0));
        // Tracking: the view follows part of how the target moved across it since the last
        // frame, its own movement and the player's, most when the crosshair is right on it.
        // Any stick input is enough for all of it, as strafing past an enemy is when it counts.
        float follow = trackingShare * (1.0f - target.share()) * std::min(1.0f, 4.0f * strength);
        yaw += std::clamp(moved.yaw, -trackingLimit, trackingLimit) * follow;
        pitch += std::clamp(moved.pitch, -trackingLimit, trackingLimit) * follow;
        // And a gentle drift toward the aim point itself.
        Direction wanted = direction(target.aimPoint - eye);
        float fraction = std::min(1.0f, pullRate * strength * deltaSeconds);
        yaw += angleDifference(wanted.yaw, yaw) * fraction;
        pitch += angleDifference(wanted.pitch, pitch) * fraction;
        view.yaw = static_cast<int32_t>(std::lround(yaw)) & 65535;
        view.pitch = static_cast<int32_t>(std::lround(pitch)) & 65535;
        // Slower turning over the target, easing off toward the edge of its reach.
        return 0.5f + 0.5f * target.share();
    }
}
