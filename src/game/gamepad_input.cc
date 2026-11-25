#include "game/gamepad_input.h"

#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/vm.h"
#include "deusex/aim_assist.h"
#include "deusex/gamepad_layout.h"
#include "deusex/gamepad_menus.h"
#include "deusex/hud.h"
#include "deusex/pad_inventory.h"
#include "deusex/prompts.h"
#include "deusex/quickslots.h"
#include "deusex/script.h"
#include "engine/console.h"
#include "engine/props_engine.h"
#include "extension/extension.h"
#include "game/switch_gyro.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace dx::game {
    namespace {
        using gamepad::Action;
        using gamepad::Input;

        // Takes the pad off the list, handing back its handle; none when it is not there.
        sdl::Gamepad take(std::vector<sdl::Gamepad>& devices, SDL_Gamepad* pad)
        {
            auto it = std::find_if(devices.begin(), devices.end(), [&](const sdl::Gamepad& each) { return each.get() == pad; });
            if (it == devices.end()) {
                return {};
            }
            sdl::Gamepad owned = std::move(*it);
            devices.erase(it);
            return owned;
        }

        // The quick slot a d-pad direction uses or assigns, or -1.
        int quickSlotOf(Action action, gamepad::Button button)
        {
            return gamepad::triggers(action, button) ? gamepad::clockwise(button) : -1;
        }

        bool isArrowKey(uint8_t key)
        {
            return key == IK_Up || key == IK_Down || key == IK_Left || key == IK_Right;
        }

        // A held inventory item takes the arrows, and an options row left and right;
        // otherwise they move focus.
        void menuNavigate(uint8_t key)
        {
            if (!padinventory::steer(key) && !gamepadmenu::adjust(key)) {
                extension::navigate(key);
            }
        }

        // Gyro turns slower than this, in radians a second (about 2 degrees), fade out as drift.
        constexpr float gyroDrift = 0.035f;

        // Analog keys the pad feeds, cleared when it goes.
        constexpr std::array<uint8_t, 7> analogKeys { IK_JoyX, IK_JoyY, IK_JoyU, IK_JoyV, IK_JoyZ, IK_JoyR, gamepad::leanKey };

        const char* const calibrationFile = "GyroCalibration.ini";
    }

    GamepadInput::GamepadInput(Host callbacks)
        : host(std::move(callbacks))
        , forgetReleased([this](const gc::ReleasedSet& released) {
            if (released.count(lastEquippedPlayer)) {
                lastEquippedPlayer = nullptr;
                lastEquippedItem = nullptr;
            }
            if (released.count(lastEquippedItem)) {
                lastEquippedItem = nullptr;
            }
        })
    {
        pressedAs.fill(gamepad::ButtonCount);
        repeating.fill(IK_None);
    }

    GamepadInput::~GamepadInput() = default;

    SDL_Gamepad* GamepadInput::find(SDL_JoystickID id) const
    {
        for (const sdl::Gamepad& each : devices) {
            if (SDL_GetGamepadID(each.get()) == id) {
                return each.get();
            }
        }
        return nullptr;
    }

    // Every connected pad is open, and whichever was used last is the one in play. Any press
    // or push past usedAxis, from any pad, is a use of the pad, wherever the game is.
    void GamepadInput::handleEvent(const SDL_Event& event)
    {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (host.openDevices && !find(event.gdevice.which)) {
                if (sdl::Gamepad owned { SDL_OpenGamepad(event.gdevice.which) }) {
                    SDL_Gamepad* opened = owned.get();
                    devices.push_back(std::move(owned));
                    enableSensors(opened);
                    padLost = false;
                    if (!device) {
                        setDevice(opened);
                    }
                    glyphs::Style style = promptStyleFor(SDL_GetGamepadType(opened), SDL_GetRealGamepadType(opened), SDL_GetGamepadButtonLabel(opened, SDL_GAMEPAD_BUTTON_SOUTH));
                    promptPads.connected(event.gdevice.which, style);
                    publishPromptStyle();
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (SDL_Gamepad* removed = find(event.gdevice.which)) {
                promptPads.removed(event.gdevice.which);
                publishPromptStyle();
                if (removed == device) {
                    unplugged();
                } else {
                    take(devices, removed);
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (SDL_Gamepad* from = find(event.gbutton.which)) {
                bool pressed = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                if (pressed) {
                    promptPads.used(event.gbutton.which);
                    publishPromptStyle();
                    markUsed();
                    setDevice(from);
                }
                // A release from a pad no longer in play was let go of when it stopped being.
                if (from == device) {
                    physical(gamepad::fromSdl(event.gbutton.button), pressed);
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (SDL_Gamepad* from = find(event.gaxis.which); from && std::abs(static_cast<float>(event.gaxis.value) / 32767.0f) > usedAxis) {
                promptPads.used(event.gaxis.which);
                publishPromptStyle();
                markUsed();
                setDevice(from);
            }
            break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            // Each sample's rate counts for the time since the one before; a gap of over a tenth
            // of a second, such as the first sample, counts for nothing.
            if (!gyroWanted() || !device || find(event.gsensor.which) != device) {
                break;
            }
            if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
                pad.gravity.accelerometer({ event.gsensor.data[0], event.gsensor.data[1], event.gsensor.data[2] });
                calibration.accelerometer({ event.gsensor.data[0], event.gsensor.data[1], event.gsensor.data[2] });
            } else if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
                uint64_t time = event.gsensor.sensor_timestamp;
                float seconds = pad.gyroSampleTime && time > pad.gyroSampleTime ? static_cast<float>(time - pad.gyroSampleTime) * 1e-9f : 0.0f;
                pad.gyroSampleTime = time;
                rawGyroSample({ event.gsensor.data[0], event.gsensor.data[1], event.gsensor.data[2] }, seconds < 0.1f ? seconds : 0.0f);
            }
            break;
        default:
            break;
        }
    }

    // With no pad left the last style stays, for prompts drawn before the keyboard takes over.
    void GamepadInput::publishPromptStyle() const
    {
        if (std::optional<glyphs::Style> style = promptPads.style()) {
            glyphs::setPadStyle(*style);
        }
    }

    SDL_JoystickID GamepadInput::activeController() const
    {
        return device ? SDL_GetGamepadID(device) : 0;
    }

    // The one way the pad in play changes, so nothing the last one held sticks down and
    // nothing measured on it, such as its drift or down, carries over to the next. A
    // calibration under way starts again on the new pad, so it measures one pad only.
    void GamepadInput::setDevice(SDL_Gamepad* next)
    {
        if (next == device) {
            return;
        }
        if (device) {
            releaseAll();
        }
        device = next;
        pad = {};
        pad.drift = savedDrift();
        if (calibration.state() == gyro::Calibration::State::Measuring) {
            calibration.start();
        }
    }

    void GamepadInput::enableSensors(SDL_Gamepad* each)
    {
        // The accelerometer finds down for player space.
        for (SDL_SensorType sensor : { SDL_SENSOR_GYRO, SDL_SENSOR_ACCEL }) {
            if (each && SDL_GamepadHasSensor(each, sensor)) {
                SDL_SetGamepadSensorEnabled(each, sensor, gyroWanted());
            }
        }
    }

    // A gyro rate held for `seconds` (0 for none): it turns gravity, and in the chosen space adds
    // to the view's turn this frame.
    void GamepadInput::addGyroSample(gyro::Turn rate, float seconds)
    {
        pad.lastGyro = rate;
        pad.gravity.turn(rate, seconds);
        if (!gyroHeldOff()) {
            pad.gyroTurn.add(gyro::view(rate, pad.gravity.down(), gyroSpace), seconds, gyroDrift);
        }
    }

    // A button with GyroOff is held: from the moment it is pressed, so a long press on the
    // right stick turns it off at once.
    bool GamepadInput::gyroHeldOff() const
    {
        for (int which = 0; which < gamepad::ButtonCount; which++) {
            if (held[static_cast<size_t>(which)] && gamepad::triggers(Action::GyroOff, static_cast<gamepad::Button>(which))) {
                return true;
            }
        }
        return false;
    }

    void GamepadInput::setGyro(bool enabled, float sensitivity, gyro::Space space)
    {
        gyroSensitivity = sensitivity;
        gyroSpace = space;
        if (enabled == gyroEnabled) {
            return;
        }
        gyroEnabled = enabled;
        // The readings start afresh; the pad's drift stays.
        gyro::Turn drift = pad.drift;
        pad = {};
        pad.drift = drift;
        updateSensors();
    }

    bool GamepadInput::gyroWanted() const
    {
        return gyroEnabled || calibration.state() == gyro::Calibration::State::Measuring;
    }

    // Turns the pads' sensors, and the Switch's own, on or off as gyroWanted says.
    void GamepadInput::updateSensors()
    {
        for (const sdl::Gamepad& each : devices) {
            enableSensors(each.get());
        }
        if (gyroWanted()) {
            switchgyro::start();
        } else {
            switchgyro::stop();
        }
    }

    // A reading as the pad gives it: measured while calibrating, then with the pad's drift
    // taken off, aimed with.
    void GamepadInput::rawGyroSample(gyro::Turn rate, float seconds)
    {
        if (calibration.state() == gyro::Calibration::State::Measuring) {
            calibration.add(rate, seconds);
            gyro::Calibration::State state = calibration.state();
            if (state == gyro::Calibration::State::Done) {
                pad.drift = calibration.drift();
                saveDrift(pad.drift);
            }
            if (state == gyro::Calibration::State::Failed) {
                log::info("DevInput", "gyro calibration: not still for {} s in a row in {} tries; the largest stray reading was {:.3f} rad/s", gyro::Calibration::stillSeconds, calibration.restarts(), calibration.largestStray());
            }
            if (state != gyro::Calibration::State::Measuring) {
                updateSensors();
            }
        }
        if (gyroEnabled) {
            addGyroSample(gyro::corrected(rate, pad.drift), seconds);
        }
    }

    void GamepadInput::startGyroCalibration()
    {
        calibration.start();
        pad.gyroSampleTime = 0;
        updateSensors();
    }

    // The Switch's own sensors are one set as far as the saved calibration goes; a desktop pad
    // is known by its serial number where it reports one, else by its model.
    std::string GamepadInput::calibrationKey() const
    {
        if (platform::kind() == platform::Kind::Switch) {
            return "Switch";
        }
        if (!device) {
            return {};
        }
        if (const char* serial = SDL_GetGamepadSerial(device); serial && *serial) {
            return std::string("Serial-") + serial;
        }
        char guid[64] {};
        SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(device)), guid, sizeof(guid));
        return std::string("Model-") + guid;
    }

    gyro::Turn GamepadInput::savedDrift() const
    {
        std::string key = calibrationKey();
        Ini saved;
        if (key.empty() || !saved.load(config::userDirectory() / calibrationFile)) {
            return {};
        }
        std::optional<std::string> value = saved.get("Gyro", key);
        gyro::Turn drift;
        if (value && std::sscanf(value->c_str(), "%f %f %f", &drift.pitch, &drift.yaw, &drift.roll) == 3) {
            return drift;
        }
        return {};
    }

    void GamepadInput::saveDrift(gyro::Turn drift) const
    {
        std::string key = calibrationKey();
        if (!key.empty()) {
            char value[96];
            std::snprintf(value, sizeof(value), "%.6f %.6f %.6f", static_cast<double>(drift.pitch), static_cast<double>(drift.yaw), static_cast<double>(drift.roll));
            Ini saved;
            saved.load(config::userDirectory() / calibrationFile);
            saved.set("Gyro", key, value);
            if (!saved.save()) {
                log::warn("DevInput", "gyro calibration: cannot save {}", saved.path.string());
            }
        }
        log::info("DevInput", "gyro calibration for {}: drift {:.4f} {:.4f} {:.4f} rad/s", key, drift.pitch, drift.yaw, drift.roll);
    }

    void GamepadInput::keyboardUsed()
    {
        usingGamepad = false;
        padLost = false;
    }

    void GamepadInput::setAimAssist(bool enabled)
    {
        aimAssist = enabled;
    }

    void GamepadInput::setStickTuning(const StickTuning& tuning)
    {
        sticks = tuning;
    }

    float GamepadInput::timeScale() const
    {
        return 1.0f - radialPause * radialPause * (3.0f - 2.0f * radialPause);
    }

    void GamepadInput::rememberEquippedItem()
    {
        UObject* player = host.player();
        if (player != lastEquippedPlayer) {
            lastEquippedPlayer = player;
            lastEquippedItem = nullptr;
        }
        UObject* item = script::object(player, "inHandPending");
        if (!item) {
            item = script::object(player, "inHand");
        }
        if (item) {
            lastEquippedItem = item;
        }
    }

    bool GamepadInput::restoreEquippedItem()
    {
        UObject* player = host.player();
        if (!player || script::object(player, "inHand") || script::object(player, "inHandPending")) {
            return false;
        }
        // Inventory membership also protects against dropped, consumed or unloaded objects.
        for (UObject* item : script::inventory(player)) {
            if (item == lastEquippedItem && !item->destroyed) {
                vm::Call(player, Name("PutInHand")).obj(item).run();
                return true;
            }
        }
        lastEquippedItem = nullptr;
        return false;
    }

    // Any use of the pad switches the HUD and menus over to it, as the mouse or keyboard
    // switches them back.
    void GamepadInput::markUsed()
    {
        usingGamepad = true;
        extension::useGamepad();
    }

    bool GamepadInput::inGame() const
    {
        return !extension::menuOpen() && host.player() && hud::shown();
    }

    void GamepadInput::physical(gamepad::Button which, bool pressed)
    {
        if (which >= gamepad::ButtonCount || physicalDown[which] == pressed) {
            return;
        }
        physicalDown[which] = pressed;
        if (!pressed) {
            gamepad::Button as = std::exchange(pressedAs[which], gamepad::ButtonCount);
            // A role another button still holds stays down.
            if (std::find(pressedAs.begin(), pressedAs.end(), as) == pressedAs.end()) {
                button(as, false);
            }
            return;
        }
        gamepad::Button as = extension::menuOpen() ? which : gamepad::role(which);
        pressedAs[which] = as;
        button(as, true);
    }

    void GamepadInput::button(gamepad::Button which, bool pressed)
    {
        if (which >= gamepad::ButtonCount || held[which] == pressed) {
            return;
        }
        held[which] = pressed;
        if (pressed) {
            onRelease[which] = press(which);
        } else if (Release release = std::exchange(onRelease[which], nullptr)) {
            release();
        }
    }

    // Does what pressing a role does now, and returns what letting go of it will do, so the
    // release undoes the press whatever has changed since.
    GamepadInput::Release GamepadInput::press(gamepad::Button which)
    {
        if (radial != Radial::None) {
            radialPress(which);
            return {};
        }
        // Play controls need the game's HUD up, so the title map, the intro and cutscenes only
        // get the buttons' plain bindings, as the keyboard does.
        bool play = inGame();
        if (int slot = quickSlotOf(Action::QuickSlots, which); play && slot >= 0) {
            quickslots::use(host.player(), static_cast<quickslots::Slot>(slot));
            return {};
        }
        if (play && gamepad::longPressOn(which)) {
            return holdPress(which);
        }
        bool belt = gamepad::triggers(Action::BeltRadial, which);
        if (play && (belt || gamepad::triggers(Action::AugRadial, which))) {
            openRadial(belt ? Radial::Belt : Radial::Augs, which, -1);
            // Letting go of the bumper that opened it only closes it.
            return [this, which] {
                if (radialOpener == which) {
                    closeRadial();
                }
            };
        }
        bool menu = extension::menuOpen();
        if (!menu && gamepad::triggers(Action::Crouch, which)) {
            crouch(true);
            return [this] { crouch(false); };
        }
        // In menus a button acts as its menu key unless the screen has a shortcut on it; in
        // play it sends its joystick key to the bindings.
        if (menu && gamepadmenu::press(which)) {
            return {};
        }
        if (menu && host.editText && gamepad::triggers(Action::MenuSelect, which) && host.editText()) {
            return {};
        }
        uint8_t key = menu ? gamepad::menuKey(which) : gamepad::buttonKey(which);
        if (menu && isArrowKey(key)) {
            menuNavigate(key);
            repeating[which] = key;
            repeat[which] = 0.4f;
            return [this, which] { repeating[which] = IK_None; };
        }
        if (key == IK_None) {
            return {};
        }
        host.sendKey(key, true);
        return [this, key] { host.sendKey(key, false); };
    }

    // While a radial is open, RadialUse uses the highlighted entry and RadialAssign puts it on
    // a quick slot. SwitchAmmo in the belt radial switches the equipped weapon's ammo, even
    // when no slice is selected.
    void GamepadInput::radialPress(gamepad::Button which)
    {
        UObject* player = host.player();
        if (radial == Radial::Belt && gamepad::triggers(Action::SwitchAmmo, which)) {
            if (radial::canSwitchAmmo(player)) {
                console::execute(player, "SwitchAmmo");
            }
            return;
        }
        std::vector<radial::Entry> entries = radialEntries();
        if (radialSelected < 0 || radialSelected >= static_cast<int>(entries.size())) {
            return;
        }
        const radial::Entry& entry = entries[static_cast<size_t>(radialSelected)];
        int quickSlot = quickSlotOf(Action::RadialAssign, which);
        if (gamepad::triggers(Action::RadialUse, which) && !entry.command.empty() && player) {
            console::execute(player, entry.command);
        } else if (quickSlot >= 0 && !entry.id.empty()) {
            quickslots::assign(static_cast<quickslots::Slot>(quickSlot), entry.id);
        }
    }

    // A tap-or-hold role: the hold runs in updateHolds, and letting go before it did is a tap,
    // unless holdPending was cleared meanwhile.
    GamepadInput::Release GamepadInput::holdPress(gamepad::Button which)
    {
        rememberEquippedItem();
        holdPending[which] = true;
        holdUsed[which] = false;
        holdTime[which] = 0;
        return [this, which] {
            if (std::exchange(holdPending[which], false) && !holdUsed[which]) {
                tap(which);
            }
        };
    }

    // A tap of a tap-or-hold role sends its binding. With empty hands, Reload's draws again
    // what was last put away.
    void GamepadInput::tap(gamepad::Button which)
    {
        if (inGame() && gamepad::triggers(Action::Reload, which)) {
            rememberEquippedItem();
            if (restoreEquippedItem()) {
                return;
            }
        }
        host.sendKey(gamepad::buttonKey(which), true);
        host.sendKey(gamepad::buttonKey(which), false);
    }

    // The pad's crouch toggles, but not through the console's Toggle: that flips a high bit
    // the script does not know, and with Toggle Crouch on DeusExPlayer.HandleWalking holds
    // bDuck at 1 while crouched, after which Toggle only moves it between two values that
    // both crouch. With Toggle Crouch on the game toggles, so the button is held as the
    // keyboard's Duck is; off, each press stands or crouches.
    void GamepadInput::crouch(bool pressed)
    {
        UObject* player = host.player();
        UProperty* duck = player ? player->cls->findProperty(Name("bDuck")) : nullptr;
        if (!duck || duck->type != PropType::Byte) {
            return;
        }
        uint8_t& value = player->data[duck->offset];
        bool gameToggles = false;
        if (UProperty* setting = player->cls->findProperty(Name("bToggleCrouch"))) {
            gameToggles = setting->toText(player->data + setting->offset) == "True";
        }
        if (gameToggles) {
            value = pressed ? 1 : 0;
        } else if (pressed) {
            value = value != 0 ? 0 : 1;
        }
    }

    void GamepadInput::openRadial(Radial which, gamepad::Button opener, int selected)
    {
        // A hold begun before the wheel opened neither holds nor taps after it closes.
        holdPending.fill(false);
        radial = which;
        radialOpener = opener;
        radialSelected = selected;
    }

    void GamepadInput::closeRadial()
    {
        radial = Radial::None;
        radialSelected = -1;
    }

    std::vector<radial::Entry> GamepadInput::radialEntries() const
    {
        UObject* player = host.player();
        switch (radial) {
        case Radial::Belt:
            return radial::beltEntries(player);
        case Radial::Augs:
            return radial::augEntries(player);
        case Radial::None:
            break;
        }
        return {};
    }

    // Counts held buttons toward their long press. Returns the lean, from the Move stick
    // while Lean is held, during which the caller stops that stick moving the player: the
    // game only leans someone standing still.
    float GamepadInput::updateHolds(float moveX, float deltaSeconds)
    {
        float lean = 0;
        UObject* player = host.player();
        for (const gamepad::Mapping& hold : gamepad::mappings()) {
            gamepad::Button which = hold.control.button;
            if (hold.control.input != gamepad::Input::LongPress || !holdPending[which]) {
                continue;
            }
            holdTime[which] += deltaSeconds;
            if (hold.action == Action::Lean) {
                lean = gamepad::shapeStick(moveX, 0, 0.25f, 1.0f).x;
                holdUsed[which] = holdUsed[which] || lean != 0 || holdTime[which] > hold.holdSeconds;
                continue;
            }
            // Gyro off follows the button itself (gyroHeldOff); held past the tap, the press
            // is only that.
            if (hold.action == Action::GyroOff) {
                holdUsed[which] = holdUsed[which] || holdTime[which] > hold.holdSeconds;
                continue;
            }
            if (holdUsed[which] || holdTime[which] < hold.holdSeconds || !player) {
                continue;
            }
            holdUsed[which] = true;
            // The title map is a level too, but there is no game to save.
            bool save = hold.action == Action::QuickSave;
            if (save && hud::atTitleMap()) {
                continue;
            }
            console::execute(player, hold.command);
            // The game saves without a word, which leaves a pad player unsure the hold took.
            if (save && pawn::Health(player) > 0) {
                vm::Call(player, Name("ClientMessage")).s("Quick saved").run();
            }
        }
        return lean;
    }

    // Pushing the left stick only part of the way walks rather than runs: the game's own
    // movement reaches full speed from any push, so the walk input is held meanwhile.
    void GamepadInput::updateWalk(const gamepad::Stick& move)
    {
        float push = std::sqrt(move.x * move.x + move.y * move.y);
        walking = push > 0 && (walking ? push < 0.65f : push < 0.55f);
        // The walk input means walk with the game's default always-run, and run without it.
        bool alwaysRun = true;
        UObject* player = host.player();
        if (UProperty* setting = player ? player->cls->findProperty(Name("bAlwaysRun")) : nullptr) {
            alwaysRun = setting->toText(player->data + setting->offset) == "True";
        }
        bool hold = alwaysRun ? walking : push > 0 && !walking;
        if (hold != walkHeld) {
            walkHeld = hold;
            console::key(gamepad::walkKey, hold ? IST_Press : IST_Release, 0);
        }
    }

    // In menus MenuNavigate's stick steps focus like the d-pad, and both repeat while held.
    // MenuScroll's stick scrolls, faster the further it is pushed.
    void GamepadInput::updateMenu(gamepad::Stick navigate, float scrollY, float deltaSeconds)
    {
        gamepad::Stick scroll = gamepad::shapeStick(0, scrollY, 0.3f, 1.0f);
        bool up = scroll.y < 0;
        if (scroll.y == 0) {
            scrolling = false;
        } else {
            // One step as soon as the stick tips, then up to 15 a second.
            if (!scrolling) {
                scrolling = true;
                scrollSteps = 0;
                extension::scroll(up);
            }
            scrollSteps += std::abs(scroll.y) * 15.0f * deltaSeconds;
            for (; scrollSteps >= 1.0f; scrollSteps -= 1.0f) {
                extension::scroll(up);
            }
        }
        gamepad::Stick stick = gamepad::shapeStick(navigate.x, navigate.y, 0.5f, 1.0f);
        uint8_t direction = IK_None;
        if (stick.x != 0 || stick.y != 0) {
            if (std::abs(stick.x) > std::abs(stick.y)) {
                direction = stick.x < 0 ? IK_Left : IK_Right;
            } else {
                direction = stick.y < 0 ? IK_Up : IK_Down;
            }
        }
        if (direction != stickDirection) {
            stickDirection = direction;
            stickRepeat = 0.4f;
            if (direction != IK_None) {
                menuNavigate(direction);
            }
        } else if (direction != IK_None) {
            stickRepeat -= deltaSeconds;
            if (stickRepeat <= 0) {
                stickRepeat += 0.12f;
                menuNavigate(direction);
            }
        }
        for (size_t which = 0; which < gamepad::ButtonCount; which++) {
            if (repeating[which] == IK_None) {
                continue;
            }
            repeat[which] -= deltaSeconds;
            if (repeat[which] <= 0) {
                repeat[which] += 0.12f;
                menuNavigate(repeating[which]);
            }
        }
    }

    void GamepadInput::update(World* world, float deltaSeconds)
    {
        rememberEquippedItem();
        bool menu = extension::menuOpen();
        if (menu) {
            closeRadial();
        }
        // Use wall time, not the scaled world delta, so a fully paused wheel can still resume.
        constexpr float rampSeconds = 0.2f;
        float step = std::max(0.0f, deltaSeconds) / rampSeconds;
        radialPause = std::clamp(radialPause + (radial == Radial::None ? -step : step), 0.0f, 1.0f);
        hud::layout(usingGamepad);
        bool calibrating = calibration.state() == gyro::Calibration::State::Measuring;
        calibration.tick(std::clamp(deltaSeconds, 0.0f, 0.1f));
        if (calibrating && calibration.state() != gyro::Calibration::State::Measuring) {
            updateSensors();
        }
        padlayout::showPaddles(device && SDL_GamepadHasButton(device, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1));
        if (!device) {
            return;
        }
        auto axis = [this](SDL_GamepadAxis which) {
            return static_cast<float>(SDL_GetGamepadAxis(device, which)) / 32767.0f;
        };
        float leftX = axis(SDL_GAMEPAD_AXIS_LEFTX);
        float leftY = axis(SDL_GAMEPAD_AXIS_LEFTY);
        float rightX = axis(SDL_GAMEPAD_AXIS_RIGHTX);
        float rightY = axis(SDL_GAMEPAD_AXIS_RIGHTY);
        float leftTrigger = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        float rightTrigger = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        // A stalled frame, such as a level load, must not complete a hold or a repeat.
        float timerDelta = std::clamp(deltaSeconds, 0.0f, 0.1f);
        if (gyroWanted()) {
            if (std::optional<switchgyro::Sample> sample = switchgyro::poll()) {
                pad.gravity.accelerometer(sample->accelerometer);
                calibration.accelerometer(sample->accelerometer);
                rawGyroSample(sample->rate, timerDelta);
            }
        }
        // Gyro off stops the turn at once, with what was queued of it.
        if (gyroHeldOff()) {
            pad.gyroTurn.clear();
        }
        // Only play turns the view by the gyro; what it turned meanwhile is dropped.
        gyro::Turn gyroAngle = pad.gyroTurn.take(timerDelta);
        // The stick an action is mapped to, as it is pushed.
        auto stickOf = [&](Action action, Input input) {
            gamepad::Button which = gamepad::control(action).button;
            for (const gamepad::Mapping& entry : gamepad::mappings()) {
                if (entry.action == action && entry.control.input == input) {
                    which = entry.control.button;
                    break;
                }
            }
            if (which == gamepad::LeftStick) {
                return gamepad::Stick { leftX, leftY };
            }
            if (which == gamepad::RightStick) {
                return gamepad::Stick { rightX, rightY };
            }
            return gamepad::Stick {};
        };
        physical(gamepad::LeftTrigger, gamepad::triggerHeld(leftTrigger, physicalDown[gamepad::LeftTrigger]));
        physical(gamepad::RightTrigger, gamepad::triggerHeld(rightTrigger, physicalDown[gamepad::RightTrigger]));
        gamepad::Stick move;
        gamepad::Stick look;
        float lean = 0;
        if (menu || radial != Radial::None) {
            // A flick held into a menu or radial starts afresh after it.
            pad.flick.reset();
        }
        if (menu) {
            leftTrigger = 0;
            rightTrigger = 0;
            updateMenu(stickOf(Action::MenuNavigate, Input::Stick), stickOf(Action::MenuScroll, Input::StickVertical).y, timerDelta);
        } else if (radial != Radial::None) {
            // Either stick picks a slice; the player stands still meanwhile.
            int count = static_cast<int>(radialEntries().size());
            int picked = radial::pick(rightX, rightY, count, 0.5f);
            if (picked < 0) {
                picked = radial::pick(leftX, leftY, count, 0.5f);
            }
            if (picked >= 0) {
                radialSelected = picked;
            }
        } else {
            gamepad::Stick moveStick = stickOf(Action::Move, Input::Stick);
            gamepad::Stick lookStick = stickOf(Action::Look, Input::Stick);
            move = gamepad::shapeStick(moveStick.x, moveStick.y, sticks.moveDeadZone, 1.0f, sticks.moveOuterZone);
            // Flick stick turns by angles, given with the gyro's below, rather than a rate.
            float flickTurn = 0;
            if (flicking()) {
                flickTurn = pad.flick.update(lookStick.x, lookStick.y, timerDelta);
            } else {
                pad.flick.reset();
                look = gamepad::shapeStick(lookStick.x, lookStick.y, sticks.lookDeadZone, sticks.lookCurve, sticks.lookOuterZone);
                look.x *= sticks.lookSpeed;
                look.y *= sticks.lookSpeed;
            }
            lean = updateHolds(moveStick.x, timerDelta);
            if (holdPending[gamepad::button(Action::Lean)]) {
                move = {};
            }
            if (aimAssist) {
                float factor = aimassist::apply(world, host.player(), std::hypot(look.x, look.y), std::hypot(move.x, move.y), deltaSeconds);
                look.x *= factor;
                look.y *= factor;
            }
            if (gyroEnabled && host.player() && hud::shown()) {
                // The gyro turns the view by 0.24 of its sensitivity: as it always has, from when
                // the look axes' model left out PlayerMove's 0.24, kept for its feel.
                gyro::Look turned = gyro::lookAxes(gyroAngle, gyroSensitivity * 0.24f);
                // A flick turns as far as the stick points, whatever the zoom.
                gyro::Look flicked = gyro::exactLookAxes({ 0, -flickTurn }, script::number(host.player(), "DesiredFOV"));
                console::heldAxis("aTurn", turned.turn + flicked.turn);
                console::heldAxis("aLookUp", turned.lookUp);
            }
        }
        updateWalk(move);
        console::analog(IK_JoyX, move.x);
        console::analog(IK_JoyY, move.y);
        console::analog(IK_JoyU, look.x);
        console::analog(IK_JoyV, look.y);
        console::analog(IK_JoyZ, leftTrigger);
        console::analog(IK_JoyR, rightTrigger);
        console::analog(gamepad::leanKey, lean);
    }

    void GamepadInput::draw(Canvas2D& canvas, UObject* player)
    {
        // The diagram covers the HUD's parts; it is drawn under the cursor, as the extension's
        // overlay (game_start).
        if (padlayout::shown()) {
            return;
        }
        bool menu = extension::menuOpen();
        if (radial != Radial::None) {
            radial::draw(canvas, radialEntries(), radialSelected, radial == Radial::Belt ? "Belt" : "Augmentations", player, radial == Radial::Belt);
        }
        // The quick slots stay up over a radial, to show what is already assigned.
        if (usingGamepad && !menu && hud::shown()) {
            quickslots::draw(canvas, player);
            if (radial == Radial::None) {
                prompts::drawInteraction(canvas, player);
            }
        }
        if (menu && usingGamepad) {
            padinventory::drawOverlay(canvas);
            prompts::drawBar(canvas, gamepadmenu::prompts());
        }
        if (padLost) {
            prompts::drawNotice(canvas, "Controller disconnected. Reconnect it, or use the keyboard and mouse.");
        }
    }

    // Losing the pad in play hands it to another connected pad, or with none left pauses on
    // the main menu.
    void GamepadInput::unplugged()
    {
        if (device) {
            // Closed once the next pad is in play, as letting go of what it held still reads it.
            sdl::Gamepad gone = take(devices, device);
            setDevice(devices.empty() ? nullptr : devices.front().get());
            gone.reset();
            if (device) {
                return;
            }
        }
        padLost = usingGamepad;
        UObject* player = host.player();
        if (player && !extension::menuOpen()) {
            closeRadial();
            console::execute(player, "ShowMainMenu");
        }
    }

    void GamepadInput::releaseAll()
    {
        // A pad that goes lets go of its buttons without that counting as a tap.
        holdPending.fill(false);
        for (int which = 0; which < gamepad::ButtonCount; which++) {
            button(static_cast<gamepad::Button>(which), false);
        }
        physicalDown.fill(false);
        pressedAs.fill(gamepad::ButtonCount);
        updateWalk({});
        closeRadial();
        scrolling = false;
        stickDirection = IK_None;
        for (uint8_t key : analogKeys) {
            console::analog(key, 0);
        }
    }

    bool GamepadInput::scriptedAction(const std::string& verb, const std::string& rest)
    {
        if (verb == "radial") {
            // "radial belt 3" opens the belt wheel on slice 3 and "radial off" closes it, as
            // letting go of the bumper does; "pad a" uses the slice in between.
            char which[16] = {};
            int slice = -1;
            std::sscanf(rest.c_str(), "%15s %d", which, &slice);
            std::string name = which;
            if (name == "off") {
                closeRadial();
            } else if (name == "augs") {
                openRadial(Radial::Augs, gamepad::button(Action::AugRadial), slice);
            } else {
                openRadial(Radial::Belt, gamepad::button(Action::BeltRadial), slice);
            }
        } else if (verb == "nav") {
            markUsed();
            menuNavigate(console::keyFromName(rest));
        } else if (verb == "scroll") {
            markUsed();
            extension::scroll(rest == "up");
        } else if (verb == "pad") {
            // "pad x" taps a button by its gamepad::buttonName: a, b, x, y, lb, rb, lt, rt,
            // view, menu, the stick buttons ls and rs, the d-pad's up, down, left and right,
            // or the back paddles r4, l4, r5 and l5. It goes through the remapping as a real
            // press does.
            if (gamepad::Button which = gamepad::buttonFromName(rest); which != gamepad::ButtonCount) {
                markUsed();
                physical(which, true);
                physical(which, false);
            }
        } else if (verb == "paddown" || verb == "padup") {
            // "paddown b" holds a button until "padup b", for presses that last frames.
            if (gamepad::Button which = gamepad::buttonFromName(rest); which != gamepad::ButtonCount) {
                bool down = verb == "paddown";
                if (down) {
                    markUsed();
                }
                physical(which, down);
            }
        } else if (verb == "unplug") {
            unplugged();
        } else {
            return false;
        }
        return true;
    }
}
