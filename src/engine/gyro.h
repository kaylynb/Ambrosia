#pragma once

#include "core/math.h"

#include <vector>

namespace dx::gyro {
    // A controller's turn, in SDL's gamepad sensor convention: about its own axes, x to the
    // right, y up and z toward the player, each counter-clockwise positive. So pitch is positive
    // as its far edge tips up, yaw as it turns left, roll as it tips onto its left side. As a
    // rate in radians a second, or an angle in radians. Once put in a space (see view) only
    // pitch and yaw count, as the view's.
    struct Turn {
        float pitch = 0;
        float yaw = 0;
        float roll = 0;
    };

    // How the controller's turn becomes the view's (gyrowiki.jibbsmart.com, "Player Space Gyro
    // and Alternatives Explained"). The values are the GamepadGyroSpace setting's.
    enum class Space {
        // The controller's own yaw and pitch axes: the most exact, and best for a handheld or a
        // controller held level, but turning it about the wrong axis held tilted turns the view
        // the wrong way.
        Local,
        // Yaw is the turn about gravity, from the controller's yaw and roll axes, so a tilted
        // controller still turns left and right as the player expects; pitch stays local.
        Player,
    };

    // Player space leaves this much of the yaw and roll axes' turn about gravity: 1.41 lets the
    // controller tilt about 45 degrees before its yaw falls off (the article's value; 1.15 to 2
    // suit most players).
    inline constexpr float playerYawRelax = 1.41f;

    // Which way is down in the controller's axes, from its accelerometer and gyro: the gyro's
    // turn keeps it up to date and the accelerometer, which also feels the hand's shakes,
    // corrects it slowly.
    class Gravity {
    public:
        // An accelerometer sample, in any units, in the controller's axes: at rest it points up.
        void accelerometer(Vector reading);
        // Turns down the other way by the controller's turn over `seconds`.
        void turn(Turn rate, float seconds);

        // A unit vector; straight down (0, -1, 0), held level, before any accelerometer sample.
        Vector down() const
        {
            return gravity;
        }

    private:
        Vector gravity { 0, -1, 0 };
        // The accelerometer's latest down, unit length, or zero before the first sample.
        Vector measured;
        bool settled = false;
    };

    // The view's turn, as pitch and yaw, for the controller's, with down from Gravity.
    Turn view(Turn rate, Vector down, Space space);

    // Measures what the gyro reads while the controller lies still, its drift, to take off
    // every reading after. The controller has to stay still for stillSeconds in a row; a move
    // starts the count again, and it gives up after giveUpSeconds.
    class Calibration {
    public:
        enum class State {
            Idle,
            Measuring,
            Done,
            // Never still for long enough.
            Failed,
            // No readings came: the controller has no gyro, or none SDL can read.
            NoGyro,
        };

        static constexpr float stillSeconds = 2.0f;
        static constexpr float giveUpSeconds = 10.0f;
        // Lying still, the gyro reads its drift, however large, plus a little noise; a move
        // shows as readings that stray from the run's average by more than this (radians a
        // second, about 4.5 degrees), or as the accelerometer's direction changing by more than
        // moveTilt (about 3 degrees).
        static constexpr float stillSpread = 0.08f;
        static constexpr float moveTilt = 0.05f;
        // A run's average over this (about 30 degrees a second) is a turn, not drift: the
        // controller may lie still on something turning.
        static constexpr float largestDrift = 0.5f;

        void start();
        // A raw reading, before any drift is taken off.
        void add(Turn rate, float seconds);
        // An accelerometer reading, in any units.
        void accelerometer(Vector reading);
        // Real time passing, called each frame: measuring with no readings for longer than
        // it gives up after is NoGyro, and a result is shown for resultSeconds before it is Idle
        // again (the drift measured stays with the caller).
        void tick(float seconds);
        static constexpr float resultSeconds = 3.0f;

        State state() const
        {
            return current;
        }

        // The drift measured, once Done.
        Turn drift() const
        {
            return measured;
        }

        // For the log: how often a run started again, and the largest stray reading, in
        // radians a second, that ended one.
        int restarts() const
        {
            return restartCount;
        }

        float largestStray() const
        {
            return strayest;
        }

    private:
        void restart();

        State current = State::Idle;
        Turn sum;
        int samples = 0;
        float stillFor = 0;
        float elapsed = 0;
        // Down by the accelerometer as the run began, or zero before a reading.
        Vector startDown;
        int restartCount = 0;
        float strayest = 0;
        // Real time since start, or since the result.
        float clock = 0;
        bool readings = false;
        Turn measured;
    };

    // A reading with the drift taken off.
    Turn corrected(Turn rate, Turn drift);

    // Slow turns below `threshold` (radians a second) fade out toward zero, so sensor drift and
    // the hand's tremor do not creep the view, while a deliberate turn passes unchanged.
    Turn shape(Turn rate, float threshold);

    // Pays the controller's turn out evenly, a frame's worth of time each frame, as a jitter
    // buffer does audio. A controller sends its samples in bursts at its own rate (a Switch Pro
    // Controller about 66 a second, three samples each), so adding up whatever arrived before a
    // frame turns the view in uneven steps, and not at all in a frame no burst came before. The
    // samples are queued instead, each a rate held for its time, and each frame takes its own
    // length of them. It keeps back as little as the bursts need for no frame to run dry,
    // learned from the frames that did, and plays a little faster or slower to hold that. All of
    // the turn comes out; only when.
    class Pacer {
    public:
        // A sample: a rate held for `seconds`, its slow part faded out (shape).
        void add(Turn rate, float seconds, float threshold);
        // The turn over a frame of `seconds`.
        Turn take(float seconds);
        // Drops what is queued, and what was learned.
        void clear();

        // The sample time queued, and kept back past a frame's worth.
        float queued() const
        {
            return queuedSeconds;
        }

        float margin() const
        {
            return keptBack;
        }

        // The most it keeps back, and the most it lets queue before catching up at once.
        static constexpr float largestMargin = 0.04f;
        static constexpr float largestQueue = 0.1f;

    private:
        struct Piece {
            Turn rate;
            float seconds = 0;
        };

        std::vector<Piece> pieces;
        float queuedSeconds = 0;
        float keptBack = 0;
        // Real time since the last sample: samples have been coming while it is short, so a
        // frame that runs dry, even to nothing, is one the margin should have covered.
        float sinceSample = 1.0f;
        // The least queued past a frame's worth before a frame took it, over the current
        // second: well above zero for a whole second, the margin is more than needed.
        float lowest = 1.0f;
        float windowSeconds = 0;
        // What is usually left past a frame's worth, averaged.
        float averageSpare = 0;
        // Kept back past what ran dry, so the next burst a little late does not.
        static constexpr float headroom = 0.001f;
    };

    // The amounts for the look axes (aTurn and aLookUp, given with console::heldAxis) that turn
    // the view by `angle` times `sensitivity`, however long the tick that reads them. At
    // sensitivity 1 the view turns as far as the controller did, at the game's default field of
    // view.
    struct Look {
        float turn = 0;
        float lookUp = 0;
    };

    Look lookAxes(Turn angle, float sensitivity);
    // The amounts that turn the view by exactly `angle` at a field of view of `fov` degrees, as
    // a flick does.
    Look exactLookAxes(Turn angle, float fov);
    // The view's turn, in rotation units (65536 a full turn), those amounts give at a field of
    // view of `fov` degrees: the game's PlayerInput, PlayerMove and UpdateRotation.
    Turn viewTurn(Look axes, float fov);
}
