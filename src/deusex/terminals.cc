#include "deusex/terminals.h"

#include "core/vm.h"
#include "core/objects.h"
#include "deusex/known_logins.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "core/gc.h"
#include "core/package.h"
#include "engine/resources.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <algorithm>
#include <array>
#include <string>

namespace dx::terminals {
    namespace {
        std::string currentMap()
        {
            return World::hasActive() ? savegame::mapBaseName(World::active().url) : std::string();
        }

        // The computer screen a terminal shows, such as its login or an email.
        UObject* computerScreen(UObject* top)
        {
            return script::isA(top, "NetworkTerminal") ? script::object(top, "winComputer") : nullptr;
        }

        void setText(UObject* edit, const std::string& text)
        {
            if (edit) {
                vm::Call(edit, Name("SetText")).s(text).run();
            }
        }

        // Setting text doesn't count as typing, which is what enables a screen's buttons, so
        // they are enabled as typing would and the one to press next is focused.
        void readyButton(UObject* screen, const char* button)
        {
            vm::Call(screen, Name("EnableButtons")).run();
            extension::setFocusWindow(script::object(screen, button));
        }

        struct Login {
            // What gets filled in: the login screen, or the keypad for a door code.
            UObject* screen = nullptr;
            std::string name;
            std::string secret;
            bool keypad = false;
        };

        // A machine's logins are known when the player has read or heard sources the table
        // links to them. The table names the accounts; their passwords, PINs or door code
        // always come from the machine, so the same table serves every version of the maps.
        // A computer may know several accounts; the list keeps the table's order.
        std::vector<Login> knownLogins(UObject* top)
        {
            std::vector<Login> logins;
            if (script::isA(top, "HUDKeypadWindow")) {
                UObject* keypad = script::object(top, "keypadOwner");
                if (keypad && !knownlogins::knownAccounts(currentMap(), keypad->name.str()).empty()) {
                    logins.push_back({ top, std::string(), script::string(keypad, "validCode"), true });
                }
                return logins;
            }
            UObject* screen = computerScreen(top);
            UObject* owner = script::object(top, "compOwner");
            bool atm = script::isA(screen, "ComputerScreenATM");
            if (!owner || (!atm && !script::isA(screen, "ComputerScreenLogin"))) {
                return logins;
            }
            int32_t users = vm::Call(owner, Name("NumUsers")).result<int32_t>();
            for (const std::string& account : knownlogins::knownAccounts(currentMap(), owner->name.str())) {
                for (int32_t i = 0; i < users; i++) {
                    std::string name = vm::Call(owner, Name(atm ? "GetAccountNumber" : "GetUserName")).i(i).result<std::string>();
                    // An account the table names may not be on this version of the map.
                    if (iequals(name, account)) {
                        logins.push_back({ screen, name, vm::Call(owner, Name(atm ? "GetPIN" : "GetPassword")).i(i).result<std::string>(), false });
                        break;
                    }
                }
            }
            return logins;
        }

        struct LoginPopup {
            UObject* window = nullptr;
            UObject* terminal = nullptr;
            std::vector<Login> logins;
            std::vector<UObject*> buttons;
            UObject* cancel = nullptr;
        };

        LoginPopup loginPopup;

        // The button last given the known-login label, and the ATM screen last filled in.
        UObject* labelledButton = nullptr;
        std::string labelledText;
        UObject* filledScreen = nullptr;

        UTexture* loginBackground(uint32_t width, uint32_t height, UObject* outer)
        {
            std::array<UTexture*, 2> tiles {};
            std::array<std::vector<uint8_t>, 2> pixels;
            for (size_t i = 0; i < tiles.size(); i++) {
                tiles[i] = cast<UTexture>(objects::loadObject("DeusExUI.UserInterface.MenuMessageBoxBackground_" + std::to_string(i + 1), nullptr));
                if (!tiles[i] || tiles[i]->mips.empty()) {
                    return nullptr;
                }
                pixels[i] = tiles[i]->rgba(0);
            }
            std::vector<uint8_t> resized(static_cast<size_t>(width) * height * 4);
            // Preserve the original frame and masked corners; tile only the middle.
            // The stock artwork is a 280x85 panel split into 256-pixel-wide tiles.
            auto source = [](uint32_t at, uint32_t size, uint32_t original) {
                constexpr uint32_t edge = 16;
                if (at < edge) {
                    return at;
                }
                if (at >= size - edge) {
                    return original - (size - at);
                }
                return edge + (at - edge) % (original - 2 * edge);
            };
            for (uint32_t y = 0; y < height; y++) {
                uint32_t sy = source(y, height, 85);
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t sx = source(x, width, 280);
                    size_t tile = sx / 256;
                    size_t from = static_cast<size_t>(sy) * tiles[tile]->mips[0].width + sx % 256;
                    size_t to = (static_cast<size_t>(y) * width + x) * 4;
                    resized[to] = pixels[tile][from * 4 + 2];
                    resized[to + 1] = pixels[tile][from * 4 + 1];
                    resized[to + 2] = pixels[tile][from * 4];
                    resized[to + 3] = tiles[tile]->format() == 0 && tiles[tile]->mips[0].data[from] == 0 ? 0 : pixels[tile][from * 4 + 3];
                }
            }
            return ui::newTexture(width, height, std::move(resized), outer);
        }

