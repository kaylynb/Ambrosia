#include "core/objects.h"
#include "core/random.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/wall_adjust.h"
#include "engine/world.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace dx {
    namespace {
        float frand()
        {
            return std::uniform_real_distribution<float>(0.0f, 1.0f)(random::engine(random::Stream::Physics));
        }

        bool move(World& world, UObject* a, const Vector& delta, CheckResult& hit)
        {
            return world.moveActor(a, delta, actor::Rotation(a), hit, false, false, false, false);
        }

        Vector zoneGravity(UObject* a)
        {
            UObject* zone = zoneOf(a);
            return zone ? zoneinfo::ZoneGravity(zone) : Vector { 0, 0, -950 };
        }

        Vector zoneVelocity(UObject* a)
        {
            UObject* zone = zoneOf(a);
            return zone ? zoneinfo::ZoneVelocity(zone) : Vector {};
        }

        float zoneGroundFriction(UObject* a)
        {
            UObject* zone = zoneOf(a);
            return zone ? zoneinfo::ZoneGroundFriction(zone) : 8.0f;
        }

        float zoneFluidFriction(UObject* a)
        {
            UObject* zone = zoneOf(a);
            return zone ? zoneinfo::ZoneFluidFriction(zone) : 1.2f;
        }

        float zoneTerminalVelocity(UObject* a)
        {
            UObject* zone = zoneOf(a);
            return zone ? zoneinfo::ZoneTerminalVelocity(zone) : 2500.0f;
        }

        bool waterZone(UObject* zone)
        {
            return zone && zoneinfo::bWaterZone(zone);
        }

        bool probing(UObject* a, const char* eventName)
        {
            return vm::findEvent(a, Name(eventName)) != nullptr;
        }

        int32_t fixedTurn(UObject* a, int32_t current, int32_t desired, int32_t deltaRate)
        {
            if (deltaRate == 0) {
                return current & 65535;
            }
            int32_t result = current & 65535;
            current = result;
            desired = desired & 65535;
            if (actor::bFixedRotationDir(a)) {
                if (actor::bRotateToDesired(a)) {
                    if (deltaRate > 0) {
                        if (current > desired) {
                            desired += 65536;
                        }
                        result += std::min(deltaRate, desired - current);
                    } else {
                        if (current < desired) {
                            current += 65536;
                        }
                        result += std::max(deltaRate, desired - current);
                    }
                } else {
                    result += deltaRate;
                }
            } else if (actor::bRotateToDesired(a)) {
                int32_t rate = std::abs(deltaRate);
                if (current > desired) {
                    if (current - desired < 32768) {
                        result -= std::min(current - desired, rate);
                    } else {
                        result += std::min(desired + 65536 - current, rate);
                    }
                } else {
                    if (desired - current < 32768) {
                        result += std::min(desired - current, rate);
                    } else {
                        result -= std::min(current + 65536 - desired, rate);
                    }
                }
            }
            return result & 65535;
        }

        void actorRotation(World& world, UObject* a, float deltaTime)
        {
            bool toDesired = actor::bRotateToDesired(a) != 0;
            if ((!toDesired && !actor::bFixedRotationDir(a)) || (toDesired && actor::Rotation(a) == actor::DesiredRotation(a))) {
                return;
            }
            Rotator rotation = actor::Rotation(a);
            Rotator rate = actor::RotationRate(a);
            Rotator desired = actor::DesiredRotation(a);
            int32_t yaw = static_cast<int32_t>(static_cast<float>(rate.yaw) * deltaTime);
            int32_t pitch = static_cast<int32_t>(static_cast<float>(rate.pitch) * deltaTime);
            int32_t roll = static_cast<int32_t>(static_cast<float>(rate.roll) * deltaTime);
            if (yaw != 0 && (!toDesired || desired.yaw != rotation.yaw)) {
                rotation.yaw = fixedTurn(a, rotation.yaw, desired.yaw, yaw);
            }
            if (pitch != 0 && (!toDesired || desired.pitch != rotation.pitch)) {
                rotation.pitch = fixedTurn(a, rotation.pitch, desired.pitch, pitch);
            }
            if (roll != 0 && (!toDesired || desired.roll != rotation.roll)) {
                rotation.roll = fixedTurn(a, rotation.roll, desired.roll, roll);
            }
            if (!(rotation == actor::Rotation(a))) {
                CheckResult hit;
                world.moveActor(a, {}, rotation, hit, false, false, false, false);
            }
            if (toDesired && actor::Rotation(a) == actor::DesiredRotation(a)) {
                vm::event(a, Name("EndedRotation"));
            }
        }

        void pawnRotation(World& world, UObject* a, float deltaTime)
        {
            Rotator rotation = actor::Rotation(a);
            Rotator rate = actor::RotationRate(a);
            if (!isPlayerPawn(a)) {
                int32_t deltaYaw = static_cast<int32_t>(static_cast<float>(rate.yaw) * deltaTime);
                actor::bRotateToDesired(a) = 1;
                actor::bFixedRotationDir(a) = 0;
                Rotator desired = actor::DesiredRotation(a);
                if (desired.yaw != rotation.yaw) {
                    rotation.yaw = fixedTurn(a, rotation.yaw, desired.yaw, deltaYaw);
                }
                if (desired.pitch != rotation.pitch) {
                    int32_t deltaPitch = static_cast<int32_t>(deltaTime * 32768.0f);
                    uint8_t physics = actor::Physics(a);
                    int32_t target = (physics == PHYS_Walking || physics == PHYS_Falling) ? 0 : desired.pitch;
                    rotation.pitch = fixedTurn(a, rotation.pitch, target, deltaPitch);
                    if (rotation.pitch < 32768) {
                        if (rotation.pitch > rate.pitch) {
                            rotation.pitch = rate.pitch;
                        }
                    } else if (rotation.pitch < 65536 - rate.pitch) {
                        rotation.pitch = 65536 - rate.pitch;
                    }
                }
            }
            rotation.roll = 0;
            if (!(rotation == actor::Rotation(a))) {
                CheckResult hit;
                world.moveActor(a, {}, rotation, hit, false, false, false, false);
            }
        }

        void calcVelocity(UObject* a, const Vector& accelDir, float deltaTime, float maxSpeed, float friction, bool fluid, bool brake, bool buoyant)
        {
            float effectiveFriction = std::max(fluid ? 1.0f : 0.0f, friction);
            bool walkingPlayer = isPlayerPawn(a) && pawn::bIsWalking(a);
            Vector& velocity = actor::Velocity(a);
            Vector& acceleration = actor::Acceleration(a);
            float accelRate = pawn::AccelRate(a);
            if (brake && acceleration.zero()) {
                // Brake in steps of 0.03 seconds, and take the average speed over the tick
                // counting only the steps still moving the way the pawn was going. Below 10
                // units a second it stops.
                Vector old = velocity;
                Vector average;
                float left = deltaTime;
                while (left > 0.03f) {
                    velocity = velocity - velocity * (2.0f * 0.03f * effectiveFriction);
                    if (old.dot(velocity) > 0) {
                        average += velocity * (0.03f / deltaTime);
                    }
                    left -= 0.03f;
                }
                velocity = velocity - velocity * (2.0f * left * effectiveFriction);
                if (old.dot(velocity) > 0) {
                    average += velocity * (left / deltaTime);
                }
                velocity = average;
                if (old.dot(velocity) < 0 || velocity.sizeSquared() < 100.0f) {
                    velocity = {};
                }
            } else {
                float speed = velocity.size();
                if (walkingPlayer) {
                    if (acceleration.sizeSquared() > 0.09f * accelRate * accelRate) {
                        acceleration = accelDir * (accelRate * 0.3f);
                    }
                } else if (acceleration.sizeSquared() > accelRate * accelRate) {
                    acceleration = accelDir * accelRate;
                }
                velocity = velocity - (velocity - accelDir * speed) * (deltaTime * effectiveFriction);
            }
            velocity = velocity * (1 - (fluid ? friction * deltaTime : 0.0f)) + acceleration * deltaTime;
            if (!isPlayerPawn(a)) {
                maxSpeed *= pawn::DesiredSpeed(a);
            }
            if (buoyant) {
                float mass = std::max(actor::Mass(a), 1.0f);
                velocity = velocity + zoneGravity(a) * (deltaTime * (1.0f - actor::Buoyancy(a) / mass));
            }
            if (walkingPlayer && velocity.sizeSquared() > 0.09f * maxSpeed * maxSpeed) {
                float speed = velocity.size();
                velocity = velocity / speed;
                velocity = velocity * std::max(0.3f * maxSpeed, speed * (1 - deltaTime * 2 * effectiveFriction));
            } else if (velocity.sizeSquared() > maxSpeed * maxSpeed) {
                velocity = velocity.normal() * maxSpeed;
            }
        }

        void physFalling(World& world, UObject* a, float deltaTime, int iterations);
        void physFlying(World& world, UObject* a, float deltaTime, int iterations);
        void physSwimming(World& world, UObject* a, float deltaTime, int iterations);

        // Walking, falling and swimming all start by checking the player's footing and nearby
        // ladders. A BSP surface is a ladder if its texture belongs to the Ladder group.

        // A trace from start to end, and the texture of the BSP surface it ends on. Movers and
        // other actors give none.
        UObject* surfaceTexture(World& world, UObject* a, CheckResult& hit, const Vector& start, const Vector& end)
        {
            hit = CheckResult {};
            world.lineCheck(hit, start, end, {}, a, TRACE_World);
            if (hit.time >= 1.0f || hit.actor != world.levelInfo) {
                return nullptr;
            }
            return hit.material;
        }

        bool ladderTexture(UObject* texture)
        {
            return texture && texture->outer && texture->outer->name == Name("Ladder");
        }

        // Probes from just above the feet out to twice the collision radius, ahead of the way
        // the pawn faces and then to its left, its right and behind it, each at nine heights
        // from level with the feet to 32 units above them. Returns the texture of the last
        // surface hit, which is a ladder's if any probe found one.
        UObject* findLadderTexture(World& world, UObject* a, CheckResult& hit)
        {
            Vector feet = actor::Location(a) - Vector { 0, 0, actor::CollisionHeight(a) * 0.95f };
            float reach = actor::CollisionRadius(a) * 2.0f;
            UObject* texture = nullptr;
            for (int32_t turn : { 0, -16384, 16384, 32768 }) {
                Rotator facing { 0, actor::Rotation(a).yaw + turn, actor::Rotation(a).roll };
                Vector probe = feet + rotationVector(facing) * reach;
                for (int up = 0; up <= 32; up += 4) {
                    texture = surfaceTexture(world, a, hit, feet, probe + Vector { 0, 0, static_cast<float>(up) });
                    if (texture && (!texture->outer || texture->outer->name == Name("Ladder"))) {
                        return texture;
                    }
                }
            }
            return texture;
        }

        // Returns true when the player is on a ladder, which takes the place of the physics mode
        // for this tick. On one, the player moves as if flying: without input they hang still,
        // and with any input they climb at a vertical speed set by where they look. Level is
        // already a climb; looking down more than 33.75 degrees climbs down. Physics is left
        // as it was, so a player can be on a ladder while walking, falling or swimming.
        // Off a ladder, it reports the floor's texture to script and slides the player down
        // slopes whose texture has a friction below 1.
        bool physLadder(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (!isPlayerPawn(a)) {
                return false;
            }
            CheckResult hit;
            UObject* tex = findLadderTexture(world, a, hit);
            if (ladderTexture(tex)) {
                vm::Call(a, Name("WalkTexture")).obj(tex).v(hit.location).v(hit.normal).run();
                if (!actor::Acceleration(a).zero()) {
                    Rotator view = pawn::ViewRotation(a);
                    view.pitch += view.pitch < 0 ? -6144 : 6144;
                    pawn::bIsWalking(a) = 1;
                    actor::Velocity(a).z = pawn::GroundSpeed(a) * rotationVector(view).z * 0.5f;
                }
                physFlying(world, a, deltaTime, iterations);
                if (actor::Acceleration(a).zero()) {
                    actor::Velocity(a) = {};
                }
                return true;
            }
            Vector location = actor::Location(a);
            Vector below { 0, 0, actor::CollisionRadius(a) * 2.0f + actor::CollisionHeight(a) };
            tex = surfaceTexture(world, a, hit, location, location - below);
            vm::Call(a, Name("WalkTexture")).obj(tex).v(hit.location).v(hit.normal).run();
            if (actor::Physics(a) == PHYS_Walking && tex && texture::Friction(tex) < 1.0f) {
                float friction = std::max(texture::Friction(tex) * 4.0f, 0.05f);
                Vector slide = zoneGravity(a) * (deltaTime * deltaTime / (friction * 0.5f));
                Vector delta = slide - hit.normal * hit.normal.dot(slide);
                if (delta.dot(slide) >= 0) {
                    move(world, a, delta, hit);
                }
            }
            return false;
        }

        // Bisects from start toward end for where the zone stops being water or stops being
        // dry, keeping end on the side of the zone the pawn is in now.
        void findWaterLine(World& world, UObject* a, Vector start, Vector& end)
        {
            bool water = waterZone(zoneOf(a));
            while ((end - start).sizeSquared() >= 1.0f) {
                Vector middle = (start + end) * 0.5f;
                if (waterZone(world.zoneAt(middle, nullptr, nullptr)) == water) {
                    end = middle;
                } else {
                    start = middle;
                }
            }
        }

        // A swimming move. If it leaves the water, the pawn is brought back to the surface and
        // the share of the move that would have been spent in the air is returned.
        float swim(World& world, UObject* a, const Vector& delta, CheckResult& hit)
        {
            Vector start = actor::Location(a);
            float airTime = 0;
            move(world, a, delta, hit);
            if (!a->destroyed && !waterZone(zoneOf(a))) {
                Vector end = actor::Location(a);
                findWaterLine(world, a, start, end);
                if (!(end == actor::Location(a))) {
                    airTime = (end - actor::Location(a)).size() / delta.size();
                    move(world, a, end - actor::Location(a), hit);
                }
            }
            return airTime;
        }

        // Entering water from walking or falling: back up to the surface, take the speed the
        // pawn came in at, and swim for the rest of the tick.
        void startSwimming(World& world, UObject* a, Vector oldVelocity, float timeTick, float remainingTime, int iterations)
        {
            Vector end = actor::Location(a);
            findWaterLine(world, a, actor::OldLocation(a), end);
            float waterTime = 0;
            if (!(end == actor::Location(a))) {
                waterTime = timeTick * (end - actor::Location(a)).size() / (actor::Location(a) - actor::OldLocation(a)).size();
                remainingTime += waterTime;
                CheckResult hit;
                move(world, a, end - actor::Location(a), hit);
                if (a->destroyed) {
                    return;
                }
            }
            Vector& velocity = actor::Velocity(a);
            if (!actor::bBounce(a) && !actor::bJustTeleported(a)) {
                velocity = (actor::Location(a) - actor::OldLocation(a)) / (timeTick - waterTime);
                velocity = velocity * 2.0f - oldVelocity;
                if (velocity.sizeSquared() > 16000000.0f) {
                    velocity = velocity.normal() * 4000.0f;
                }
            }
            if (velocity.z > -160.0f && velocity.z < 0) {
                velocity.z = -80.0f - velocity.size2D() * 0.7f;
            }
            if (remainingTime > 0.01f) {
                physSwimming(world, a, remainingTime, iterations);
            }
        }

        void stepUp(World& world, UObject* a, const Vector& gravDir, const Vector& desiredDir, Vector delta, CheckResult& hit)
        {
            Vector down = gravDir * pawn::MaxStepHeight(a);
            Vector up = down * -1.0f;
            move(world, a, up, hit);
            move(world, a, delta, hit);
            if (hit.time < 1.0f) {
                UObject* hitActor = hit.actor;
                bool pushable = hitActor && isPlayerPawn(a) && hitActor->isA(engineClasses().decoration) && decoration::bPushable(hitActor) && hit.normal.dot(desiredDir) < -0.9f;
                if (pushable) {
                    actor::bJustTeleported(a) = 1;
                    actor::Velocity(a) = actor::Velocity(a) * (actor::Mass(a) / (actor::Mass(a) + actor::Mass(hitActor)));
                    processHitWall(a, hit.normal, hitActor);
                    if (actor::Physics(a) == PHYS_Falling) {
                        return;
                    }
                } else if (std::abs(hit.normal.z) < 0.2f && hit.time * delta.sizeSquared() > 144.0f) {
                    stepUp(world, a, gravDir, desiredDir, delta * (1 - hit.time), hit);
                    if (actor::Physics(a) == PHYS_Falling) {
                        return;
                    }
                } else {
                    processHitWall(a, hit.normal, hitActor);
                    Vector original = delta;
                    Vector oldNormal = hit.normal;
                    delta = (delta - hit.normal * delta.dot(hit.normal)) * (1.0f - hit.time);
                    if (delta.dot(original) >= 0) {
                        move(world, a, delta, hit);
                        if (hit.time < 1.0f) {
                            processHitWall(a, hit.normal, hit.actor);
                            if (actor::Physics(a) == PHYS_Falling) {
                                return;
                            }
                            twoWallAdjust(desiredDir, delta, hit.normal, oldNormal, hit.time);
                            move(world, a, delta, hit);
                        }
                    }
                }
            }
            move(world, a, down, hit);
            if (hit.time < 1.0f && hit.normal.z < 0.5f) {
                Vector slide = (down - hit.normal * down.dot(hit.normal)) * (1.0f - hit.time);
                if (slide.dot(down) >= 0) {
                    move(world, a, slide, hit);
                }
            }
        }

        void physWalking(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (physLadder(world, a, deltaTime, iterations)) {
                return;
            }
            if (region(a).zoneNumber == 0) {
                // Outside the level. Pawn.FellOutOfWorld kills the pawn, the player included.
                vm::event(a, Name("FellOutOfWorld"));
                return;
            }
            Vector& velocity = actor::Velocity(a);
            Vector& acceleration = actor::Acceleration(a);
            velocity.z = 0;
            acceleration.z = 0;
            Vector accelDir = acceleration.zero() ? Vector {} : acceleration.normal();
            calcVelocity(a, accelDir, deltaTime, pawn::GroundSpeed(a), zoneGroundFriction(a), false, true, false);
            Vector desiredMove = actor::Velocity(a);
            Vector zoneVel = zoneVelocity(a);
            if (isPlayerPawn(a) || zoneVel.sizeSquared() > 90000.0f) {
                // Scaled by 25 times the tick, as Deus Ex does when walking and swimming.
                desiredMove = desiredMove + zoneVel * (25.0f * deltaTime);
            }
            desiredMove.z = 0;
            Vector gravDir { 0, 0, zoneGravity(a).z > 0 ? 1.0f : -1.0f };
            float maxStep = pawn::MaxStepHeight(a);
            Vector down = gravDir * (maxStep + 2.0f);
            CheckResult hit;
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            bool checkedFall = false;
            float remaining = deltaTime;
            float height = actor::CollisionHeight(a);
            Vector extent = cylinderExtent(a);
            while (remaining > 0 && iterations < 8) {
                iterations++;
                float tick;
                if (remaining > 0.05f && (isPlayerPawn(a) || desiredMove.sizeSquared() * remaining * remaining > 400.0f)) {
                    tick = std::min(0.05f, remaining * 0.5f);
                } else {
                    tick = remaining;
                }
                remaining -= tick;
                Vector delta = desiredMove * tick;
                Vector subLocation = actor::Location(a);
                Vector subMove = delta;
                bool zeroMove = std::abs(delta.x) < 0.0001f && std::abs(delta.y) < 0.0001f && std::abs(delta.z) < 0.0001f;
                if (zeroMove) {
                    remaining = 0;
                    pawn::bHitSlopedWall(a) = 0;
                } else {
                    // Deus Ex's physWalking has none of Unreal Tournament's look-ahead for
                    // ledges (bAvoidLedges): pawns walk straight on, and the floor check
                    // below decides whether they fall.
                    if (pawn::bHitSlopedWall(a)) {
                        float desiredDist = delta.size();
                        Vector desiredDir = delta / desiredDist;
                        Vector location = actor::Location(a);
                        world.lineCheck(hit, location, location + desiredDir * std::max(30.0f, desiredDist + 4), extent, a, TRACE_World | TRACE_Blocking);
                        bool sloped = hit.time < 1.0f && hit.normal.z >= 0.01f && hit.normal.z < 0.7f;
                        pawn::bHitSlopedWall(a) = sloped ? 1 : 0;
                        if (sloped) {
                            Vector normal = Vector { hit.normal.x, hit.normal.y, 0 }.normal();
                            delta = delta - normal * delta.dot(normal);
                        } else if (isPlayerPawn(a)) {
                            // A player stays against a sloped wall while one is within 100
                            // units of their feet, ahead or 45 degrees to either side.
                            Vector feet { location.x, location.y, location.z - height + maxStep + 4.0f };
                            Vector sides[] = {
                                desiredDir,
                                Vector { desiredDir.x + desiredDir.y, desiredDir.y - desiredDir.x, desiredDir.z }.normal(),
                                Vector { desiredDir.x - desiredDir.y, desiredDir.x + desiredDir.y, desiredDir.z }.normal(),
                            };
                            for (const Vector& side : sides) {
                                CheckResult check;
                                world.lineCheck(check, feet, feet + side * 100.0f, {}, a, TRACE_World | TRACE_Blocking);
                                if (check.time < 1.0f && check.normal.z >= 0.01f && check.normal.z < 0.7f) {
                                    pawn::bHitSlopedWall(a) = 1;
                                    break;
                                }
                            }
                        }
                        move(world, a, delta, hit);
                    } else {
                        move(world, a, delta, hit);
                        pawn::bHitSlopedWall(a) = (hit.time < 1.0f && hit.normal.z >= 0.01f && hit.normal.z < 0.7f) ? 1 : 0;
                    }
                    if (a->destroyed) {
                        return;
                    }
                    if (hit.time < 1.0f) {
                        Vector desiredDir = delta.normal();
                        stepUp(world, a, gravDir, desiredDir, delta * (1.0f - hit.time), hit);
                        if (actor::Physics(a) == PHYS_Falling) {
                            float desiredDist = subMove.size();
                            float actualDist = (actor::Location(a) - subLocation).size2D();
                            if (desiredDist > 0) {
                                remaining += tick * (1 - std::min(1.0f, actualDist / desiredDist));
                            }
                            vm::event(a, Name("Falling"));
                            if (actor::Physics(a) == PHYS_Falling) {
                                if (remaining > 0.01f) {
                                    physFalling(world, a, remaining, iterations);
                                }
                            } else if (actor::Physics(a) == PHYS_Flying) {
                                actor::Velocity(a) = { 0, 0, pawn::AirSpeed(a) };
                                actor::Acceleration(a) = { 0, 0, pawn::AccelRate(a) };
                                if (remaining > 0.01f) {
                                    physFlying(world, a, remaining, iterations);
                                }
                            }
                            return;
                        }
                    }
                    if (actor::Physics(a) == PHYS_Swimming) {
                        startSwimming(world, a, actor::Velocity(a), tick, remaining, iterations);
                        return;
                    }
                }
                Vector location = actor::Location(a);
                if (zeroMove) {
                    Vector foot = location - Vector { 0, 0, height };
                    world.lineCheck(hit, foot, foot - Vector { 0, 0, 20 }, {}, a, TRACE_World | TRACE_Blocking);
                    float floorDist = hit.time * 20;
                    zeroMove = actor::Base(a) == hit.actor && floorDist <= 4.6f && floorDist >= 4.1f;
                }
                if (!zeroMove) {
                    world.lineCheck(hit, location, location + down, extent, a, TRACE_AllColliding | TRACE_Blocking);
                    float floorDist = hit.time * (maxStep + 2.0f);
                    if (hit.time < 1.0f && (hit.actor != actor::Base(a) || floorDist > 2.4f)) {
                        move(world, a, down, hit);
                        if (hit.actor != actor::Base(a)) {
                            setSupportBase(a, hit.actor);
                        }
                        if (actor::Physics(a) == PHYS_Swimming) {
                            startSwimming(world, a, actor::Velocity(a), tick, remaining, iterations);
                            return;
                        }
                    } else if (floorDist < 1.9f) {
                        Vector realNormal = hit.normal;
                        move(world, a, { 0, 0, 2.1f - floorDist }, hit);
                        hit.time = 0;
                        hit.normal = realNormal;
                    }
                    if (hit.time < 1.0f && hit.normal.z >= 0.7f) {
                        float friction = zoneGroundFriction(a);
                        if (hit.normal.z < 1.0f && hit.normal.z * friction < 3.3f) {
                            Vector slide = zoneGravity(a) * (deltaTime * deltaTime / (2 * std::max(0.5f, friction)));
                            Vector slideDelta = slide - hit.normal * slide.dot(hit.normal);
                            if (slideDelta.dot(slide) >= 0) {
                                move(world, a, slideDelta, hit);
                            }
                            if (actor::Physics(a) == PHYS_Swimming) {
                                startSwimming(world, a, actor::Velocity(a), tick, remaining, iterations);
                                return;
                            }
                        }
                    } else {
                        // Off a ledge. Only a pawn that cannot jump stops at it; Deus Ex lets a
                        // walking player step off just as a running one does.
                        if (pawn::bCanJump(a) && !checkedFall && probing(a, "MayFall")) {
                            checkedFall = true;
                            vm::event(a, Name("MayFall"));
                        }
                        if (!pawn::bCanJump(a)) {
                            actor::Velocity(a) = {};
                            actor::Acceleration(a) = {};
                            world.farMoveActor(a, actor::OldLocation(a), false, false);
                            pawn::MoveTimer(a) = -1.0f;
                            return;
                        }
                        if (hit.time < 1.0f) {
                            pawn::bHitSlopedWall(a) = 1;
                        }
                        float desiredDist = subMove.size();
                        float actualDist = (actor::Location(a) - subLocation).size2D();
                        if (desiredDist > 0) {
                            remaining += tick * (1 - std::min(1.0f, actualDist / desiredDist));
                        } else {
                            remaining = 0;
                        }
                        actor::Velocity(a).z = 0;
                        vm::event(a, Name("Falling"));
                        if (actor::Physics(a) == PHYS_Walking) {
                            setPhysics(a, PHYS_Falling, nullptr);
                        }
                        if (actor::Physics(a) == PHYS_Falling) {
                            float velocityZ = actor::Velocity(a).z;
                            if (!actor::bJustTeleported(a) && deltaTime > remaining) {
                                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / (deltaTime - remaining);
                            }
                            actor::Velocity(a).z = velocityZ;
                            if (remaining > 0.01f) {
                                physFalling(world, a, remaining, iterations);
                            }
                            return;
                        }
                        move(world, a, desiredMove * remaining, hit);
                        remaining = 0;
                    }
                }
            }
            if (!actor::bJustTeleported(a)) {
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaTime;
            }
            actor::Velocity(a).z = 0;
        }

        Vector fallingVelocity(UObject* a, const Vector& oldVelocity, float tick)
        {
            UObject* zone = zoneOf(a);
            Vector acceleration = actor::Acceleration(a);
            Vector gravity = zoneGravity(a);
            if (!waterZone(zone)) {
                if (a->isA(engineClasses().decoration) && decoration::bBobbing(a)) {
                    return oldVelocity + (acceleration + gravity * 0.5f) * (0.5f * tick);
                }
                if (isPlayerPawn(a)) {
                    UObject* foot = reinterpret_cast<PointRegion*>(pawn::FootRegion(a))->zone;
                    if (waterZone(foot) && oldVelocity.z < 0) {
                        return oldVelocity * (1 - zoneinfo::ZoneFluidFriction(foot) * tick) + (acceleration + gravity) * (0.5f * tick);
                    }
                }
                return oldVelocity + (acceleration + gravity) * (0.5f * tick);
            }
            float mass = std::max(1.0f, actor::Mass(a));
            return oldVelocity * (1 - 2 * zoneFluidFriction(a) * tick) + (acceleration + gravity * (1.0f - actor::Buoyancy(a) / mass)) * (0.5f * tick);
        }

        void physFalling(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (isPawn(a) && physLadder(world, a, deltaTime, iterations)) {
                return;
            }
            if (region(a).zoneNumber == 0) {
                // Actor.FellOutOfWorld destroys the actor; Pawn.FellOutOfWorld kills the
                // pawn, the player included.
                vm::event(a, Name("FellOutOfWorld"));
                return;
            }
            // Air control limits acceleration only during the fall. Save the pawn's
            // acceleration so it can be restored afterward.
            Vector savedAcceleration = actor::Acceleration(a);
            float maxSpeed2D = 0;
            if (isPawn(a)) {
                Vector& acceleration = actor::Acceleration(a);
                float airControl = pawn::AirControl(a);
                if (airControl > 0.15f) {
                    // Little control while steering into something.
                    Vector steer = (actor::Velocity(a) + acceleration.normal() * (airControl * pawn::AccelRate(a))) * deltaTime;
                    Vector location = actor::Location(a);
                    CheckResult check;
                    world.lineCheck(check, location, location + Vector { steer.x, steer.y, 0 }, cylinderExtent(a), a, TRACE_World | TRACE_Blocking);
                    if (check.actor) {
                        airControl = 0.05f;
                    }
                }
                float maxAccel = airControl * pawn::AccelRate(a);
                acceleration.z = 0;
                float speed2d = actor::Velocity(a).size2D();
                if (speed2d < 10.0f) {
                    maxAccel = maxAccel + (10 - speed2d) / deltaTime;
                } else if (speed2d >= pawn::GroundSpeed(a)) {
                    if (airControl > 0.05f) {
                        maxSpeed2D = speed2d;
                    } else {
                        maxAccel = 1.0f;
                    }
                }
                if (acceleration.sizeSquared() > maxAccel * maxAccel) {
                    acceleration = acceleration.normal() * maxAccel;
                }
            }
            float remaining = deltaTime;
            int bounces = 0;
            CheckResult hit;
            bool adjustApex = false;
            while (remaining > 0 && iterations < 8) {
                iterations++;
                float tick = remaining > 0.1f ? std::min(0.1f, remaining * 0.5f) : remaining;
                remaining -= tick;
                actor::OldLocation(a) = actor::Location(a);
                actor::bJustTeleported(a) = 0;
                Vector oldVelocity = actor::Velocity(a);
                actor::Velocity(a) = fallingVelocity(a, oldVelocity, tick);
                Vector velocity = actor::Velocity(a);
                if (!adjustApex && ((oldVelocity.z > 0) != (velocity.z > 0)) && std::abs(oldVelocity.z) > 5.0f && std::abs(velocity.z) > 5.0f) {
                    adjustApex = true;
                    float part = std::abs(oldVelocity.z) / (std::abs(oldVelocity.z) + std::abs(velocity.z));
                    if (part * tick > 0.015f && (1 - part) * tick > 0.015f) {
                        remaining = remaining + tick * (1 - part);
                        tick = tick * part;
                        actor::Velocity(a) = fallingVelocity(a, oldVelocity, tick);
                    }
                } else {
                    adjustApex = false;
                }
                if (maxSpeed2D != 0) {
                    Vector& v = actor::Velocity(a);
                    if (v.x * v.x + v.y * v.y > maxSpeed2D * maxSpeed2D) {
                        Vector flat = Vector { v.x, v.y, 0 }.normal() * maxSpeed2D;
                        v = { flat.x, flat.y, v.z };
                    }
                }
                Vector zoneVel = zoneVelocity(a);
                if (isPawn(a) && !isPlayerPawn(a) && zoneVel.sizeSquared() <= 40000.0f) {
                    zoneVel = {};
                }
                Vector adjusted = (actor::Velocity(a) + zoneVel) * tick;
                move(world, a, adjusted, hit);
                bool hitSomething = hit.time < 1.0f;
                if (a->destroyed) {
                    return;
                }
                if (isPawn(a) && actor::Physics(a) == PHYS_Swimming) {
                    remaining = remaining + tick * (1.0f - hit.time);
                    startSwimming(world, a, oldVelocity, tick, remaining, iterations);
                    return;
                }
                if (hit.time < 1.0f) {
                    if (actor::bBounce(a)) {
                        vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                        if (actor::Physics(a) == PHYS_None || a->destroyed) {
                            return;
                        }
                        if (bounces < 2) {
                            remaining += tick * (1.0f - hit.time);
                        }
                        bounces++;
                    } else if (hit.normal.z > 0.7f) {
                        remaining += tick * (1.0f - hit.time);
                        if (!actor::bJustTeleported(a) && hit.time > 0.1f && hit.time * tick > 0.003f) {
                            actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / (tick * hit.time);
                        }
                        processLanded(world, a, hit.normal, hit.actor, remaining, iterations);
                        return;
                    } else {
                        processHitWall(a, hit.normal, hit.actor);
                        if (a->destroyed || actor::Physics(a) != PHYS_Falling) {
                            return;
                        }
                        Vector oldNormal = hit.normal;
                        Vector delta = (adjusted - hit.normal * adjusted.dot(hit.normal)) * (1.0f - hit.time);
                        if (delta.dot(adjusted) >= 0) {
                            move(world, a, delta, hit);
                            if (hit.time < 1.0f) {
                                if (hit.normal.z > 0.7f) {
                                    processLanded(world, a, hit.normal, hit.actor, 0, iterations);
                                    return;
                                }
                                processHitWall(a, hit.normal, hit.actor);
                                Vector desiredDir = adjusted.normal();
                                twoWallAdjust(desiredDir, delta, hit.normal, oldNormal, hit.time);
                                bool ditch = oldNormal.z > 0 && hit.normal.z > 0 && delta.z == 0 && hit.normal.dot(oldNormal) < 0;
                                move(world, a, delta, hit);
                                if (ditch || hit.normal.z > 0.7f) {
                                    processLanded(world, a, hit.normal, hit.actor, 0, iterations);
                                    return;
                                }
                            }
                        }
                        float oldZ = oldVelocity.z;
                        oldVelocity = (actor::Location(a) - actor::OldLocation(a)) / tick;
                        oldVelocity.z = oldZ;
                    }
                }
                // The current carries the actor, but its own velocity leaves the current out.
                // A bouncing actor keeps the velocity HitWall gave it.
                if ((!actor::bBounce(a) || !hitSomething) && !actor::bJustTeleported(a)) {
                    Vector newVelocity = (actor::Location(a) - actor::OldLocation(a)) / tick - zoneVel;
                    if (newVelocity.z < oldVelocity.z || oldVelocity.z >= 0) {
                        newVelocity = newVelocity * 2.0f - oldVelocity;
                    }
                    float terminal = zoneTerminalVelocity(a);
                    if (newVelocity.sizeSquared() > terminal * terminal) {
                        newVelocity = newVelocity.normal() * terminal;
                    }
                    actor::Velocity(a) = newVelocity;
                }
            }
            actor::Acceleration(a) = savedAcceleration;
        }

        void physFlying(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (actor::bCollideWorld(a) && region(a).zoneNumber == 0) {
                if (!pawn::bIsPlayer(a)) {
                    world.destroy(a);
                }
                return;
            }
            Vector acceleration = actor::Acceleration(a);
            Vector accelDir = acceleration.zero() ? Vector {} : acceleration.normal();
            calcVelocity(a, accelDir, deltaTime, pawn::AirSpeed(a), zoneFluidFriction(a), true, false, false);
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            Vector zoneVel = (isPlayerPawn(a) || zoneVelocity(a).sizeSquared() > 90000.0f) ? zoneVelocity(a) : Vector {};
            Vector adjusted = (actor::Velocity(a) + zoneVel) * deltaTime;
            CheckResult hit;
            move(world, a, adjusted, hit);
            if (hit.time < 1.0f && !a->destroyed) {
                Vector gravDir { 0, 0, zoneGravity(a).z > 0 ? 1.0f : -1.0f };
                Vector desiredDir = adjusted.normal();
                float upDown = gravDir.dot(actor::Velocity(a).normal());
                if (std::abs(hit.normal.z) < 0.2f && upDown < 0.5f && upDown > -0.2f) {
                    float stepZ = actor::Location(a).z;
                    stepUp(world, a, gravDir, desiredDir, adjusted * (1.0f - hit.time), hit);
                    actor::OldLocation(a).z = actor::Location(a).z + (actor::OldLocation(a).z - stepZ);
                } else {
                    processHitWall(a, hit.normal, hit.actor);
                    Vector oldNormal = hit.normal;
                    Vector delta = (adjusted - hit.normal * adjusted.dot(hit.normal)) * (1.0f - hit.time);
                    if (delta.dot(adjusted) >= 0) {
                        move(world, a, delta, hit);
                        if (hit.time < 1.0f) {
                            processHitWall(a, hit.normal, hit.actor);
                            twoWallAdjust(desiredDir, delta, hit.normal, oldNormal, hit.time);
                            move(world, a, delta, hit);
                        }
                    }
                }
            }
            if (!actor::bJustTeleported(a)) {
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaTime;
            }
            (void)iterations;
        }

        void physSwimming(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (deltaTime <= 0) {
                return;
            }
            if (physLadder(world, a, deltaTime, iterations)) {
                return;
            }
            UObject* head = reinterpret_cast<PointRegion*>(pawn::HeadRegion(a))->zone;
            if (!waterZone(head) && actor::Velocity(a).z > 100.0f) {
                actor::Velocity(a).z = actor::Velocity(a).z * (1 - deltaTime);
            }
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            Vector acceleration = actor::Acceleration(a);
            Vector accelDir = acceleration.zero() ? Vector {} : acceleration.normal();
            calcVelocity(a, accelDir, deltaTime, pawn::WaterSpeed(a), zoneFluidFriction(a), true, false, true);
            float velocityZ = actor::Velocity(a).z;
            // Deus Ex scales a water current by 25 times the tick here, unlike when flying or falling.
            Vector zoneVel = (isPlayerPawn(a) || zoneVelocity(a).sizeSquared() > 90000.0f) ? zoneVelocity(a) * (25.0f * deltaTime) : Vector {};
            Vector adjusted = (actor::Velocity(a) + zoneVel) * deltaTime;
            CheckResult hit;
            // The time left over once the move has carried the pawn out of the water.
            float remaining = swim(world, a, adjusted, hit) * deltaTime;
            if (a->destroyed) {
                return;
            }
            if (hit.time < 1.0f) {
                Vector gravDir { 0, 0, zoneGravity(a).z > 0 ? 1.0f : -1.0f };
                Vector desiredDir = adjusted.normal();
                float upDown = gravDir.dot(actor::Velocity(a).normal());
                if (std::abs(hit.normal.z) < 0.2f && upDown < 0.5f && upDown > -0.2f) {
                    float stepZ = actor::Location(a).z;
                    stepUp(world, a, gravDir, desiredDir, adjusted * (1.0f - hit.time), hit);
                    actor::OldLocation(a).z = actor::Location(a).z + (actor::OldLocation(a).z - stepZ);
                } else {
                    processHitWall(a, hit.normal, hit.actor);
                    if (a->destroyed) {
                        return;
                    }
                    Vector oldNormal = hit.normal;
                    Vector delta = (adjusted - hit.normal * adjusted.dot(hit.normal)) * (1.0f - hit.time);
                    if (delta.dot(adjusted) >= 0) {
                        // Use the slide's hit time here. There is no time left to scale
                        // unless the first move took the pawn out of the water.
                        float airTime = swim(world, a, delta, hit);
                        remaining = remaining * (1.0f - hit.time) * airTime;
                        if (hit.time < 1.0f && !a->destroyed) {
                            processHitWall(a, hit.normal, hit.actor);
                            twoWallAdjust(desiredDir, delta, hit.normal, oldNormal, hit.time);
                            airTime = swim(world, a, delta, hit);
                            remaining = remaining * (1.0f - hit.time) * airTime;
                        }
                        if (a->destroyed) {
                            return;
                        }
                    }
                }
            }
            if (!actor::bJustTeleported(a) && remaining < deltaTime) {
                // A zone change during the move, such as the water jump PlayerSwimming starts on
                // leaving the water, sets the vertical speed; keep it over the distance moved.
                bool zoneSetZ = velocityZ != actor::Velocity(a).z;
                velocityZ = actor::Velocity(a).z;
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / (deltaTime - remaining);
                if (zoneSetZ) {
                    actor::Velocity(a).z = velocityZ;
                }
            }
            if (!waterZone(zoneOf(a))) {
                if (actor::Physics(a) == PHYS_Swimming) {
                    setPhysics(a, PHYS_Falling, nullptr);
                }
                Vector& velocity = actor::Velocity(a);
                if (velocity.z < 160.0f && velocity.z > 0) {
                    velocity.z = 40.0f + velocity.size2D() * 0.4f;
                }
            }
            if (remaining > 0.01f) {
                if (actor::Physics(a) == PHYS_Falling) {
                    physFalling(world, a, remaining, iterations + 1);
                } else if (actor::Physics(a) == PHYS_Flying) {
                    physFlying(world, a, remaining, iterations + 1);
                }
            }
        }

        void physProjectile(World& world, UObject* a, float deltaTime, int iterations)
        {
            if (region(a).zoneNumber == 0) {
                world.destroy(a);
                return;
            }
            float remaining = deltaTime;
            int bounces = 0;
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            CheckResult hit;
            while (remaining > 0 && iterations < 8) {
                iterations++;
                if (waterZone(zoneOf(a))) {
                    actor::Velocity(a) = actor::Velocity(a) * (1 - 0.2f * zoneFluidFriction(a) * remaining);
                }
                actor::Velocity(a) = actor::Velocity(a) + actor::Acceleration(a) * remaining;
                float tick = remaining;
                remaining = 0;
                if (a->isA(engineClasses().projectile)) {
                    float maxSpeed = projectile::MaxSpeed(a);
                    if (actor::Velocity(a).sizeSquared() > maxSpeed * maxSpeed) {
                        actor::Velocity(a) = actor::Velocity(a).normal() * maxSpeed;
                    }
                }
                Vector adjusted = actor::Velocity(a) * deltaTime;
                move(world, a, adjusted, hit);
                if (hit.time < 1.0f && !a->destroyed && !actor::bJustTeleported(a)) {
                    vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                    if (a->destroyed) {
                        return;
                    }
                    if (actor::bBounce(a)) {
                        if (bounces < 2) {
                            remaining = tick * (1.0f - hit.time);
                        }
                        bounces++;
                        if (actor::Physics(a) == PHYS_Falling) {
                            physFalling(world, a, remaining, iterations);
                        }
                    }
                }
            }
            if (!actor::bBounce(a) && !actor::bJustTeleported(a) && !a->destroyed) {
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaTime;
            }
        }

        float splerp(float f)
        {
            float s = f * f;
            return (1.0f / 16.0f) * s * s - 0.5f * s + 1;
        }

        // FRotator * float truncates each axis, so a weighted sum of rotators is the
        // sum of the truncated parts.
        Rotator scaleRotator(const Rotator& r, float s)
        {
            return { static_cast<int32_t>(static_cast<float>(r.pitch) * s), static_cast<int32_t>(static_cast<float>(r.yaw) * s), static_cast<int32_t>(static_cast<float>(r.roll) * s) };
        }

        Rotator mixRotators(const Rotator* rotators, const float* weights, int count)
        {
            Rotator sum;
            for (int i = 0; i < count; i++) {
                Rotator part = scaleRotator(rotators[i], weights[i]);
                sum = { sum.pitch + part.pitch, sum.yaw + part.yaw, sum.roll + part.roll };
            }
            return sum;
        }

        UObject* interpolationLink(UObject* point, const char* name)
        {
            uint8_t* address = point ? point->propertyAddress(Name(name)) : nullptr;
            return address ? *reinterpret_cast<UObject**>(address) : nullptr;
        }

        float interpolationFloat(UObject* point, const char* name, float fallback)
        {
            uint8_t* address = point ? point->propertyAddress(Name(name)) : nullptr;
            return address ? *reinterpret_cast<float*>(address) : fallback;
        }

        bool skipsNextPath(UObject* point)
        {
            static const Name nameSkip("bSkipNextPath");
            UClass* interpolationClass = engineClasses().interpolationPoint;
            if (!point || !interpolationClass || !point->isA(interpolationClass)) {
                return false;
            }
            uint8_t* address = point->propertyAddress(nameSkip);
            return address && *reinterpret_cast<uint32_t*>(address) != 0;
        }

        // AActor::physPathing. Deus Ex's
        // version, like UT's, also takes the game speed from the points and, for a
        // player, the field of view and screen flash, all blended at the alpha
        // the tick starts from.
        void applyInterpolationPoint(World& world, UObject* a, UObject* dest, UObject* next, float alpha)
        {
            auto blend = [&](const char* name) {
                return interpolationFloat(next, name, 1.0f) * alpha + (1.0f - alpha) * interpolationFloat(dest, name, 1.0f);
            };
            if (world.levelInfo) {
                levelinfo::TimeDilation(world.levelInfo) = blend("GameSpeedModifier");
            }
            if (!a->isA(engineClasses().playerPawn)) {
                return;
            }
            float flashScale = blend("ScreenFlashScale");
            playerpawn::DesiredFlashScale(a) = flashScale;
            playerpawn::FlashScale(a) = { flashScale, flashScale, flashScale };
            Vector destFog;
            Vector nextFog;
            if (uint8_t* address = dest->propertyAddress(Name("ScreenFlashFog"))) {
                destFog = *reinterpret_cast<Vector*>(address);
            }
            if (uint8_t* address = next->propertyAddress(Name("ScreenFlashFog"))) {
                nextFog = *reinterpret_cast<Vector*>(address);
            }
            Vector fog = destFog * (1.0f - alpha) + nextFog * alpha;
            playerpawn::DesiredFlashFog(a) = fog;
            playerpawn::FlashFog(a) = fog;
            UObject* defaults = a->cls ? a->cls->defaultObject : nullptr;
            if (defaults) {
                pawn::FovAngle(a) = blend("FovModifier") * pawn::FovAngle(defaults);
            }
        }

        void physPathing(World& world, UObject* a, float deltaTime)
        {
            UClass* interpolationClass = engineClasses().interpolationPoint;
            int guard = 0;
            while (actor::PhysRate(a) != 0 && actor::bInterpolating(a) && deltaTime > 0 && guard++ < 16) {
                UObject* target = actor::Target(a);
                UObject* dest = (target && interpolationClass && target->isA(interpolationClass)) ? target : nullptr;
                UObject* next = interpolationLink(dest, "Next");
                UObject* prev = interpolationLink(dest, "Prev");
                float rateModifier = 1.0f;
                float& alpha = actor::PhysAlpha(a);
                if (dest && next) {
                    rateModifier = interpolationFloat(next, "RateModifier", 1.0f) * alpha + (1 - alpha) * interpolationFloat(dest, "RateModifier", 1.0f);
                    applyInterpolationPoint(world, a, dest, next, alpha);
                }
                float oldAlpha = alpha;
                float destAlpha = rateModifier * actor::PhysRate(a) * deltaTime + alpha;
                alpha = std::clamp(destAlpha, 0.0f, 1.0f);
                if (dest && next) {
                    UObject* nextNext = interpolationLink(next, "Next");
                    Vector newLocation;
                    Rotator newRotation;
                    if (prev && nextNext) {
                        // Weighted by the unnormalized spline weights, then scaled by
                        // their reciprocal sum, as the original does.
                        float w[4] = { splerp(alpha + 1), splerp(alpha), splerp(alpha - 1), splerp(alpha - 2) };
                        float inv = 1.0f / (w[3] + w[2] + w[1] + w[0]);
                        newLocation = (actor::Location(prev) * w[0] + actor::Location(dest) * w[1] + actor::Location(next) * w[2] + actor::Location(nextNext) * w[3]) * inv;
                        Rotator rotators[4] = { actor::Rotation(prev), actor::Rotation(dest), actor::Rotation(next), actor::Rotation(nextNext) };
                        newRotation = scaleRotator(mixRotators(rotators, w, 4), inv);
                    } else {
                        float w[2] = { 1 - alpha, alpha };
                        newLocation = actor::Location(next) * w[1] + actor::Location(dest) * w[0];
                        Rotator rotators[2] = { actor::Rotation(dest), actor::Rotation(next) };
                        newRotation = mixRotators(rotators, w, 2);
                    }
                    CheckResult hit;
                    world.moveActor(a, newLocation - actor::Location(a), newRotation, hit, false, false, false, false);
                    if (isPawn(a)) {
                        pawn::ViewRotation(a) = actor::Rotation(a);
                    }
                }
                if (actor::PhysRate(a) > 0 && destAlpha > 1.0f) {
                    alpha = 0;
                    deltaTime *= (destAlpha - 1.0f) / (destAlpha - oldAlpha);
                    if (target) {
                        vm::Call(target, Name("InterpolateEnd")).obj(a).run();
                        vm::Call(a, Name("InterpolateEnd")).obj(target).run();
                        // The original steps on from the point it started the segment
                        // at, past any point marked bSkipNextPath.
                        // (The original would loop forever on a ring of skipped points.)
                        UObject* point = dest;
                        for (int steps = 0; point && steps < 1024; steps++) {
                            point = interpolationLink(point, "Next");
                            actor::Target(a) = point;
                            if (!skipsNextPath(point)) {
                                break;
                            }
                        }
                    }
                } else if (actor::PhysRate(a) < 0 && destAlpha < 0) {
                    alpha = 1;
                    deltaTime *= (0 - destAlpha) / (oldAlpha - destAlpha);
                    if (target) {
                        vm::Call(target, Name("InterpolateEnd")).obj(a).run();
                        vm::Call(a, Name("InterpolateEnd")).obj(target).run();
                        UObject* point = dest;
                        for (int steps = 0; point && steps < 1024; steps++) {
                            point = interpolationLink(point, "Prev");
                            actor::Target(a) = point;
                            if (!skipsNextPath(point)) {
                                break;
                            }
                        }
                    }
                    vm::Call(a, Name("InterpolateEnd")).obj(nullptr).run();
                } else {
                    deltaTime = 0;
                }
            }
        }

        void physMovingBrush(World& world, UObject* a, float deltaTime)
        {
            if (!isMover(a)) {
                return;
            }
            int32_t keyNum = std::clamp<int32_t>(mover::KeyNum(a), 0, 7);
            int guard = 0;
            while (actor::bInterpolating(a) && deltaTime > 0 && guard++ < 8) {
                float alpha = actor::PhysAlpha(a);
                float newAlpha = alpha + deltaTime * actor::PhysRate(a);
                if (newAlpha > 1.0f) {
                    deltaTime *= (newAlpha - 1.0f) / (newAlpha - alpha);
                    newAlpha = 1.0f;
                } else {
                    deltaTime = 0;
                }
                float renderAlpha = newAlpha;
                if (mover::MoverGlideType(a) == 1) {
                    renderAlpha = 3.0f * newAlpha * newAlpha - 2.0f * newAlpha * newAlpha * newAlpha;
                }
                Vector oldPos = mover::OldPos(a);
                Rotator oldRot = mover::OldRot(a);
                Vector targetPos = mover::BasePos(a) + mover::KeyPos(a, keyNum);
                Rotator baseRot = mover::BaseRot(a);
                Rotator keyRot = mover::KeyRot(a, keyNum);
                Rotator targetRot { baseRot.pitch + keyRot.pitch, baseRot.yaw + keyRot.yaw, baseRot.roll + keyRot.roll };
                Vector newLocation = oldPos + (targetPos - oldPos) * renderAlpha;
                Rotator newRotation {
                    oldRot.pitch + static_cast<int32_t>(static_cast<float>(targetRot.pitch - oldRot.pitch) * renderAlpha),
                    oldRot.yaw + static_cast<int32_t>(static_cast<float>(targetRot.yaw - oldRot.yaw) * renderAlpha),
                    oldRot.roll + static_cast<int32_t>(static_cast<float>(targetRot.roll - oldRot.roll) * renderAlpha),
                };
                CheckResult hit;
                if (world.moveActor(a, newLocation - actor::Location(a), newRotation, hit, false, false, false, false)) {
                    actor::PhysAlpha(a) = newAlpha;
                    if (newAlpha == 1.0f) {
                        actor::bInterpolating(a) = 0;
                        vm::Call(a, Name("InterpolateEnd")).obj(nullptr).run();
                    }
                }
            }
        }

        bool nearlyZero(const Vector& v)
        {
            return std::abs(v.x) < 1e-4f && std::abs(v.y) < 1e-4f && std::abs(v.z) < 1e-4f;
        }

        bool checkFloor(World& world, UObject* a, const Vector& direction, CheckResult& hit)
        {
            Vector location = actor::Location(a);
            world.lineCheck(hit, location, location - direction * pawn::MaxStepHeight(a), cylinderExtent(a), nullptr, TRACE_World);
            if (hit.time < 1.0f) {
                pawn::Floor(a) = hit.normal;
                return true;
            }
            return false;
        }

        bool findNewFloor(World& world, UObject* a, const Vector& oldLocation, float deltaTime, float remainingTime, int iterations)
        {
            CheckResult hit;
            for (const Vector& direction : { Vector { 0, 0, 1 }, Vector { 0, 1, 0 }, Vector { 0, -1, 0 }, Vector { 1, 0, 0 }, Vector { -1, 0, 0 } }) {
                if (checkFloor(world, a, direction, hit)) {
                    return true;
                }
            }
            vm::event(a, Name("Falling"));
            if (actor::Physics(a) == PHYS_Spider) {
                setPhysics(a, PHYS_Falling, nullptr);
            }
            if (actor::Physics(a) == PHYS_Falling) {
                float velocityZ = actor::Velocity(a).z;
                if (!actor::bJustTeleported(a) && deltaTime > remainingTime) {
                    actor::Velocity(a) = (actor::Location(a) - oldLocation) / (deltaTime - remainingTime);
                }
                actor::Velocity(a).z = velocityZ;
                if (remainingTime > 0.005f) {
                    physFalling(world, a, remainingTime, iterations);
                }
            }
            return false;
        }

        void physSpider(World& world, UObject* a, float deltaTime, int iterations)
        {
            Vector& velocity = actor::Velocity(a);
            Vector& acceleration = actor::Acceleration(a);
            Vector accelDir;
            if (acceleration.zero()) {
                Vector oldVelocity = velocity;
                velocity = velocity - velocity * (2 * deltaTime * zoneGroundFriction(a));
                if (oldVelocity.dot(velocity) < 0) {
                    velocity = acceleration;
                }
            } else {
                accelDir = acceleration.normal();
                float velocitySize = velocity.size();
                float accelRate = pawn::AccelRate(a);
                if (acceleration.sizeSquared() > accelRate * accelRate) {
                    acceleration = accelDir * accelRate;
                }
                velocity = velocity - (velocity - accelDir * velocitySize) * (deltaTime * zoneGroundFriction(a));
            }
            velocity = velocity + acceleration * deltaTime;
            float maxSpeed = pawn::GroundSpeed(a) * pawn::DesiredSpeed(a);
            iterations++;
            if (velocity.sizeSquared() > maxSpeed * maxSpeed) {
                velocity = velocity.normal() * maxSpeed;
            }
            Vector zoneVel = zoneVelocity(a).sizeSquared() > 90000.0f ? zoneVelocity(a) : Vector {};
            Vector desiredMove = velocity + zoneVel;
            float moveSize = desiredMove.size();
            Vector desiredDir = desiredMove / moveSize;
            bool needFloor = nearlyZero(pawn::Floor(a));
            CheckResult hit;
            Vector gravDir = pawn::Floor(a) * -1.0f;
            Vector down = gravDir * (pawn::MaxStepHeight(a) + 4.0f);
            actor::DesiredRotation(a) = actor::Rotation(a);
            if (!needFloor) {
                Vector location = actor::Location(a);
                world.lineCheck(hit, location, location + down, cylinderExtent(a), nullptr, TRACE_World);
                needFloor = hit.time == 1.0f;
            }
            if (needFloor) {
                if (!findNewFloor(world, a, actor::Location(a), deltaTime, deltaTime, iterations)) {
                    return;
                }
                gravDir = pawn::Floor(a) * -1.0f;
                down = gravDir * (pawn::MaxStepHeight(a) + 4.0f);
            }
            Rotator desiredRotation = vectorRotation(pawn::Floor(a));
            desiredRotation.pitch -= 16384;
            desiredRotation.roll = 0;
            actor::DesiredRotation(a) = desiredRotation;
            float dotp = accelDir.dot(pawn::Floor(a));
            Vector realDir = desiredDir;
            if (pawn::Floor(a).z < 0.6f && dotp > 0.9f) {
                pawn::Floor(a) = {};
                vm::event(a, Name("Falling"));
                setPhysics(a, PHYS_Falling, nullptr);
                physFalling(world, a, deltaTime, iterations);
                return;
            }
            // At zero speed, desiredDir contains NaN. Normalizing the projected direction
            // gives a zero vector, so the pawn stays put but still checks for a floor.
            Vector projected = desiredDir - pawn::Floor(a) * desiredDir.dot(pawn::Floor(a));
            desiredDir = std::isnan(projected.sizeSquared()) ? Vector {} : projected.normal();
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            float remaining = deltaTime;
            desiredMove = desiredDir * moveSize;
            while (remaining > 0 && iterations < 8) {
                iterations++;
                float tick = remaining > 0.05f ? std::min(0.05f, remaining * 0.5f) : remaining;
                remaining -= tick;
                Vector delta = desiredMove * tick;
                if (!nearlyZero(delta)) {
                    world.moveActor(a, delta, actor::DesiredRotation(a), hit, false, false, false, false);
                    if (a->destroyed) {
                        return;
                    }
                    if (hit.time < 1.0f) {
                        if (hit.normal.z >= 0) {
                            if (hit.normal.dot(realDir) < 0 && pawn::Floor(a).dot(realDir) < 0) {
                                vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                            } else {
                                Vector combo = (hit.normal + pawn::Floor(a)).normal();
                                if (realDir.dot(combo) > 0.9f) {
                                    vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                                }
                            }
                            pawn::Floor(a) = hit.normal;
                            gravDir = pawn::Floor(a) * -1.0f;
                            down = gravDir * (pawn::MaxStepHeight(a) + 4.0f);
                        } else if (hit.normal.dot(realDir) < 0) {
                            vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                        } else if (pawn::Floor(a).dot(realDir) > 0.7f) {
                            vm::event(a, Name("Falling"));
                            if (actor::Physics(a) == PHYS_Spider) {
                                setPhysics(a, PHYS_Falling, nullptr);
                            }
                            if (actor::Physics(a) == PHYS_Falling) {
                                float velocityZ = actor::Velocity(a).z;
                                if (!actor::bJustTeleported(a) && deltaTime > remaining) {
                                    actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / (deltaTime - remaining);
                                }
                                actor::Velocity(a).z = velocityZ;
                                if (remaining > 0.005f) {
                                    physFalling(world, a, remaining, iterations);
                                }
                                return;
                            }
                        }
                        stepUp(world, a, gravDir, delta.normal(), delta * (1.0f - hit.time), hit);
                        if (actor::Physics(a) == PHYS_Falling) {
                            if (remaining > 0.005f) {
                                physFalling(world, a, remaining, iterations);
                            }
                            return;
                        }
                    }
                }
                move(world, a, down, hit);
                if (a->destroyed) {
                    return;
                }
                if (hit.time == 1.0f) {
                    if (!findNewFloor(world, a, actor::OldLocation(a), deltaTime, remaining, iterations)) {
                        return;
                    }
                    gravDir = pawn::Floor(a) * -1.0f;
                    down = gravDir * (pawn::MaxStepHeight(a) + 4.0f);
                } else {
                    pawn::Floor(a) = hit.normal;
                    if (hit.actor != actor::Base(a) && !isPawn(hit.actor)) {
                        setSupportBase(a, hit.actor);
                    }
                }
            }
            if (!actor::bJustTeleported(a)) {
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaTime;
            }
        }

        // AActor::physTrailer.
        void physTrailer(World& world, UObject* a)
        {
            UObject* owner = actor::Owner(a);
            if (!owner) {
                return;
            }
            const uint8_t DT_Sprite = 1;
            if (actor::DrawType(a) != DT_Sprite) {
                // Meshes and the rest go to the owner and face against its velocity, or as it does.
                world.farMoveActor(a, actor::Location(owner), false, true);
                Rotator rotation;
                if (actor::bTrailerSameRotation(a)) {
                    rotation = actor::Rotation(owner);
                } else if (nearlyZero(actor::Velocity(owner))) {
                    rotation = { 16384, 0, 0 };
                } else {
                    rotation = vectorRotation(actor::Velocity(owner) * -1.0f);
                }
                CheckResult hit;
                world.moveActor(a, {}, rotation, hit, false, false, false, false);
                return;
            }
            // Sprites only move: offset by PrePivot, or Mass units behind the owner when sharing its rotation.
            Vector location = actor::Location(owner);
            if (actor::bTrailerPrePivot(a)) {
                location = location + actor::PrePivot(a);
            } else if (actor::bTrailerSameRotation(a)) {
                location = location - rotationVector(actor::Rotation(owner)) * actor::Mass(a);
            }
            world.farMoveActor(a, location, false, true);
        }

        void physRolling(World& world, UObject* a, float deltaTime, int iterations)
        {
            Vector& velocity = actor::Velocity(a);
            Vector velDir = velocity.normal();
            Vector accelDir = actor::Acceleration(a).normal();
            velocity = velocity - (velDir - accelDir) * (velocity.size() * deltaTime * zoneGroundFriction(a));
            velocity = velocity * (1 - zoneFluidFriction(a) * deltaTime) + actor::Acceleration(a) * deltaTime;
            Vector desiredMove = velocity + zoneVelocity(a);
            actor::OldLocation(a) = actor::Location(a);
            actor::bJustTeleported(a) = 0;
            float remaining = deltaTime;
            Vector gravDir { 0, 0, zoneGravity(a).z > 0 ? 1.0f : -1.0f };
            Vector down = gravDir * 16.0f;
            CheckResult hit;
            int bounces = 0;
            while (remaining > 0 && iterations < 8) {
                iterations++;
                float tick = remaining > 0.1f ? std::min(0.1f, remaining * 0.5f) : remaining;
                remaining -= tick;
                Vector delta = desiredMove * tick;
                Vector subMove = delta;
                Vector subLocation = actor::Location(a);
                if (!(std::abs(delta.x) < 1e-4f && std::abs(delta.y) < 1e-4f && std::abs(delta.z) < 1e-4f)) {
                    move(world, a, delta, hit);
                    if (hit.time < 1.0f) {
                        vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                        if (actor::bBounce(a)) {
                            if (bounces < 2) {
                                remaining += tick * (1.0f - hit.time);
                            }
                            bounces++;
                        } else {
                            Vector original = delta;
                            Vector oldNormal = hit.normal;
                            delta = (delta - hit.normal * delta.dot(hit.normal)) * (1.0f - hit.time);
                            if (delta.dot(original) >= 0) {
                                move(world, a, delta, hit);
                                if (hit.time < 1.0f) {
                                    vm::Call(a, Name("HitWall")).v(hit.normal).obj(hit.actor).run();
                                    twoWallAdjust(desiredMove.normal(), delta, hit.normal, oldNormal, hit.time);
                                    move(world, a, delta, hit);
                                }
                            }
                        }
                    }
                }
                if (a->destroyed) {
                    return;
                }
                move(world, a, down, hit);
                float dropTime = hit.time;
                float dropHitZ = hit.normal.z;
                if (dropTime < 1.0f && hit.normal.z < 1.0f && hit.normal.z * zoneGroundFriction(a) < 3.3f) {
                    Vector slide = zoneGravity(a) * (deltaTime / (2 * std::max(0.5f, zoneGroundFriction(a))) * deltaTime);
                    delta = slide - hit.normal * slide.dot(hit.normal);
                    if (delta.dot(slide) >= 0) {
                        move(world, a, delta, hit);
                        dropHitZ = std::max(dropHitZ, hit.normal.z);
                    }
                }
                if (dropTime == 1.0f || dropHitZ < 0.7f) {
                    move(world, a, down * (-dropTime), hit);
                    float desiredDist = subMove.size();
                    float actualDist = (actor::Location(a) - subLocation).size2D();
                    remaining += tick * (1 - std::min(1.0f, actualDist / desiredDist));
                    vm::event(a, Name("Falling"));
                    if (actor::Physics(a) == PHYS_Rolling) {
                        setPhysics(a, PHYS_Falling, nullptr);
                    }
                    if (actor::Physics(a) == PHYS_Falling) {
                        if (!actor::bJustTeleported(a) && deltaTime > remaining) {
                            actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / (deltaTime - remaining);
                        }
                        actor::Velocity(a).z = 0;
                        if (remaining > 0.005f) {
                            physFalling(world, a, remaining, iterations);
                        }
                        return;
                    }
                    move(world, a, desiredMove * remaining, hit);
                } else if (hit.actor != actor::Base(a)) {
                    setSupportBase(a, hit.actor);
                }
            }
            if (!actor::bJustTeleported(a)) {
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaTime;
            }
        }
    }

    namespace {
        void postTouch(UObject* a)
        {
            UObject* pending = actor::PendingTouch(a);
            if (!pending) {
                return;
            }
            vm::Call(pending, Name("PostTouch")).obj(a).run();
            pending = actor::PendingTouch(a);
            if (!pending) {
                return;
            }
            actor::PendingTouch(a) = actor::PendingTouch(pending);
            actor::PendingTouch(pending) = nullptr;
        }
    }

    void processHitWall(UObject* a, const Vector& hitNormal, UObject* hitActor)
    {
        if (hitActor && isPawn(hitActor)) {
            return;
        }
        if (isPawn(a)) {
            Vector direction = (pawn::Destination(a) - actor::Location(a)).normal();
            Vector normal = hitNormal;
            if (actor::Physics(a) == PHYS_Walking) {
                normal.z = 0;
                direction.z = 0;
            }
            if (pawn::MinHitWall(a) < direction.dot(normal)) {
                return;
            }
            if (actor::Acceleration(a).zero()) {
                return;
            }
            if (!probing(a, "HitWall") && actor::Physics(a) != PHYS_Falling) {
                pawn::MoveTimer(a) = -1.0f;
                pawn::bFromWall(a) = 1;
                return;
            }
        } else if (!probing(a, "HitWall")) {
            return;
        }
        vm::Call(a, Name("HitWall")).v(hitNormal).obj(hitActor).run();
    }

    void processLanded(World& world, UObject* a, const Vector& hitNormal, UObject* hitActor, float remainingTime, int iterations)
    {
        constexpr uint32_t landTrace = TRACE_World | TRACE_Pawns | TRACE_Others | TRACE_Blocking;
        UObject* zone = zoneOf(a);
        // Anything but a pawn that lands in a zone with bBounceVelocity is thrown back up.
        if (!isPawn(a) && zone && zoneinfo::bBounceVelocity(zone) && !zoneinfo::ZoneVelocity(zone).zero()) {
            actor::Velocity(a) = zoneinfo::ZoneVelocity(zone) + Vector { 0, 0, 80 };
            return;
        }
        if (isPawn(a)) {
            // A pawn whose feet are not all on something, such as one landing on the edge of
            // another pawn's head, is moved aside.
            CheckResult hit;
            Vector location = actor::Location(a);
            world.lineCheck(hit, location, location - Vector { 0, 0, 0.2f * actor::CollisionRadius(a) + 8 }, cylinderExtent(a) * 0.9f, a, landTrace | TRACE_OnlyProjActor);
            if (hit.time >= 1.0f) {
                Vector adjusted = location;
                if (world.findSpot(cylinderExtent(a) * 1.1f, adjusted, false, false) && !(adjusted == location)) {
                    world.farMoveActor(a, adjusted, false, false);
                    actor::Velocity(a).x += frand() * 60 - 30;
                    actor::Velocity(a).y += frand() * 60 - 30;
                    return;
                }
            }
        } else if (a->isA(engineClasses().decoration)) {
            if (decoration::numLandings(a) < 5) {
                float radius = actor::CollisionRadius(a);
                float height = actor::CollisionHeight(a);
                Vector location = actor::Location(a);
                CheckResult hit;
                world.lineCheck(hit, location - Vector { 0, 0, 0.8f * height }, location - Vector { 0, 0, height + radius + 8 }, {}, a, landTrace | TRACE_OnlyProjActor);
                if (!hit.actor) {
                    // Nothing under the middle: if two neighboring quarters hang over an
                    // edge, it tips off that way.
                    Vector extent { 0.5f * radius, 0.5f * radius, height };
                    auto clear = [&](float x, float y) {
                        Vector start = location + Vector { x * 0.5f * radius, y * 0.5f * radius, 0 };
                        CheckResult corner;
                        return world.lineCheck(corner, start, start - Vector { 0, 0, 8 }, extent, a, landTrace) ? 0 : 1;
                    };
                    int c1 = clear(1, 1);
                    int c2 = clear(-1, 1);
                    int c3 = clear(-1, -1);
                    int c4 = clear(1, -1);
                    if (c1 + c2 + c3 + c4 > 1 && c1 + c3 != 0 && c2 + c4 != 0) {
                        decoration::numLandings(a)++;
                        float speed = -actor::Velocity(a).z;
                        speed = speed < 30.0f ? 30.0f : std::min(speed, radius + 30.0f);
                        speed *= 2.0f;
                        actor::Velocity(a) = { static_cast<float>(c1 + c4 - c2 - c3) * speed, static_cast<float>(c1 + c2 - c3 - c4) * speed, 0.5f * speed };
                        return;
                    }
                }
                // A sliding carcass slides off slopes.
                if (a->isA(engineClasses().carcass) && hitNormal.z < 0.9f && carcass::bSlidingCarcass(a)) {
                    if (frand() < 0.2f) {
                        decoration::numLandings(a)++;
                    }
                    actor::Velocity(a) = hitNormal * 120.0f;
                    actor::Velocity(a).z = 70.0f;
                    return;
                }
            }
            decoration::numLandings(a) = 0;
        }
        vm::Call(a, Name("Landed")).v(hitNormal).run();
        if (a->destroyed) {
            return;
        }
        if (actor::Physics(a) == PHYS_Falling) {
            setPhysics(a, isPawn(a) ? PHYS_Walking : PHYS_None, hitActor);
        }
        if (actor::Physics(a) == PHYS_Walking && isPawn(a)) {
            actor::Acceleration(a) = actor::Acceleration(a).normal();
            if (remainingTime > 0.01f) {
                physWalking(world, a, remainingTime, iterations);
            }
        }
    }

    void performPhysics(World* world, UObject* a, float deltaSeconds)
    {
        uint8_t physics = actor::Physics(a);
        if (isPawn(a)) {
            switch (physics) {
            case PHYS_Walking:
                physWalking(*world, a, deltaSeconds, 0);
                break;
            case PHYS_Falling:
                physFalling(*world, a, deltaSeconds, 0);
                break;
            case PHYS_Flying:
                physFlying(*world, a, deltaSeconds, 0);
                break;
            case PHYS_Swimming:
                physSwimming(*world, a, deltaSeconds, 0);
                break;
            case PHYS_Spider:
                physSpider(*world, a, deltaSeconds, 0);
                break;
            case PHYS_Interpolating:
                actor::OldLocation(a) = actor::Location(a);
                physPathing(*world, a, deltaSeconds);
                actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaSeconds;
                break;
            default:
                break;
            }
            if (a->destroyed) {
                return;
            }
            Rotator rotation = actor::Rotation(a);
            if (actor::Physics(a) != PHYS_Spider && (isPlayerPawn(a) || !(rotation == actor::DesiredRotation(a)) || actor::RotationRate(a).roll > 0)) {
                pawnRotation(*world, a, deltaSeconds);
            }
            pawn::MoveTimer(a) -= deltaSeconds;
            pawn::AvgPhysicsTime(a) = 0.8f * pawn::AvgPhysicsTime(a) + 0.2f * deltaSeconds;
            postTouch(a);
            return;
        }
        switch (physics) {
        case PHYS_Projectile:
            physProjectile(*world, a, deltaSeconds, 0);
            break;
        case PHYS_Falling:
            physFalling(*world, a, deltaSeconds, 0);
            break;
        case PHYS_Interpolating:
            actor::OldLocation(a) = actor::Location(a);
            physPathing(*world, a, deltaSeconds);
            actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaSeconds;
            break;
        case PHYS_MovingBrush:
            actor::OldLocation(a) = actor::Location(a);
            physMovingBrush(*world, a, deltaSeconds);
            actor::Velocity(a) = (actor::Location(a) - actor::OldLocation(a)) / deltaSeconds;
            break;
        case PHYS_Trailer:
            physTrailer(*world, a);
            break;
        case PHYS_Rolling:
            physRolling(*world, a, deltaSeconds, 0);
            break;
        default:
            break;
        }
        if (a->destroyed) {
            return;
        }
        Rotator rate = actor::RotationRate(a);
        if ((rate.pitch & 65535) != 0 || (rate.yaw & 65535) != 0 || (rate.roll & 65535) != 0) {
            actorRotation(*world, a, deltaSeconds);
        }
        postTouch(a);
    }
}
