#pragma once

#include "core/gc.h"
#include "core/object.h"
#include "core/sdl_handles.h"
#include "deusex/radial.h"
#include "engine/canvas.h"
#include "engine/gamepad.h"
#include "engine/gyro.h"
#include "engine/input.h"
#include "game/pad_prompts.h"

#include <SDL3/SDL.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dx {
    class World;
}

namespace dx::game {
    // The controller in play and in menus: buttons become the game's joystick keys, menu keys
    // or the native shortcuts (radials, quick slots, holds, the pad's own menu shortcuts), and
    // the sticks and triggers feed the game's axes, all as the mapping in engine/gamepad.h
    // says. Whichever of the pad or the keyboard and mouse was used last decides which prompts
    // are drawn.
    class GamepadInput {
    public:
        struct Host {
            // The player of the world in play, or null.
            std::function<UObject*()> player;
            // Sends a key through the usual keyboard path.
            std::function<void(uint8_t key, bool pressed)> sendKey;
            // MenuSelect on the focused window: true when it was a text field and a system
            // keyboard took the press. Unset where the real keyboard types.
            std::function<bool()> editText;
            // False leaves controllers alone, for hidden automated runs: controller input is
            // global and would reach whoever is using the desktop.
            bool openDevices = true;
        };

        explicit GamepadInput(Host callbacks);
        ~GamepadInput();
        GamepadInput(const GamepadInput&) = delete;
        GamepadInput& operator=(const GamepadInput&) = delete;

        // The level changed: the player and item remembered for re-equipping are gone.
        void levelChanged()
        {
            lastEquippedPlayer = nullptr;
            lastEquippedItem = nullptr;
        }

        // SDL's gamepad events.
        void handleEvent(const SDL_Event& event);
        // The keyboard or mouse was used, which switches the prompts back to them.
        void keyboardUsed();
        // Reads the sticks and triggers and runs held buttons and repeats. Called each frame, with
        // the world in play or null when there is none.
        void update(World* world, float deltaSeconds);
        void setAimAssist(bool enabled);
        // Aiming by turning the controller, on top of the Look stick, during play only.
        // Sensitivity 1 turns the view as far as the controller turns; space is how its turn
        // becomes the view's.
        void setGyro(bool enabled, float sensitivity, gyro::Space space);

        // The sticks' response, from the Gamepad* settings: dead and outer zones as parts of
        // the stick's travel, the Look stick's curve (the power its push is raised to) and speed
        // (1 as it always was), and flick stick, which takes over the Look stick while gyro
        // aiming is on.
        struct StickTuning {
            float moveDeadZone = 0.2f;
            float moveOuterZone = 1.0f;
            float lookDeadZone = 0.15f;
            float lookOuterZone = 1.0f;
            float lookCurve = 2.0f;
            float lookSpeed = 1.0f;
            bool flickStick = false;
        };

        void setStickTuning(const StickTuning& tuning);
        // Real-time easing into a full pause while a radial is open, and back out on release.
        float timeScale() const;
        // The radials, quick slots and prompts over the frame.
        void draw(Canvas2D& canvas, UObject* player);
        // Losing the pad in play hands over to another connected pad; with none left, mid-game
        // it pauses on the main menu, as a console port would, and says to reconnect it.
        void unplugged();

        // The pad in play, or 0 with none.
        SDL_JoystickID activeController() const;

        // Read-only views for the Input tool: every open pad, the pad the prompts follow,
        // whether the pad was the last thing used, whether gyro aiming is on, and the gyro rate
        // last read from the pad in play (none before the first, or with gyro aiming off).
        const std::vector<sdl::Gamepad>& openPads() const
        {
            return devices;
        }

        std::optional<SDL_JoystickID> promptPad() const
        {
            return promptPads.current();
        }

        bool gamepadLastUsed() const
        {
            return usingGamepad;
        }

        bool gyroAiming() const
        {
            return gyroEnabled;
        }

        std::optional<gyro::Turn> lastGyroRate() const
        {
            return pad.lastGyro;
        }

        // Measures the pad in play's gyro drift while it lies still, then saves it for that
        // controller (in GyroCalibration.ini in the user folder) and takes it off every reading.
        // Works with gyro aiming off too.
        void startGyroCalibration();

        gyro::Calibration::State gyroCalibrationState() const
        {
            return calibration.state();
        }

        // For the Input tool: down in the pad in play's axes, and the drift taken off its gyro.
        Vector gyroDown() const
        {
            return pad.gravity.down();
        }

        gyro::Turn gyroDriftTaken() const
        {
            return pad.drift;
        }

        // How long the gyro's turn is kept back to pay out evenly, in seconds.
        float gyroKeptBack() const
        {
            return pad.gyroTurn.margin();
        }

        // Scripted test actions: "radial belt 3" or "radial off", "pad x" to tap a button,
        // "paddown b" and "padup b" to hold one across frames, "nav down", "scroll up" and
        // "unplug". Returns false for any other verb.
        bool scriptedAction(const std::string& verb, const std::string& rest);

    private:
        enum class Radial { None,
            Belt,
            Augs };

        // What letting go of a role does, decided as it is pressed; empty for nothing.
        using Release = std::function<void()>;