        bool showLogins(UObject* top, const std::vector<Login>& logins)
        {
            UObject* root = extension::rootWindow();
            UObject* popup = vm::Call(root, Name("PushWindow")).obj(objects::findClass("DeusEx.MenuUIScreenWindow")).b(false).result<UObject*>();
            if (!popup) {
                return false;
            }
            loginPopup = { popup, top, logins, {}, nullptr };
            vm::Call(popup, Name("SetTitle")).s("Autologin").run();
            vm::Call(script::object(popup, "winHelp"), Name("Hide")).run();
            // Use the compact message-box frame, without the settings screen's side
            // extensions, which leave notches around a small credential list.
            for (const char* edge : { "winLeftEdge", "winRightEdge" }) {
                vm::Call(script::object(popup, edge), Name("Destroy")).run();
                if (auto* field = script::field<UObject*>(popup, edge)) {
                    *field = nullptr;
                }
            }
            UObject* client = script::object(popup, "winClient");
            float width = 280;
            for (const Login& login : logins) {
                width = std::max(width, 40.0f + 8.0f * static_cast<float>(login.name.size() + login.secret.size() + 3));
            }
            float height = 16 + 25 * static_cast<float>(logins.size());
            vm::Call(client, Name("SetSize")).f(width).f(height).run();
            vm::Call(client, Name("SetTextureLayout")).i(0).i(0).run();
            vm::Call(client, Name("SetBackground")).obj(loginBackground(static_cast<uint32_t>(width), static_cast<uint32_t>(height), popup)).run();
            vm::Call(client, Name("SetBackgroundStyle")).i(2).run(); // DSTY_Masked
            for (size_t i = 0; i < logins.size(); i++) {
                UObject* button = vm::Call(client, Name("NewChild")).obj(objects::findClass("DeusEx.MenuUIActionButtonWindow")).b(true).result<UObject*>();
                vm::Call(button, Name("SetButtonText")).s(logins[i].name + " / " + logins[i].secret).run();
                vm::Call(button, Name("SetPos")).f(8).f(8 + 25 * static_cast<float>(i)).run();
                vm::Call(button, Name("SetSize")).f(width - 16).f(19).run();
                loginPopup.buttons.push_back(button);
            }
            loginPopup.cancel = vm::Call(script::object(popup, "winButtonBar"), Name("AddButton")).s("Cancel").i(2).result<UObject*>();
            vm::Call(popup, Name("AskParentForReconfigure")).run();
            extension::setFocusWindow(loginPopup.buttons.front());
            return true;
        }

        // The screen's Autologin button, or a keypad's Enter Code one, made on the screen's own
        // button bar so it has the game's font, bevels, theme and layout.
        UObject* knownLoginButton(UObject* screen, bool create)
        {
            bool keypad = script::isA(screen, "HUDKeypadWindow");
            auto* bar = cast<extension::UWindow>(keypad ? screen : script::object(screen, "winButtonBar"));
            if (!bar) {
                return nullptr;
            }
            for (extension::UWindow* child = bar->firstChild(); child; child = child->nextSibling()) {
                if (script::object(child, "clientObject") == screen) {
                    return child;
                }
            }
            if (!create) {
                return nullptr;
            }
            UObject* button = nullptr;
            if (keypad) {
                // A freshly pushed keypad is still at the default window size until its first
                // layout pass, so wait for that rather than sizing the footer from 10x10.
                if (bar->width() < 50) {
                    return nullptr;
                }
                button = vm::Call(bar, Name("NewChild")).obj(objects::findClass("DeusEx.MenuUIActionButtonWindow")).b(true).result<UObject*>();
                if (button) {
                    // A small footer under the keypad's number panel, which stays as it was.
                    if (UObject** font = script::field<UObject*>(button, "fontButtonText")) {
                        *font = ui::font("DeusExUI.FontMenuSmall");
                    }
                    vm::Call(button, Name("SetButtonText")).s("Enter Code").run();
                    vm::Call(button, Name("SetPos")).f(8).f(bar->height() + 2).run();
                    vm::Call(button, Name("SetSize")).f(bar->width() - 16).f(19).run();
                    vm::Call(bar, Name("SetHeight")).f(bar->height() + 25).run();
                }
            } else {
                // Left-aligned beside the right-aligned Login and Close buttons.
                button = vm::Call(bar, Name("AddButton")).s("Autologin").i(0).result<UObject*>();
            }
            if (button) {
                // Marked with its screen, so going back to the screen finds this button
                // instead of adding a second one.
                vm::Call(button, Name("SetClientObject")).obj(screen).run();
                vm::Call(button, Name("SetSelectability")).b(!keypad).run();
            }
            return button;
        }

