#include "engine/gyro.h"

#include "core/math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dx::gyro {
    namespace {
        // Rotation units a radian.
        constexpr float unitsPerRadian = 65536.0f / (2.0f * Pi);
        // How the game turns an amount on a look axis: the input layer adds it and scales the
        // axis by 20 / seconds (console::readInput), PlayerInput scales it by DesiredFOV *
        // 0.01111, every movement state's PlayerMove by 0.24 (PlayerPawn's PlayerWalking,
        // PlayerSwimming, PlayerFlying and the rest), and UpdateRotation adds 32 * seconds of
        // it. The tick's seconds cancel.
        constexpr float axisScale = 20.0f * 32.0f * 0.24f;
        constexpr float defaultFov = 75.0f;

        float fovScale(float fov)
        {
            return fov * 0.01111f;
        }
    }

    namespace {
        // How long the accelerometer takes to pull down most of the way (1 - 1/e) to its
        // reading. Longer keeps the hand's shakes out; shorter catches gyro error sooner.
        constexpr float gravitySettleSeconds = 0.2f;

        Vector axes(const Turn& rate)
        {
            return { rate.pitch, rate.yaw, rate.roll };
        }
    }

    void Gravity::accelerometer(Vector reading)
    {
        Vector down = (-reading).normal();
        if (down.sizeSquared() == 0) {
            return;
        }
        measured = down;
        if (!settled) {
            gravity = down;
            settled = true;
        }
    }

    void Gravity::turn(Turn rate, float seconds)
    {
        if (seconds <= 0) {
            return;
        }
        // A direction fixed in the world turns the other way in the controller's axes.
        gravity -= axes(rate).cross(gravity) * seconds;
        if (measured.sizeSquared() > 0) {
            float pull = 1.0f - std::exp(-seconds / gravitySettleSeconds);
            gravity += (measured - gravity) * pull;
        }
        Vector unit = gravity.normal();
        gravity = unit.sizeSquared() > 0 ? unit : Vector { 0, -1, 0 };
    }

    Turn view(Turn rate, Vector down, Space space)
    {
        switch (space) {
        case Space::Local:
            return { rate.pitch, rate.yaw };
        case Space::Player: {
            // The yaw and roll axes' turn about gravity, kept up to their whole turn, so a
            // tilted controller still turns as far; near the tilt where they would turn the
            // view the wrong way it falls to nothing rather than flipping.
            float aboutDown = rate.yaw * down.y + rate.roll * down.z;
            float most = std::hypot(rate.yaw, rate.roll);
            float yaw = -std::copysign(std::min(std::abs(aboutDown) * playerYawRelax, most), aboutDown);
            return { rate.pitch, yaw };
        }
        }
        return { rate.pitch, rate.yaw };
    }

    void Calibration::start()
    {
        *this = {};
        current = State::Measuring;
    }

    void Calibration::add(Turn rate, float seconds)
    {
        if (current != State::Measuring || seconds <= 0) {
            return;
        }
        readings = true;
        elapsed += seconds;
        // A few readings first, so the average they are held against means something. The
        // reading that strays is left out of the next run.
        bool moved = false;
        if (samples >= 8) {
            Vector average = axes(sum) * (1.0f / stillFor);
            float stray = (axes(rate) - average).size();
            if (stray > stillSpread || average.size() > largestDrift) {
                strayest = std::max(strayest, stray);
                restart();
                moved = true;
            }
        }
        if (!moved) {
            sum.pitch += rate.pitch * seconds;
            sum.yaw += rate.yaw * seconds;
            sum.roll += rate.roll * seconds;
            stillFor += seconds;
            samples++;
        }
        if (stillFor >= stillSeconds) {
            measured = { sum.pitch / stillFor, sum.yaw / stillFor, sum.roll / stillFor };
            current = State::Done;
            clock = 0;
        } else if (elapsed >= giveUpSeconds) {
            current = State::Failed;
            clock = 0;
        }
    }

    void Calibration::accelerometer(Vector reading)
    {
        if (current != State::Measuring) {
            return;
        }
        Vector down = (-reading).normal();
        if (down.sizeSquared() == 0) {
            return;
        }
        if (startDown.sizeSquared() == 0) {
            startDown = down;
        } else if ((down - startDown).size() > moveTilt) {
            restart();
            startDown = down;
        }
    }

    // Starts the still run again, from the next reading.
    void Calibration::restart()
    {
        sum = {};
        samples = 0;
        stillFor = 0;
        startDown = {};
        restartCount++;
    }

    void Calibration::tick(float seconds)
    {
        if (current == State::Idle) {
            return;
        }
        clock += std::max(seconds, 0.0f);
        if (current == State::Measuring) {
            if (!readings && clock > giveUpSeconds) {
                current = State::NoGyro;
                clock = 0;
            }
        } else if (clock > resultSeconds) {
            current = State::Idle;
        }
    }

    Turn corrected(Turn rate, Turn drift)
    {
        return { rate.pitch - drift.pitch, rate.yaw - drift.yaw, rate.roll - drift.roll };
    }

    Turn shape(Turn rate, float threshold)
    {
        float speed = std::hypot(rate.pitch, rate.yaw);
        if (threshold <= 0 || speed >= threshold) {
            return rate;
        }
        float keep = speed / threshold;
        return { rate.pitch * keep, rate.yaw * keep };
    }

    void Pacer::add(Turn rate, float seconds, float threshold)
    {
        if (seconds <= 0) {
            return;
        }
        pieces.push_back({ shape(rate, threshold), seconds });
        queuedSeconds += seconds;
        sinceSample = 0;
    }

    Turn Pacer::take(float seconds)
    {
        if (seconds <= 0) {
            return {};
        }
        // Learn the margin while samples are coming; with none for a tenth of a second the gyro
        // is not sending, and running dry says nothing.
        bool sending = sinceSample < 0.1f;
        sinceSample += seconds;
        float spare = queuedSeconds - seconds;
        if (sending) {
            // Short of a frame's worth: keep back that much more from now on, and a little.
            if (spare < 0) {
                keptBack = std::min(keptBack - spare + headroom, largestMargin);
            }
            // A whole second well above that: keep back less, half the extra at a time.
            lowest = std::min(lowest, spare);
            windowSeconds += seconds;
            if (windowSeconds >= 1.0f) {
                if (lowest > headroom) {
                    keptBack = std::max(keptBack - (lowest - headroom) * 0.5f, 0.0f);
                }
                lowest = 1.0f;
                windowSeconds = 0;
            }
        }
        // A frame's worth, up to a tenth faster or slower to bring what is usually left over to
        // the margin. The level is averaged over a quarter second, so the rise and fall from
        // burst to burst does not make the steps uneven.
        averageSpare += (spare - averageSpare) * (1.0f - std::exp(-seconds / 0.25f));
        float want = seconds * (1.0f + std::clamp((averageSpare - keptBack) / 0.1f, -0.1f, 0.1f));
        if (queuedSeconds - want > largestQueue) {
            want = queuedSeconds - largestQueue;
        }
        want = std::min(want, queuedSeconds);
        Turn turned;
        size_t used = 0;
        float left = want;
        while (used < pieces.size() && left > 0) {
            Piece& piece = pieces[used];
            float part = std::min(piece.seconds, left);
            turned.pitch += piece.rate.pitch * part;
            turned.yaw += piece.rate.yaw * part;
            piece.seconds -= part;
            left -= part;
            if (piece.seconds <= 0) {
                used++;
            }
        }
        pieces.erase(pieces.begin(), pieces.begin() + static_cast<std::ptrdiff_t>(used));
        queuedSeconds = std::max(queuedSeconds - want, 0.0f);
        if (pieces.empty()) {
            queuedSeconds = 0;
        }
        return turned;
    }

    void Pacer::clear()
    {
        *this = {};
    }

    Look lookAxes(Turn angle, float sensitivity)
    {
        float scale = sensitivity * unitsPerRadian / (axisScale * fovScale(defaultFov));
        // The game's yaw grows turning right and its look axis looks up.
        return { -angle.yaw * scale, angle.pitch * scale };
    }

    Look exactLookAxes(Turn angle, float fov)
    {
        return lookAxes(angle, fov > 0 ? defaultFov / fov : 1.0f);
    }

    Turn viewTurn(Look axes, float fov)
    {
        float scale = axisScale * fovScale(fov);
        return { axes.lookUp * scale, axes.turn * scale };
    }
}
