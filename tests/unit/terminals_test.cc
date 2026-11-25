#include "core/config.h"
#include "test_paths.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/game_state.h"
#include "deusex/known_logins.h"
#include "deusex/script.h"
#include "deusex/startup.h"
#include "deusex/terminals.h"
#include "engine/input.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <catch_amalgamated.hpp>

#include <algorithm>

namespace {
    dx::UObject* findActor(dx::World& world, const char* name)
    {
        for (dx::UObject* actor : world.actors) {
            if (actor && !actor->destroyed && actor->name == dx::Name(name)) {
                return actor;
            }
        }
        return nullptr;
    }

    dx::extension::UButtonWindow* knownButton(dx::UObject* screen, bool keypad = false)
    {
        auto* parent = dx::cast<dx::extension::UWindow>(keypad ? screen : dx::script::object(screen, "winButtonBar"));
        for (auto* child = parent ? parent->firstChild() : nullptr; child; child = child->nextSibling()) {
            if (dx::script::object(child, "clientObject") == screen) {
                return dx::cast<dx::extension::UButtonWindow>(child);
            }
        }
        return nullptr;
    }

}

TEST_CASE("Autologin opens a picker even for one account and logs in on selection", "[terminals][data]")
{
    struct Example {
        const char* map;
        const char* machine;
        const char* source;
        bool atm;
    };

    const Example examples[] {
        { "01_NYC_UNATCOHQ", "ComputerPersonal1", "con:DL_Office", false },
        { "01_NYC_UNATCOIsland", "ComputerSecurity0", "text:01_Datacube06", false },
        { "01_NYC_UNATCOIsland", "ATM0", "text:01_Datacube07", true },
    };
    int index = GENERATE(0, 1, 2);
    const Example& example = examples[index];
    INFO(example.machine);
    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    std::string previous = dx::gamestate::save();
    dx::gamestate::restore({});
    dx::objects::unloadPackage(dx::Name(example.map));
    {
        dx::World world;
        REQUIRE(world.loadMap(example.map));
        dx::UObject* machine = findActor(world, example.machine);
        REQUIRE(machine != nullptr);
        if (example.atm) {
            dx::vm::Call(machine, dx::Name("Frob")).obj(world.player()).obj(nullptr).run();
        } else {
            dx::vm::Call(world.player(), dx::Name("InvokeComputerScreen")).obj(machine).f(-300).f(world.timeSeconds).run();
        }
        dx::UObject* top = dx::extension::topModal();
        REQUIRE(dx::script::isA(top, "NetworkTerminal"));
        dx::UObject* screen = dx::script::object(top, "winComputer");
        REQUIRE(dx::script::isA(screen, example.atm ? "ComputerScreenATM" : "ComputerScreenLogin"));
        dx::terminals::tick(0);
        auto* button = knownButton(screen);
        REQUIRE(button != nullptr);
        CHECK(button->cls == dx::script::object(screen, "btnLogin")->cls);
        CHECK(dx::script::string(button, "buttonText") == "Autologin");
        CHECK_FALSE(button->sensitive());
        dx::UObject* bar = dx::script::object(screen, "winButtonBar");
        CHECK(dx::script::integer(bar, "buttonCount") == 3);
        dx::terminals::tick(0);
        CHECK(knownButton(screen) == button);
        CHECK(dx::script::integer(bar, "buttonCount") == 3);
        dx::knownlogins::markRead(example.source);
        dx::terminals::tick(0);
        REQUIRE(button->sensitive());
        CHECK(button->selectable());
        std::vector<dx::UObject*> cycle;
        dx::UObject* first = dx::extension::focusWindow();
        bool sawAction = false;
        bool sawAutologin = false;
        for (int i = 0; i < 20; i++) {
            dx::extension::keyEvent(dx::IK_Tab, dx::IST_Press, 0);
            dx::extension::keyEvent(dx::IK_Tab, dx::IST_Release, 0);
            dx::UObject* focused = dx::extension::focusWindow();
            REQUIRE(focused != nullptr);
            sawAutologin = sawAutologin || focused == button;
            sawAction = sawAction || dx::script::isA(focused, "MenuUIActionButtonWindow");
            cycle.push_back(focused);
            if (cycle.size() > 1 && focused == first) {
                break;
            }
        }
        CHECK(sawAction);
        CHECK(sawAutologin);
        CHECK(cycle.size() >= 3);
        auto* root = dx::cast<dx::extension::URootWindow>(dx::extension::rootWindow());
        REQUIRE(root != nullptr);
        auto candidates = root->navigationCandidates();
        CHECK(std::find(candidates.begin(), candidates.end(), button) != candidates.end());
        auto accounts = dx::knownlogins::knownAccounts(example.map, example.machine);
        REQUIRE_FALSE(accounts.empty());
        std::string account;
        std::string secret;
        int32_t users = dx::vm::Call(machine, dx::Name("NumUsers")).result<int32_t>();
        for (int32_t i = 0; i < users; i++) {
            std::string candidate = dx::vm::Call(machine, dx::Name(example.atm ? "GetAccountNumber" : "GetUserName")).i(i).result<std::string>();
            if (dx::iequals(candidate, accounts.front())) {
                account = candidate;
                secret = dx::vm::Call(machine, dx::Name(example.atm ? "GetPIN" : "GetPassword")).i(i).result<std::string>();
                break;
            }
        }
        REQUIRE_FALSE(account.empty());
        REQUIRE_FALSE(secret.empty());
        dx::extension::setButtonHook(dx::terminals::buttonPressed);
        button->activate(dx::IK_Enter);
        dx::UObject* popup = dx::extension::topModal();
        REQUIRE(popup != top);
        auto* panel = dx::cast<dx::UTexture>(dx::script::object(dx::script::object(popup, "winClient"), "background"));
        REQUIRE(panel != nullptr);
        CHECK(panel->mips[0].height == 41);
        auto* choice = dx::cast<dx::extension::UButtonWindow>(dx::extension::focusWindow());
        REQUIRE(choice != nullptr);
        CHECK(dx::script::string(choice, "buttonText") == account + " / " + secret);
        dx::extension::keyEvent(dx::IK_Enter, dx::IST_Press, 0);
        dx::extension::keyEvent(dx::IK_Enter, dx::IST_Release, 0);
        if (example.atm) {
            CHECK(dx::vm::Call(machine, dx::Name("GetAccountNumber")).i(dx::script::integer(top, "userIndex")).result<std::string>() == account);
        } else {
            CHECK(dx::iequals(dx::script::string(top, "userName"), account));
        }
        CHECK(dx::script::object(top, "winComputer") != screen);
        CHECK(dx::extension::topModal() == top);
        if (index == 1) {
            auto* security = dx::script::object(top, "winComputer");
            auto* slider = dx::script::object(dx::script::object(security, "winPanSlider"), "winSlider");
            REQUIRE(slider);
            root->updateLayout();
            REQUIRE(root->setRootFocusWindow(dx::cast<dx::extension::UWindow>(slider)));
            int tick = dx::scalewindow::currentPos(slider);
            REQUIRE(dx::extension::navigate(dx::IK_Right));
            CHECK(dx::scalewindow::currentPos(slider) > tick);
            REQUIRE(dx::extension::navigate(dx::IK_Left));
            CHECK(dx::scalewindow::currentPos(slider) == tick);
            for (auto key : { dx::IK_Up, dx::IK_Down }) {
                REQUIRE(root->setRootFocusWindow(dx::cast<dx::extension::UWindow>(slider)));
                REQUIRE(dx::extension::navigate(key));
                auto* expected = key == dx::IK_Up ? dx::script::object(security, "btnPanDown")
                    : dx::script::object(dx::script::object(security, "winCameras", 1), "btnCamera");
                REQUIRE(expected);
                CHECK(root->focusWindow() == expected);
            }
        }
        dx::extension::setButtonHook({});
    }
    dx::objects::unloadPackage(dx::Name(example.map));
    dx::gamestate::restore(previous);
}

