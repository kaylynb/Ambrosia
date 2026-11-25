#include "core/math.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/console.h"
#include "engine/gamepad.h"
#include "engine/gyro.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

#include <cmath>

namespace {
    constexpr float unitsPerRadian = 65536.0f / (2.0f * dx::Pi);
}

TEST_CASE("gyro turns slower than the drift threshold fade out and faster ones pass unchanged", "[gyro]")
{
    dx::gyro::Turn still = dx::gyro::shape({ 0, 0 }, 0.04f);
    CHECK(still.pitch == 0.0f);
    CHECK(still.yaw == 0.0f);
    dx::gyro::Turn slow = dx::gyro::shape({ 0, 0.01f }, 0.04f);
    CHECK(slow.yaw == Catch::Approx(0.0025f));
    dx::gyro::Turn edge = dx::gyro::shape({ 0.03f, 0.04f }, 0.05f);
    CHECK(edge.pitch == Catch::Approx(0.03f));
    CHECK(edge.yaw == Catch::Approx(0.04f));
    dx::gyro::Turn fast = dx::gyro::shape({ -1.0f, 2.0f }, 0.04f);
    CHECK(fast.pitch == -1.0f);
    CHECK(fast.yaw == 2.0f);
}

TEST_CASE("the gyro's samples all come out of the pacer, a frame's time each frame", "[gyro]")
{
    dx::gyro::Pacer pacer;
    for (int i = 0; i < 100; i++) {
        pacer.add({ 0.5f, -1.0f }, 0.01f, 0.04f);
    }
    // A second of samples at once, taken a frame at a time: it catches up to the most it lets
    // queue, then pays out the rest over the next frames.
    dx::gyro::Turn total;
    for (int frame = 0; frame < 120; frame++) {
        dx::gyro::Turn turned = pacer.take(1.0f / 60.0f);
        total.pitch += turned.pitch;
        total.yaw += turned.yaw;
    }
    CHECK(total.pitch == Catch::Approx(0.5f));
    CHECK(total.yaw == Catch::Approx(-1.0f));
    CHECK(pacer.queued() == 0.0f);
    // A sample with no time to it turns nothing.
    pacer.add({ 3.0f, 3.0f }, 0.0f, 0.04f);
    CHECK(pacer.take(0.01f).yaw == 0.0f);
}

TEST_CASE("a steady turn sent in bursts comes out in even steps at any frame rate", "[gyro]")
{
    struct Pad {
        const char* name;
        float burst;
        int samples;
    };

    // A Switch Pro Controller sends three 5 ms samples every 15 ms; a DualSense over USB one
    // sample every 4 ms; over Bluetooth, about every 8 ms.
    for (Pad pad : { Pad { "Switch Pro", 0.015f, 3 }, Pad { "DualSense USB", 0.004f, 1 }, Pad { "Bluetooth", 0.008f, 1 } }) {
        for (float fps : { 60.0f, 144.0f, 240.0f }) {
            CAPTURE(pad.name, fps);
            dx::gyro::Pacer pacer;
            float frame = 1.0f / fps;
            float clock = 0;
            float sent = 0;
            float total = 0;
            float smallest = 1e9f;
            float largest = 0;
            int frames = static_cast<int>(fps) * 10;
            for (int i = 0; i < frames; i++) {
                clock += frame;
                while (sent + pad.burst <= clock) {
                    for (int sample = 0; sample < pad.samples; sample++) {
                        pacer.add({ 0, 1.0f }, pad.burst / static_cast<float>(pad.samples), 0.04f);
                    }
                    sent += pad.burst;
                }
                float turned = pacer.take(frame).yaw;
                total += turned;
                // After the first two seconds, once it has learned how much to keep back.
                if (i >= static_cast<int>(fps) * 2) {
                    smallest = std::min(smallest, turned);
                    largest = std::max(largest, turned);
                }
            }
            // Every frame turns about a frame's worth: without the pacer, at 144 fps the Switch
            // Pro Controller's frames would alternate between none and two frames' worth.
            CHECK(smallest > frame * 0.85f);
            CHECK(largest < frame * 1.15f);
            // Behind by no more than a burst and a frame.
            CHECK(sent - total < pad.burst + frame + 0.002f);
            CHECK(pacer.margin() <= dx::gyro::Pacer::largestMargin);
        }
    }
}