        // A button as pressed on the pad, which presses the button whose role it has: its
        // remapped one in play (gamepad::role), itself in menus.
        void physical(gamepad::Button which, bool pressed);
        // A role pressed or let go. A press keeps the Release it returns, and letting go runs
        // it, whatever opened or closed in between.
        void button(gamepad::Button which, bool pressed);
        Release press(gamepad::Button which);
        void radialPress(gamepad::Button which);
        Release holdPress(gamepad::Button which);
        void tap(gamepad::Button which);
        void crouch(bool pressed);
        void markUsed();
        // Whether play controls act: the game's HUD is up, with no menu over it.
        bool inGame() const;
        void openRadial(Radial which, gamepad::Button button, int selected);
        void closeRadial();
        void rememberEquippedItem();
        bool restoreEquippedItem();
        std::vector<radial::Entry> radialEntries() const;
        float updateHolds(float moveX, float deltaSeconds);
        void updateWalk(const gamepad::Stick& move);
        void updateMenu(gamepad::Stick navigate, float scrollY, float deltaSeconds);
        // Lets go of everything held, without a hold or tap going off.
        void releaseAll();
        // Makes `next` (or none) the pad in play: everything the last one held is let go and
        // the state that follows the pad starts afresh.
        void setDevice(SDL_Gamepad* next);
        SDL_Gamepad* find(SDL_JoystickID id) const;
        // Tells the glyphs which style the pad in use calls for.
        void publishPromptStyle() const;

        // How far a stick or trigger has to move to count as using the pad, past the drift
        // and jitter of a stick left alone.
        static constexpr float usedAxis = 0.5f;

        Host host;
        // Every open pad, closed as it leaves the list, and the one in play (one of them): the
        // last to press a button or move an axis.
        std::vector<sdl::Gamepad> devices;
        SDL_Gamepad* device = nullptr;
        // Which pad the prompts follow. It moves with the same presses and pushes as the pad
        // in play, and also to a pad as it is connected.
        PromptPads promptPads;
        bool aimAssist = true;
        StickTuning sticks;

        // Flick stick is in use: set, with gyro aiming on.
        bool flicking() const
        {
            return sticks.flickStick && gyroEnabled;
        }

        // What follows the pad in play, made afresh by setDevice for each: its gyro's
        // readings, the turn queued from them, down by its accelerometer, its saved drift and
        // a flick under way.
        struct PadState {
            // Down in the pad's axes, for player space.
            gyro::Gravity gravity;
            gyro::Pacer gyroTurn;
            // The time of the last gyro sample, in nanoseconds, or 0.
            uint64_t gyroSampleTime = 0;
            std::optional<gyro::Turn> lastGyro;
            // The drift taken off each reading.
            gyro::Turn drift;
            gamepad::FlickStick flick;
        };

        PadState pad;

        bool gyroEnabled = false;
        float gyroSensitivity = 1;
        gyro::Space gyroSpace = gyro::Space::Player;
        void addGyroSample(gyro::Turn rate, float seconds);
        void rawGyroSample(gyro::Turn rate, float seconds);
        bool gyroHeldOff() const;
        // The gyro is read for aiming or for a calibration under way.
        bool gyroWanted() const;
        void updateSensors();
        // The pad in play's name in the saved calibrations, or empty with none.
        std::string calibrationKey() const;
        // The drift saved for the pad in play, or none.
        gyro::Turn savedDrift() const;
        void saveDrift(gyro::Turn drift) const;
        gyro::Calibration calibration;
        void enableSensors(SDL_Gamepad* pad);
        // Whether the gamepad was the last thing used, which decides if prompts are shown.
        bool usingGamepad = false;
        // The pad in use went with none left, so a notice asks for it back until a pad is
        // connected or the keyboard or mouse is used.
        bool padLost = false;

        // Each button's state on the pad, and the role it pressed, let go with it.
        std::array<bool, gamepad::ButtonCount> physicalDown {};
        std::array<gamepad::Button, gamepad::ButtonCount> pressedAs {};
        // Each role's state, and what letting go of it does.
        std::array<bool, gamepad::ButtonCount> held {};
        std::array<Release, gamepad::ButtonCount> onRelease {};
        // The arrow key a held role steps a menu's focus with, and when it next repeats.
        std::array<uint8_t, gamepad::ButtonCount> repeating {};
        std::array<float, gamepad::ButtonCount> repeat {};

        Radial radial = Radial::None;
        gamepad::Button radialOpener = gamepad::ButtonCount;
        int radialSelected = -1;
        float radialPause = 0;

        // Only dereference the remembered item after finding it in the current inventory.
        // Neither keeps its object: both are let go when it is deleted, so a new object made
        // at the same address is not taken for it.
        UObject* lastEquippedPlayer = nullptr;
        UObject* lastEquippedItem = nullptr;
        gc::ReleaseHook forgetReleased;

        // Tap-or-hold roles: whether one is down with its tap still to come, whether its hold
        // already acted, and for how long. A radial opening or the pad going clears holdPending,
        // which cancels both.
        std::array<bool, gamepad::ButtonCount> holdPending {};
        std::array<bool, gamepad::ButtonCount> holdUsed {};
        std::array<float, gamepad::ButtonCount> holdTime {};

        // Whether the left stick is pushed only lightly, and whether that holds the walk input.
        bool walking = false;
        bool walkHeld = false;

        // The right stick scrolling a menu, and the left stick stepping its focus.
        float scrollSteps = 0;
        bool scrolling = false;
        uint8_t stickDirection = IK_None;
        float stickRepeat = 0;
    };
}