TEST_CASE("Autologin lists every known account and can be cancelled", "[terminals][data]")
{
    int chosen = GENERATE(0, 1, 5);
    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    std::string previous = dx::gamestate::save();
    dx::gamestate::restore({});
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOHQ"));
        dx::UObject* machine = findActor(world, "ComputerPersonal1");
        REQUIRE(machine != nullptr);
        dx::vm::Call(world.player(), dx::Name("InvokeComputerScreen")).obj(machine).f(-300).f(world.timeSeconds).run();
        dx::UObject* screen = dx::script::object(dx::extension::topModal(), "winComputer");
        REQUIRE(dx::script::isA(screen, "ComputerScreenLogin"));
        // Two accounts from different sources, in the table's order: JCD then jmanderley.
        dx::knownlogins::markRead("con:DL_Office");
        dx::knownlogins::markRead("text:01_Datacube01");
        REQUIRE(dx::knownlogins::knownAccounts("01_NYC_UNATCOHQ", "ComputerPersonal1") == std::vector<std::string> { "JCD", "jmanderley" });
        if (chosen == 5) {
            dx::knownlogins::markRead("text:05_Datacube08");
        }
        dx::terminals::tick(0);
        auto* button = knownButton(screen);
        REQUIRE(button != nullptr);
        REQUIRE(button->sensitive());
        CHECK(dx::script::string(button, "buttonText") == "Autologin");
        dx::UObject* terminal = dx::extension::topModal();
        REQUIRE(dx::terminals::press(terminal, dx::gamepad::X));
        dx::extension::keyEvent(dx::IK_Escape, dx::IST_Press, 0);
        dx::extension::keyEvent(dx::IK_Escape, dx::IST_Release, 0);
        CHECK(dx::extension::topModal() == terminal);
        CHECK(dx::script::object(terminal, "winComputer") == screen);
        auto fill = [&](const char* account, int index) {
            dx::extension::setButtonHook(dx::terminals::buttonPressed);
            button->activate(dx::IK_Enter);
            auto* popup = dx::extension::topModal();
            auto* client = dx::cast<dx::extension::UWindow>(dx::script::object(popup, "winClient"));
            REQUIRE(client != nullptr);
            std::vector<dx::extension::UButtonWindow*> choices;
            for (auto* child = client->firstChild(); child; child = child->nextSibling()) {
                if (auto* choice = dx::cast<dx::extension::UButtonWindow>(child)) {
                    choices.push_back(choice);
                }
            }
            REQUIRE(choices.size() == (chosen == 5 ? 6 : 2));
            auto* panel = dx::cast<dx::UTexture>(dx::script::object(client, "background"));
            REQUIRE(panel != nullptr);
            CHECK(panel->mips[0].height == 16 + 25 * choices.size());
            choices[static_cast<size_t>(index)]->activate(dx::IK_Enter);
            dx::extension::setButtonHook({});
            CHECK(dx::iequals(dx::script::string(terminal, "userName"), account));
            CHECK(dx::script::object(terminal, "winComputer") != screen);
        };
        fill(chosen == 0 ? "JCD" : (chosen == 1 ? "jmanderley" : "jreyes"), chosen);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    dx::gamestate::restore(previous);
}