        void updateKnownLoginButton(UObject* top, bool dialing)
        {
            UObject* screen = script::isA(top, "HUDKeypadWindow") ? top : computerScreen(top);
            if (!script::isA(screen, "HUDKeypadWindow") && !script::isA(screen, "ComputerScreenLogin") && !script::isA(screen, "ComputerScreenATM")) {
                return;
            }
            std::vector<Login> logins = knownLogins(top);
            // Do not create a footer (or reserve space for it) until a code is known.
            if (script::isA(screen, "HUDKeypadWindow") && logins.empty()) {
                return;
            }
            if (UObject* button = knownLoginButton(screen, true)) {
                bool available = !logins.empty() && !dialing && !script::flag(screen, "bWait");
                vm::Call(button, Name("EnableWindow")).b(available).run();
                std::string label = script::isA(screen, "HUDKeypadWindow") ? (logins.empty() ? "Enter Code" : "Enter " + logins.front().secret) : "Autologin";
                if (labelledButton != button || labelledText != label) {
                    labelledButton = button;
                    labelledText = label;
                    vm::Call(button, Name("SetButtonText")).s(label).run();
                }
            }
        }

        // A known door code being dialled: one key at a time, each shown pressed and sounding
        // its tone, as if typed in.
        struct Dial {
            UObject* keypad = nullptr;
            std::string code;
            size_t next = 0;
            float timer = 0;
            UObject* held = nullptr;
        };

        Dial dial;

        // Windows are deleted once their level is gone; nothing here may still point at one.
        gc::ReleaseHook releaseListener([](const gc::ReleasedSet& released) {
            // The popup is forgotten as a whole when any window it names goes, the screens its
            // logins fill in and its buttons included.
            bool popupGoes = released.count(loginPopup.window) || released.count(loginPopup.terminal) || released.count(loginPopup.cancel);
            for (const Login& login : loginPopup.logins) {
                popupGoes = popupGoes || released.count(login.screen) != 0;
            }
            for (UObject* button : loginPopup.buttons) {
                popupGoes = popupGoes || released.count(button) != 0;
            }
            if (popupGoes) {
                loginPopup = {};
            }
            if (released.count(dial.keypad) || released.count(dial.held)) {
                dial = {};
            }
            if (released.count(labelledButton)) {
                labelledButton = nullptr;
            }
            if (released.count(filledScreen)) {
                filledScreen = nullptr;
            }
        });

        constexpr float dialInterval = 0.22f;
        constexpr float dialHold = 0.12f;

        void releaseDialKey()
        {
            script::setFlag(dial.held, "bButtonPressed", false);
            dial.held = nullptr;
        }

        void stepDial(float deltaSeconds)
        {
            if (!dial.keypad) {
                return;
            }
            if (extension::topModal() != dial.keypad) {
                // Closed partway through.
                releaseDialKey();
                dial = {};
                return;
            }
            dial.timer -= deltaSeconds;
            if (dial.held && dial.timer <= dialInterval - dialHold) {
                releaseDialKey();
            }
            if (dial.timer > 0) {
                return;
            }
            if (dial.next >= dial.code.size()) {
                dial = {};
                return;
            }
            const std::string keys = "123456789*0#";
            size_t index = keys.find(dial.code[dial.next++]);
            dial.timer = dialInterval;
            if (index == std::string::npos) {
                return;
            }
            dial.held = script::object(dial.keypad, "btnKeys", static_cast<int32_t>(index));
            script::setFlag(dial.held, "bButtonPressed", true);
            // The keypad's own key handler plays the tone and checks the code after the last.
            vm::Call(dial.keypad, Name("PressButton")).i(static_cast<int32_t>(index)).run();
        }

