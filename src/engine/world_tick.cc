#include "engine/world.h"

#include "core/profile.h"
#include "core/vm.h"
#include "engine/actor.h"
#include "engine/latent.h"
#include "engine/light_effects.h"
#include "engine/resources.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace dx {
    bool World::inStasis(UObject* a)
    {
        if (timeSeconds - actor::LastRenderTime(a) <= 5.0f || !actor::bStasis(a)) {
            return false;
        }
        uint8_t physics = actor::Physics(a);
        if (!actor::bForceStasis(a) && physics != PHYS_None && physics != PHYS_Rotating) {
            return false;
        }
        uint8_t zone = region(a).zoneNumber;
        float zoneRenderTime = level && level->model && zone < level->model->zones.size() ? level->model->zones[zone].lastRenderTime : 0.0f;
        return timeSeconds - zoneRenderTime > 5.0f || actor::DistanceFromPlayer(a) > 1200.0f;
    }

    bool World::tickActor(UObject* a, float deltaSeconds)
    {
        if (!a || a->destroyed) {
            return true;
        }
        UObject* owner = actor::Owner(a);
        if (owner && !owner->destroyed && actor::bTicked(owner) != tickParity) {
            newlySpawned.push_back(a);
            return false;
        }
        actor::bTicked(a) = tickParity;
        static const Name nameScriptedPawn("ScriptedPawn");
        if (a->isA(nameScriptedPawn) && engine::gameHooks().scriptedPawnTick) {
            DX_TIME("AI: ScriptedPawn tick");
            if (!engine::gameHooks().scriptedPawnTick(*this, a, deltaSeconds)) {
                return true;
            }
        }
        if (inStasis(a)) {
            if (actor::bTransient(a) && actor::Role(a) >= 2) {
                destroy(a);
            }
            return true;
        }

        {
            DX_TIME("Animation");
            updateAnimation(a, deltaSeconds);
        }
        if (a->destroyed) {
            return true;
        }

        bool isPlayer = playerPawnOf(a) != nullptr;
        if (!isPlayer) {
            if (levelinfo::bPlayersOnly(levelInfo)) {
                return true;
            }
            static const Name nameTick("Tick");
            auto onEffectClock = engine::gameHooks().onEffectClock;
            bool clocked = steadyTiming() && onEffectClock && onEffectClock(a);
            if (!clocked || effectStep > 0) {
                vm::Call tick(a, nameTick);
                if (tick.valid()) {
                    tick.f(clocked ? effectStep : deltaSeconds).run();
                }
            }
        } else {
            if (inputHook) {
                inputHook(a, deltaSeconds);
            }
            static const Name namePlayerInput("PlayerInput");
            vm::Call playerInput(a, namePlayerInput);
            if (playerInput.valid()) {
                playerInput.f(deltaSeconds).run();
            }
            static const Name namePlayerTick("PlayerTick");
            vm::Call playerTick(a, namePlayerTick);
            if (playerTick.valid()) {
                playerTick.f(deltaSeconds).run();
            }
            if (inputHook) {
                inputHook(a, -1.0f);
            }
        }
        if (a->destroyed) {
            return true;
        }

        {
            DX_TIME("Latent actions");
            latent::poll(a, deltaSeconds);
        }
        vm::runStateCode(a);
        if (a->destroyed) {
            return true;
        }

        float timerRate = actor::TimerRate(a);
        if (timerRate > 0) {
            float& counter = actor::TimerCounter(a);
            counter += deltaSeconds;
            if (counter >= timerRate) {
                int passed = static_cast<int>(counter / timerRate);
                counter -= timerRate * static_cast<float>(passed);
                if (!actor::bTimerLoop(a)) {
                    actor::TimerRate(a) = 0;
                }
                // The original fires once however many periods passed. Steady timing fires a
                // looping timer once per period (up to 64), stopping if the timer is changed.
                int calls = steadyTiming() && actor::bTimerLoop(a) ? std::min(passed, 64) : 1;
                static const Name nameTimer("Timer");
                for (int call = 0; call < calls; call++) {
                    vm::event(a, nameTimer);
                    if (a->destroyed) {
                        return true;
                    }
                    if (actor::TimerRate(a) != timerRate || !actor::bTimerLoop(a)) {
                        break;
                    }
                }
            }
        }

        float& lifeSpan = actor::LifeSpan(a);
        if (lifeSpan != 0) {
            lifeSpan -= deltaSeconds;
            if (lifeSpan <= 0.0001f) {
                static const Name nameExpired("Expired");
                vm::event(a, nameExpired);
                destroy(a);
                return true;
            }
        }

        if (actor::Physics(a) != PHYS_None) {
            DX_TIME("Physics");
            performPhysics(this, a, deltaSeconds);
        }
        if (a->destroyed) {
            return true;
        }

        // The end of AActor::Tick for pawns.
        if (isPawn(a)) {
            if (pawn::bIsPlayer(a) && actor::Role(a) > 2) {
                if (pawn::bViewTarget(a)) {
                    static const Name nameUpdateEyeHeight("UpdateEyeHeight");
                    vm::Call eyeHeight(a, nameUpdateEyeHeight);
                    if (eyeHeight.valid()) {
                        eyeHeight.f(deltaSeconds).run();
                    }
                } else {
                    pawn::ViewRotation(a) = actor::Rotation(a);
                }
                if (a->destroyed) {
                    return true;
                }
            }
            // The weapon goes wherever its holder is, so sounds and AI events it makes come from there.
            UObject* weapon = pawn::Weapon(a);
            if (weapon && !weapon->destroyed) {
                farMoveActor(weapon, actor::Location(a), false, false);
            }
            {
                DX_TIME("AI: pawn senses");
                tickPawnSenses(a, deltaSeconds);
            }
            float& painTime = pawn::PainTime(a);
            if (painTime > 0) {
                painTime -= deltaSeconds;
                if (painTime < 0.001f) {
                    painTime = 0;
                    static const Name namePainTimer("PainTimer");
                    vm::event(a, namePainTimer);
                }
            }
            float& speechTime = pawn::SpeechTime(a);
            if (speechTime > 0) {
                speechTime -= deltaSeconds;
                if (speechTime < 0.001f) {
                    speechTime = 0;
                    static const Name nameSpeechTimer("SpeechTimer");
                    vm::event(a, nameSpeechTimer);
                }
            }
            if (!a->destroyed && pawn::bAdvancedTactics(a)) {
                static const Name nameUpdateTactics("UpdateTactics");
                vm::Call tactics(a, nameUpdateTactics);
                if (tactics.valid()) {
                    tactics.f(deltaSeconds).run();
                }
            }
        }
        return true;
    }

    void World::tick(float deltaSeconds)
    {
        if (!levelInfo) {
            return;
        }
        vm::resetRunaway();
        float dilation = levelinfo::TimeDilation(levelInfo);
        if (dilation <= 0) {
            dilation = 1;
        }
        float delta = deltaSeconds * dilation;
        timeSeconds += delta;
        levelinfo::TimeSeconds(levelInfo) = timeSeconds;
        std::time_t now = std::time(nullptr);
        std::tm local = *std::localtime(&now);
        levelinfo::Year(levelInfo) = local.tm_year + 1900;
        levelinfo::Month(levelInfo) = local.tm_mon + 1;
        levelinfo::DayOfWeek(levelInfo) = local.tm_wday;
        levelinfo::Day(levelInfo) = local.tm_mday;
        levelinfo::Hour(levelInfo) = local.tm_hour;
        levelinfo::Minute(levelInfo) = local.tm_min;
        levelinfo::Second(levelInfo) = local.tm_sec;
        if (steadyTiming()) {
            // ULevel::Tick raises a tick under 5 ms to 5 ms, so above 200 fps the game runs
            // faster than real time. Steady timing gathers short frames until 5 ms have
            // passed instead; input keeps adding up until the tick that reads it.
            delta += shortFrameTime;
            if (delta < 0.005f) {
                shortFrameTime = delta;
                return;
            }
            shortFrameTime = 0;
        }
        delta = std::clamp(delta, 0.005f, 0.4f);
        frame++;

        paused = !levelinfo::Pauser(levelInfo).empty();
        {
            DX_ZONE_NAMED("Collision hash sync");
            collisionHash.sync(actors);
        }
        if (localPlayer && !localPlayer->destroyed) {
            Vector viewLocation = actor::Location(localPlayer);
            for (UObject* a : actors) {
                if (a && !a->destroyed && !actor::bStatic(a)) {
                    actor::DistanceFromPlayer(a) = (actor::Location(a) - viewLocation).size();
                }
            }
        }
        if (!paused) {
            // Whole periods of the effect clock, the rest carried to the next tick.
            effectTime += delta;
            const float effectPeriod = static_cast<float>(1.0 / SteadyEffectRate);
            float periods = std::floor(effectTime / effectPeriod);
            effectStep = periods * effectPeriod;
            effectTime -= effectStep;
            if (effectStep > 0) {
                effectSteps++;
            }
            inTick = true;
            newlySpawned.clear();
            {
                DX_ZONE_NAMED("Actor ticks");
                int updated = 0;
                for (size_t i = firstDynamicActor; i < actors.size(); i++) {
                    UObject* a = actors[i];
                    if (!a || a->destroyed) {
                        continue;
                    }
                    updated += tickActor(a, delta) ? 1 : 0;
                }
                while (!newlySpawned.empty() && updated) {
                    std::vector<UObject*> pending;
                    pending.swap(newlySpawned);
                    updated = 0;
                    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                        UObject* a = *it;
                        if (!a->destroyed && actor::bTicked(a) != tickParity) {
                            updated += tickActor(a, delta) ? 1 : 0;
                        }
                    }
                }
            }
            inTick = false;
            if (!levelinfo::bPlayersOnly(levelInfo)) {
                DX_ZONE_NAMED("AI events");
                aiEvents();
            }
        } else {
            DX_ZONE_NAMED("Paused actor ticks");
            for (size_t i = firstDynamicActor; i < actors.size(); i++) {
                UObject* a = actors[i];
                if (!a || a->destroyed) {
                    continue;
                }
                if (playerPawnOf(a)) {
                    if (inputHook) {
                        inputHook(a, delta);
                    }
                    static const Name namePlayerInput("PlayerInput");
                    vm::Call playerInput(a, namePlayerInput);
                    if (playerInput.valid()) {
                        playerInput.f(delta).run();
                    }
                    for (UProperty* property : a->cls->properties) {
                        if ((property->propertyFlags & CPF_Input) && property->type == PropType::Float) {
                            *reinterpret_cast<float*>(a->data + property->offset) = 0;
                        }
                    }
                } else if (actor::bAlwaysTick(a)) {
                    tickActor(a, delta);
                }
            }
            newlySpawned.clear();
        }
        tickParity = tickParity ? 0 : 1;
        DX_ZONE_NAMED("Destroyed actor cleanup");
        cleanupDestroyed();
        releaseDestroyed();
    }
}