TEST_CASE("Enter Code displays and dials the learned keypad code", "[terminals][data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::config::system().set("Engine.Engine", "Root", "DeusEx.DeusExRootWindow");
    std::string previous = dx::gamestate::save();
    dx::gamestate::restore({});
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* keypad = findActor(world, "Keypad0");
        REQUIRE(keypad != nullptr);
        dx::vm::Call(keypad, dx::Name("Frob")).obj(world.player()).obj(nullptr).run();
        dx::UObject* screen = dx::extension::topModal();
        REQUIRE(dx::script::isA(screen, "HUDKeypadWindow"));
        // Without a render pass the window keeps its pre-layout size; the game lays it out
        // before the native footer is added.
        auto* keypadWindow = dx::cast<dx::extension::UWindow>(screen);
        REQUIRE(keypadWindow != nullptr);
        keypadWindow->width() = 103;
        keypadWindow->height() = 162;
        dx::terminals::tick(0);
        auto* button = knownButton(screen, true);
        CHECK(button == nullptr);
        float height = dx::cast<dx::extension::UWindow>(screen)->height();
        dx::terminals::tick(0);
        CHECK(knownButton(screen, true) == nullptr);
        CHECK(dx::cast<dx::extension::UWindow>(screen)->height() == height);
        dx::knownlogins::markRead("text:01_Datacube03");
        dx::terminals::tick(0);
        button = knownButton(screen, true);
        REQUIRE(button != nullptr);
        REQUIRE(button->sensitive());
        CHECK(dx::script::string(button, "buttonText") == "Enter " + dx::script::string(keypad, "validCode"));
        // F1 stays with the game's own data vault shortcuts; only the button or pad X dials.
        dx::terminals::tick(0);
        CHECK(dx::script::string(screen, "inputCode").empty());
        *dx::script::field<std::string>(screen, "inputCode") = "99";
        dx::extension::setButtonHook(dx::terminals::buttonPressed);
        button->activate(dx::IK_Enter);
        REQUIRE(dx::extension::topModal() == screen);
        dx::terminals::tick(0);
        std::string code = dx::script::string(keypad, "validCode");
        REQUIRE_FALSE(code.empty());
        CHECK(dx::script::string(screen, "inputCode") == code.substr(0, 1));
        CHECK_FALSE(button->sensitive());
        for (size_t i = 1; i < code.size(); i++) {
            dx::terminals::tick(0.22f);
        }
        CHECK(dx::script::string(screen, "inputCode") == code);
        CHECK(dx::script::flag(screen, "bWait"));
        dx::extension::setButtonHook({});
        // Closing mid-animation must not leave a pressed digit on an unloaded screen.
        dx::vm::Call(dx::extension::rootWindow(), dx::Name("PopWindow")).run();
        dx::terminals::tick(0);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    dx::gamestate::restore(previous);
}
