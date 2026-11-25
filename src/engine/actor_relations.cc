#include "engine/actor.h"

#include "core/vm.h"
#include "engine/world.h"

namespace dx {
    bool isBlockedBy(const World& world, UObject* a, UObject* other)
    {
        if (other == world.levelInfo) {
            return actor::bCollideWorld(a) != 0;
        }
        bool aPlayer = playerPawnOf(a) != nullptr;
        bool otherPlayer = playerPawnOf(other) != nullptr;
        if (isBrush(other)) {
            return actor::bCollideWorld(a) && (aPlayer ? actor::bBlockPlayers(other) : actor::bBlockActors(other));
        }
        if (isBrush(a)) {
            return actor::bCollideWorld(other) && (otherPlayer ? actor::bBlockPlayers(a) : actor::bBlockActors(a));
        }
        bool aProjectile = a->isA(engineClasses().projectile);
        bool otherProjectile = other->isA(engineClasses().projectile);
        bool first = (aPlayer || aProjectile) ? actor::bBlockPlayers(other) != 0 : actor::bBlockActors(other) != 0;
        bool second = (otherPlayer || otherProjectile) ? actor::bBlockPlayers(a) != 0 : actor::bBlockActors(a) != 0;
        return first && second;
    }

    bool isBasedOn(UObject* a, UObject* other)
    {
        int guard = 0;
        for (UObject* test = a; test && guard < 64; test = actor::Base(test), guard++) {
            if (test == other) {
                return true;
            }
        }
        return false;
    }

    bool isOverlapping(const World* world, UObject* a, UObject* other)
    {
        if (isBrush(a) || isBrush(other) || (world && other == world->levelInfo)) {
            return false;
        }
        Vector d = actor::Location(a) - actor::Location(other);
        float radius = actor::CollisionRadius(a) + actor::CollisionRadius(other);
        float height = actor::CollisionHeight(a) + actor::CollisionHeight(other);
        return d.x * d.x + d.y * d.y < radius * radius && d.z * d.z < height * height;
    }

    void setOwner(UObject* a, UObject* newOwner)
    {
        UObject* old = actor::Owner(a);
        if (old) {
            vm::Call(old, Name("LostChild")).obj(a).run();
        }
        actor::Owner(a) = newOwner;
        if (newOwner) {
            vm::Call(newOwner, Name("GainedChild")).obj(a).run();
        }
    }

    void setBase(UObject* a, UObject* newBase, bool notify)
    {
        int guard = 0;
        for (UObject* loop = newBase; loop && guard < 64; loop = actor::Base(loop), guard++) {
            if (loop == a) {
                return;
            }
        }
        UObject* base = actor::Base(a);
        if (newBase == base) {
            return;
        }
        UObject* level = World::hasActive() ? World::active().levelInfo : nullptr;
        if (base && base != level) {
            actor::StandingCount(base)--;
            vm::Call(base, Name("Detach")).obj(a).run();
        }
        actor::Base(a) = newBase;
        if (newBase && newBase != level) {
            actor::StandingCount(newBase)++;
            vm::Call(newBase, Name("Attach")).obj(a).run();
        }
        if (notify) {
            vm::event(a, Name("BaseChange"));
        }
    }

    namespace {
        bool touchTo(UObject* a, UObject* other)
        {
            int available = -1;
            for (int i = 0; i < 4; i++) {
                UObject* touching = actor::Touching(a, i);
                if (!touching) {
                    available = i;
                } else if (touching == other) {
                    return true;
                }
            }
            if (available == -1) {
                for (int i = 0; i < 4; i++) {
                    UObject* touching = actor::Touching(a, i);
                    if (touching && actor::Physics(touching) == PHYS_None) {
                        endTouch(a, touching, false);
                        available = i;
                    }
                }
            }
            if (available == -1 && isPawn(other)) {
                for (int i = 0; i < 4; i++) {
                    UObject* touching = actor::Touching(a, i);
                    if (touching && !isPawn(touching)) {
                        endTouch(a, touching, false);
                        available = i;
                        break;
                    }
                }
                if (available == -1 && pawn::bIsPlayer(other)) {
                    for (int i = 0; i < 4; i++) {
                        UObject* touching = actor::Touching(a, i);
                        if (touching && (!isPawn(touching) || !pawn::bIsPlayer(touching))) {
                            endTouch(a, touching, false);
                            available = i;
                            break;
                        }
                    }
                }
            }
            if (available >= 0) {
                actor::Touching(a, available) = other;
                vm::Call(a, Name("Touch")).obj(other).run();
                if (actor::Touching(a, available) != other) {
                    return false;
                }
            }
            return true;
        }
    }

    void beginTouch(UObject* a, UObject* other)
    {
        if (a == other || a->destroyed || other->destroyed) {
            return;
        }
        if (touchTo(a, other)) {
            touchTo(other, a);
        }
    }

    void endTouch(UObject* a, UObject* other, bool noNotifySelf)
    {
        for (int i = 0; i < 4; i++) {
            if (actor::Touching(a, i) == other) {
                actor::Touching(a, i) = nullptr;
                if (!noNotifySelf) {
                    vm::Call(a, Name("UnTouch")).obj(other).run();
                }
                break;
            }
        }
        for (int i = 0; i < 4; i++) {
            if (actor::Touching(other, i) == a) {
                actor::Touching(other, i) = nullptr;
                vm::Call(other, Name("UnTouch")).obj(a).run();
                break;
            }
        }
    }

    void setCollision(UObject* a, bool collideActors, bool blockActors, bool blockPlayers)
    {
        actor::bCollideActors(a) = collideActors ? 1 : 0;
        actor::bBlockActors(a) = blockActors ? 1 : 0;
        actor::bBlockPlayers(a) = blockPlayers ? 1 : 0;
        if (World::hasActive()) {
            World::active().collisionHash.update(a);
        }
    }

    void setPhysics(UObject* a, uint8_t newPhysics, UObject* newFloor)
    {
        if (actor::Physics(a) == newPhysics) {
            return;
        }
        actor::Physics(a) = newPhysics;
        if (newPhysics == PHYS_Walking || newPhysics == PHYS_None || newPhysics == PHYS_Rolling || newPhysics == PHYS_Rotating || newPhysics == PHYS_Spider) {
            if (newFloor) {
                if (actor::Base(a) != newFloor) {
                    setSupportBase(a, newFloor);
                }
            } else {
                findBase(a);
            }
        } else if (actor::Base(a)) {
            setBase(a, nullptr, true);
        }
        if (actor::Physics(a) == PHYS_None || actor::Physics(a) == PHYS_Rotating) {
            actor::Velocity(a) = {};
            actor::Acceleration(a) = {};
        }
    }

    void setSupportBase(UObject* a, UObject* newBase)
    {
        if (newBase) {
            vm::Call(newBase, Name("SupportActor")).obj(a).run();
        } else {
            setBase(a, nullptr, true);
        }
    }

    void findBase(UObject* a)
    {
        if (!World::hasActive()) {
            return;
        }
        CheckResult hit;
        Vector location = actor::Location(a);
        World::active().lineCheck(hit, location, location + Vector { 0, 0, -8 }, cylinderExtent(a), a, TRACE_AllColliding);
        if (actor::Base(a) != hit.actor && (!hit.actor || !isBasedOn(hit.actor, a))) {
            setSupportBase(a, hit.actor);
        }
    }
}