TEST_CASE("at sensitivity 1 the view turns as far as the controller, at any frame rate", "[gyro]")
{
    // A tenth of a radian to the left and up.
    dx::gyro::Look look = dx::gyro::lookAxes({ 0.1f, 0.1f }, 1.0f);
    dx::gyro::Turn view = dx::gyro::viewTurn(look, 75.0f);
    // The game's yaw grows to the right.
    CHECK(view.yaw == Catch::Approx(-0.1f * unitsPerRadian));
    CHECK(view.pitch == Catch::Approx(0.1f * unitsPerRadian));
    // Twice the sensitivity turns twice as far; zooming in, a smaller field of view, less.
    CHECK(dx::gyro::viewTurn(dx::gyro::lookAxes({ 0, 0.1f }, 2.0f), 75.0f).yaw == Catch::Approx(-0.2f * unitsPerRadian));
    CHECK(std::abs(dx::gyro::viewTurn(dx::gyro::lookAxes({ 0, 0.1f }, 1.0f), 30.0f).yaw) < 0.1f * unitsPerRadian * 0.5f);
    // The same turn split over 30 or 144 frames a second ends in the same place.
    for (float fps : { 30.0f, 144.0f }) {
        float total = 0;
        for (int frame = 0; frame < static_cast<int>(fps); frame++) {
            dx::gyro::Pacer pacer;
            pacer.add({ 0, 1.0f }, 1.0f / fps, 0.04f);
            total += dx::gyro::viewTurn(dx::gyro::lookAxes(pacer.take(1.0f / fps), 1.0f), 75.0f).yaw;
        }
        CHECK(total == Catch::Approx(-unitsPerRadian).epsilon(0.001));
    }
}

namespace {
    // Down in the axes of a controller rolled `degrees` onto its left side, or with its far edge
    // tipped up by `degrees`.
    dx::Vector rolled(float degrees)
    {
        float r = degrees * dx::Pi / 180.0f;
        return { -std::sin(r), -std::cos(r), 0 };
    }

    dx::Vector tipped(float degrees)
    {
        float r = degrees * dx::Pi / 180.0f;
        return { 0, -std::cos(r), std::sin(r) };
    }
}

TEST_CASE("held level, both gyro spaces turn the view as the controller turns", "[gyro]")
{
    dx::gyro::Turn rate { 0.3f, -0.7f, 0 };
    for (dx::gyro::Space space : { dx::gyro::Space::Local, dx::gyro::Space::Player }) {
        dx::gyro::Turn view = dx::gyro::view(rate, { 0, -1, 0 }, space);
        CHECK(view.pitch == Catch::Approx(0.3f));
        CHECK(view.yaw == Catch::Approx(-0.7f));
    }
    // Rolling as well, player space takes the yaw and roll axes' whole turn, up to the relaxed
    // part about gravity; local space leaves roll out.
    dx::gyro::Turn rolling { 0, -0.7f, 0.5f };
    CHECK(dx::gyro::view(rolling, { 0, -1, 0 }, dx::gyro::Space::Player).yaw == Catch::Approx(-std::hypot(0.7f, 0.5f)));
    CHECK(dx::gyro::view(rolling, { 0, -1, 0 }, dx::gyro::Space::Local).yaw == Catch::Approx(-0.7f));
}

TEST_CASE("a controller held upright turns the view by its roll axis in player space", "[gyro]")
{
    // Tipped up 90 degrees, facing the player, the controller turns left about its z axis,
    // which now points up: counter-clockwise about z seen from the player is a negative roll.
    dx::Vector down = tipped(90.0f);
    dx::gyro::Turn turnLeft { 0, 0, -1.0f };
    CHECK(dx::gyro::view(turnLeft, down, dx::gyro::Space::Local).yaw == Catch::Approx(0.0f));
    CHECK(dx::gyro::view(turnLeft, down, dx::gyro::Space::Player).yaw == Catch::Approx(1.0f));
}

TEST_CASE("player space keeps a tilted controller's whole yaw", "[gyro]")
{
    // Rolled 30 degrees and turned about its own y axis.
    dx::gyro::Turn rate { 0, 1.0f, 0 };
    CHECK(dx::gyro::view(rate, rolled(30.0f), dx::gyro::Space::Player).yaw == Catch::Approx(1.0f));
    // Rolled past where the yaw axis turns the other way, player space turns the view hardly at
    // all rather than backwards.
    float flipped = dx::gyro::view(rate, rolled(91.0f), dx::gyro::Space::Player).yaw;
    CHECK(std::abs(flipped) < 0.05f);
}