        bool enterLogin(UObject* top)
        {
            std::vector<Login> logins = knownLogins(top);
            if (logins.empty() || script::flag(logins.front().screen, "bWait")) {
                return false;
            }
            const Login& login = logins.front();
            if (login.keypad) {
                if (dial.keypad != login.screen) {
                    // Start from an empty display, as anything typed already would spoil the code.
                    if (std::string* input = script::field<std::string>(login.screen, "inputCode")) {
                        input->clear();
                    }
                    vm::Call(login.screen, Name("GenerateKeypadDisplay")).run();
                    dial = { login.screen, login.secret, 0, 0.0f, nullptr };
                }
                return true;
            }
            return showLogins(top, logins);
        }

        // The Ice Breaker, when the player has the computer skill to use it.
        UObject* hackWindow(UObject* top)
        {
            UObject* hack = script::isA(top, "NetworkTerminal") ? script::object(top, "winHack") : nullptr;
            return script::usableButton(hack, "btnHack") ? hack : nullptr;
        }

        // An ATM has its whole balance filled in, with Withdraw ready to press, rather than
        // asking for an amount to be typed.
        void fillWithdrawal(UObject* top)
        {
            UObject* screen = computerScreen(top);
            if (!script::isA(screen, "ComputerScreenATMWithdraw")) {
                filledScreen = nullptr;
                return;
            }
            UObject* atm = script::object(screen, "atmOwner");
            UObject* edit = script::object(screen, "editWithdraw");
            if (screen == filledScreen || !atm || !edit) {
                return;
            }
            filledScreen = screen;
            int32_t user = script::flag(top, "bHacked") ? -1 : vm::Call(top, Name("GetUserIndex")).result<int32_t>();
            int32_t credits = vm::Call(atm, Name("GetBalance")).i(user).f(script::number(screen, "balanceModifier")).result<int32_t>();
            if (credits > 0) {
                setText(edit, std::to_string(credits));
                readyButton(screen, "btnWithdraw");
            }
        }
    }

    bool press(UObject* top, gamepad::Button button)
    {
        if (gamepad::triggers(gamepad::Action::Hack, button)) {
            if (UObject* hack = hackWindow(top)) {
                return script::pressButton(hack, "btnHack");
            }
        }
        return gamepad::triggers(gamepad::Action::Autologin, button) && enterLogin(top);
    }

    void addPrompts(UObject* top, std::vector<prompts::Prompt>& out)
    {
        if (UObject* hack = hackWindow(top)) {
            // Once the Ice Breaker is through, the same button goes into the computer.
            out.push_back({ gamepad::Action::Hack, script::flag(hack, "bHacked") ? "Enter" : "Hack" });
        }
        std::vector<Login> logins = knownLogins(top);
        if (!logins.empty()) {
            out.push_back({ gamepad::Action::Autologin, logins.front().keypad ? "Enter " + logins.front().secret : "Autologin" });
        }
    }

    bool buttonPressed(UObject* button)
    {
        UObject* top = extension::topModal();
        if (top && top == loginPopup.window) {
            for (size_t i = 0; i < loginPopup.buttons.size(); i++) {
                if (button == loginPopup.buttons[i]) {
                    Login login = loginPopup.logins[i];
                    UObject* terminal = loginPopup.terminal;
                    vm::Call(extension::rootWindow(), Name("PopWindow")).run();
                    loginPopup = {};
                    if (extension::topModal() == terminal && computerScreen(terminal) == login.screen) {
                        bool atm = script::isA(login.screen, "ComputerScreenATM");
                        setText(script::object(login.screen, atm ? "editAccount" : "editUserName"), login.name);
                        setText(script::object(login.screen, atm ? "editPIN" : "editPassword"), login.secret);
                        readyButton(login.screen, "btnLogin");
                        script::pressButton(login.screen, "btnLogin");
                    }
                    return true;
                }
            }
            if (button == loginPopup.cancel) {
                vm::Call(extension::rootWindow(), Name("PopWindow")).run();
                loginPopup = {};
                return true;
            }
        }
        UObject* screen = script::isA(top, "HUDKeypadWindow") ? top : computerScreen(top);
        return screen && button && button == knownLoginButton(screen, false) && enterLogin(top);
    }

    void tick(float deltaSeconds)
    {
        if (loginPopup.window && extension::topModal() != loginPopup.window) {
            loginPopup = {};
        }
        updateKnownLoginButton(extension::topModal(), dial.keypad == extension::topModal());
        stepDial(deltaSeconds);
        fillWithdrawal(extension::topModal());
    }
}