TEST_CASE("gravity follows the gyro's turn and settles on the accelerometer", "[gyro]")
{
    dx::gyro::Gravity gravity;
    // Level at rest: the accelerometer points up.
    gravity.accelerometer({ 0, 9.8f, 0 });
    CHECK(gravity.down().y == Catch::Approx(-1.0f));
    // Tip the far edge up 90 degrees over a second; the accelerometer is not read meanwhile.
    dx::gyro::Gravity turned;
    for (int i = 0; i < 1000; i++) {
        turned.turn({ dx::Pi / 2.0f, 0, 0 }, 0.001f);
    }
    CHECK(turned.down().z == Catch::Approx(1.0f).margin(0.01));
    // A reading the other way pulls down to it within a second.
    gravity.accelerometer({ 0, 0, -9.8f });
    for (int i = 0; i < 100; i++) {
        gravity.turn({}, 0.01f);
    }
    CHECK(gravity.down().z == Catch::Approx(1.0f).margin(0.01));
}

TEST_CASE("calibration measures the drift of a still controller", "[gyro]")
{
    using State = dx::gyro::Calibration::State;
    dx::gyro::Calibration calibration;
    CHECK(calibration.state() == State::Idle);
    calibration.start();
    dx::gyro::Turn drift { 0.01f, -0.02f, 0.005f };
    for (int i = 0; i < 150; i++) {
        calibration.add(drift, 0.01f);
        calibration.tick(0.01f);
    }
    CHECK(calibration.state() == State::Measuring);
    // A move part way starts the count again.
    calibration.add({ 0, 1.0f, 0 }, 0.01f);
    for (int i = 0; i < 150; i++) {
        calibration.add(drift, 0.01f);
    }
    CHECK(calibration.state() == State::Measuring);
    for (int i = 0; i < 60; i++) {
        calibration.add(drift, 0.01f);
    }
    REQUIRE(calibration.state() == State::Done);
    CHECK(calibration.drift().pitch == Catch::Approx(0.01f));
    CHECK(calibration.drift().yaw == Catch::Approx(-0.02f));
    CHECK(calibration.drift().roll == Catch::Approx(0.005f));
    dx::gyro::Turn fixed = dx::gyro::corrected({ 0.51f, -0.02f, 0 }, calibration.drift());
    CHECK(fixed.pitch == Catch::Approx(0.5f));
    CHECK(fixed.yaw == Catch::Approx(0.0f).margin(1e-6));
    // The result shows for a while, then the calibration is idle again.
    calibration.tick(dx::gyro::Calibration::resultSeconds + 0.1f);
    CHECK(calibration.state() == State::Idle);
}

TEST_CASE("calibration takes a large drift with noise on it, and starts again when the controller tilts", "[gyro]")
{
    using State = dx::gyro::Calibration::State;
    // A badly drifting controller lying still: 0.3 rad/s (17 degrees a second) of yaw, with
    // noise on every axis.
    dx::gyro::Calibration drifting;
    drifting.start();
    for (int i = 0; i < 210; i++) {
        float noise = (i % 2 == 0 ? 0.02f : -0.02f);
        drifting.add({ noise, 0.3f + noise, -noise }, 0.01f);
        drifting.accelerometer({ 0, 9.8f, 0 });
    }
    REQUIRE(drifting.state() == State::Done);
    CHECK(drifting.drift().yaw == Catch::Approx(0.3f).margin(0.002));
    CHECK(drifting.drift().pitch == Catch::Approx(0.0f).margin(0.002));
    // Tilted part way, as it is set down, the count starts again.
    dx::gyro::Calibration tilted;
    tilted.start();
    for (int i = 0; i < 150; i++) {
        tilted.add({}, 0.01f);
        tilted.accelerometer({ 0, 9.8f, 0 });
    }
    tilted.accelerometer({ 2.0f, 9.6f, 0 });
    for (int i = 0; i < 100; i++) {
        tilted.add({}, 0.01f);
        tilted.accelerometer({ 2.0f, 9.6f, 0 });
    }
    CHECK(tilted.state() == State::Measuring);
    CHECK(tilted.restarts() == 1);
}

TEST_CASE("calibration gives up on a controller that keeps moving or sends no readings", "[gyro]")
{
    using State = dx::gyro::Calibration::State;
    // Held in a hand, the readings wander.
    dx::gyro::Calibration moving;
    moving.start();
    for (int i = 0; i < 1100; i++) {
        moving.add({ 0, 0.3f * std::sin(static_cast<float>(i) * 0.05f), 0 }, 0.01f);
    }
    CHECK(moving.state() == State::Failed);
    CHECK(moving.restarts() > 0);
    // Turning steadily faster than any drift.
    dx::gyro::Calibration spinning;
    spinning.start();
    for (int i = 0; i < 1100; i++) {
        spinning.add({ 0, 1.0f, 0 }, 0.01f);
    }
    CHECK(spinning.state() == State::Failed);
    dx::gyro::Calibration silent;
    silent.start();
    silent.tick(dx::gyro::Calibration::giveUpSeconds + 0.1f);
    CHECK(silent.state() == State::NoGyro);
}

TEST_CASE("the look axes for a gyro turn turn the player's view by that much", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UProperty* fovProperty = player->cls->findProperty(dx::Name("DesiredFOV"));
        REQUIRE(fovProperty != nullptr);
        float fov = *reinterpret_cast<float*>(player->data + fovProperty->offset);
        dx::gyro::Look look = dx::gyro::lookAxes({ 0, 0.2f }, 1.0f);
        // The same turn however long the tick that reads it: one frame's, or several frames'
        // turns gathered into one longer tick.
        for (float seconds : { 1.0f / 250.0f, 1.0f / 60.0f, 1.0f / 20.0f }) {
            dx::console::resetInput(player);
            dx::console::heldAxis("aTurn", look.turn * 0.5f);
            dx::console::heldAxis("aTurn", look.turn * 0.5f);
            dx::console::readInput(player, seconds);
            dx::vm::Call(player, dx::Name("PlayerInput")).f(seconds).run();
            int32_t before = dx::pawn::ViewRotation(player).yaw;
            // PlayerMove, which scales the axes before UpdateRotation.
            dx::vm::Call(player, dx::Name("PlayerMove")).f(seconds).run();
            int32_t turned = dx::pawn::ViewRotation(player).yaw - before;
            CHECK(static_cast<float>(turned) == Catch::Approx(dx::gyro::viewTurn(look, fov).yaw).margin(2.0f));
            // To the left, by the 0.2 radians at the default field of view.
            CHECK(turned < 0);
        }
        dx::console::resetInput(player);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}

TEST_CASE("a flick turns the player's view in play by the stick's angle", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UProperty* fovProperty = player->cls->findProperty(dx::Name("DesiredFOV"));
        REQUIRE(fovProperty != nullptr);
        // As the game reads the input, each tick (game_travel.cc).
        world.inputHook = [](dx::UObject* pawn, float delta) {
            dx::console::readInput(pawn, delta);
        };
        for (float seconds : { 1.0f / 144.0f, 1.0f / 60.0f, 1.0f / 30.0f }) {
            for (int i = 0; i < 10; i++) {
                world.tick(seconds);
            }
            dx::console::resetInput(player);
            int32_t start = dx::pawn::ViewRotation(player).yaw;
            dx::gamepad::FlickStick flick;
            // As GamepadInput does: a frame's flick given on the look axis, then the world's tick.
            auto frame = [&](float x, float y) {
                float fov = *reinterpret_cast<float*>(player->data + fovProperty->offset);
                float turn = flick.update(x, y, seconds);
                dx::console::heldAxis("aTurn", dx::gyro::exactLookAxes({ 0, -turn }, fov).turn);
                world.tick(seconds);
            };
            // Right, held for a third of a second: a quarter turn right.
            for (int i = 0; static_cast<float>(i) * seconds < 0.33f; i++) {
                frame(1.0f, 0.0f);
            }
            int32_t flicked = static_cast<int16_t>(static_cast<uint16_t>((dx::pawn::ViewRotation(player).yaw - start) & 65535));
            INFO("frame " << seconds << " s, flick turned " << flicked);
            CHECK(flicked == Catch::Approx(16384).margin(64));
            // Then round the rim to straight down, over a third of a second: another quarter.
            int steps = static_cast<int>(0.33f / seconds);
            for (int i = 1; i <= steps; i++) {
                float angle = 1.5707963f + 1.5707963f * static_cast<float>(i) / static_cast<float>(steps);
                frame(std::sin(angle), -std::cos(angle));
            }
            frame(0.0f, 0.0f);
            int32_t total = static_cast<int16_t>(static_cast<uint16_t>((dx::pawn::ViewRotation(player).yaw - start) & 65535));
            INFO("and with the rim " << total);
            CHECK(std::abs(total) == Catch::Approx(32768).margin(128));
        }
        dx::console::resetInput(player);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
}
