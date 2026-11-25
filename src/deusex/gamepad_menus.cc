#include "deusex/gamepad_menus.h"

#include "core/objects.h"
#include "core/vm.h"
#include "deusex/pad_inventory.h"
#include "deusex/script.h"
#include "deusex/terminals.h"
#include "engine/input.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <array>

namespace dx::gamepadmenu {
    namespace {
        struct Action {
            gamepad::Action action;
            const char* field;
            const char* label;
        };

        struct Screen {
            const char* className;
            std::vector<Action> actions;
        };

        const std::vector<Screen>& screens()
        {
            using enum gamepad::Action;
            static const std::vector<Screen> table {
                // The medical bot's screens are persona screens' subclasses, so they come first.
                { "HUDMedBotAddAugsScreen", { { UpgradeAug, "btnInstall", "Install" } } },
                // Move, equip and drop follow gamepad focus; MenuSelect uses the selected item.
                { "PersonaScreenInventory", { { ChangeAmmo, "btnChangeAmmo", "Change Ammo" } } },
                { "PersonaScreenAugmentations", { { ActivateAug, "btnActivate", "Activate" }, { UpgradeAug, "btnUpgrade", "Upgrade" }, { UseCell, "btnUseCell", "Use Cell" } } },
                { "PersonaScreenSkills", { { UpgradeSkill, "btnUpgrade", "Upgrade" } } },
                { "MenuScreenNewGame", { { UpgradeSkill, "btnUpgrade", "Upgrade" }, { DowngradeSkill, "btnDowngrade", "Downgrade" } } },
                { "PersonaScreenHealth", { { HealAll, "btnHealAll", "Heal All" } } },
                { "PersonaScreenGoals", { { DeleteNote, "btnDeleteNote", "Delete Note" } } },
                { "PersonaScreenImages", { { DeleteNote, "btnDeleteNote", "Delete Note" } } },
            };
            return table;
        }

        // The persona screens in the order of the tabs along their top.
        constexpr std::array<const char*, 8> tabs {
            "PersonaScreenInventory",
            "PersonaScreenHealth",
            "PersonaScreenAugmentations",
            "PersonaScreenSkills",
            "PersonaScreenGoals",
            "PersonaScreenConversations",
            "PersonaScreenImages",
            "PersonaScreenLogs",
        };

        const Screen* screenOf(UObject* top)
        {
            for (const Screen& screen : screens()) {
                if (script::isA(top, screen.className)) {
                    return &screen;
                }
            }
            return nullptr;
        }

        int tabIndex(UObject* top)
        {
            for (size_t i = 0; i < tabs.size(); i++) {
                if (script::isA(top, tabs[i])) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // PreviousTab and NextTab step through the tabs, with the same steps as clicking one in
        // PersonaNavBarWindow.
        bool medBotScreen(UObject* top)
        {
            return script::isA(top, "HUDMedBotHealthScreen") || script::isA(top, "HUDMedBotAddAugsScreen");
        }

        bool switchTab(UObject* top, bool forward)
        {
            // The medical bot has two tabs of its own, Health and Augmentations, and the one
            // showing has its button turned off: either bumper presses the other.
            if (medBotScreen(top)) {
                UObject* bar = script::object(top, "winNavBar");
                return script::pressButton(bar, "btnAugs") || script::pressButton(bar, "btnHealth");
            }
            int tab = tabIndex(top);
            UObject* root = extension::rootWindow();
            if (tab < 0 || !root) {
                return false;
            }
            int count = static_cast<int>(tabs.size());
            UClass* cls = objects::findClass(std::string("DeusEx.") + tabs[static_cast<size_t>((tab + (forward ? 1 : count - 1)) % count)]);
            if (!cls) {
                return false;
            }
            vm::Call(top, Name("SaveSettings")).run();
            vm::Call(root, Name("InvokeUIScreen")).obj(cls).run();
            return true;
        }
    }

    bool press(gamepad::Button button)
    {
        UObject* top = extension::topModal();
        if (!top) {
            return false;
        }
        if (terminals::press(top, button) || padinventory::press(top, button)) {
            return true;
        }
        if (gamepad::triggers(gamepad::Action::MenuSelect, button) && script::isA(top, "MenuScreenSaveGame")
            && extension::focusWindow() == script::object(top, "editName") && script::integer(top, "editRowId") != script::integer(top, "newSaveRowID")) {
            // The editor covers the selected row. On an existing slot, Select opens
            // the game's overwrite confirmation instead of the on-screen keyboard.
            return cast<extension::UWindow>(top)->virtualKeyPressed(IK_Enter, false);
        }
        bool next = gamepad::triggers(gamepad::Action::NextTab, button);
        if (next || gamepad::triggers(gamepad::Action::PreviousTab, button)) {
            return switchTab(top, next);
        }
        if (const Screen* screen = screenOf(top)) {
            for (const Action& action : screen->actions) {
                if (gamepad::triggers(action.action, button)) {
                    script::pressButton(top, action.field);
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<prompts::Prompt> prompts()
    {
        UObject* top = extension::topModal();
        // Conversations need no prompts: A carries the talk on and picks replies.
        if (!top || script::isA(top, "ConWindowActive")) {
            return {};
        }
        if (padinventory::busy()) {
            return padinventory::busyPrompts();
        }
        std::vector<prompts::Prompt> out { { gamepad::Action::MenuSelect, padinventory::usesOnA(top) ? "Use" : "Select" }, { gamepad::Action::MenuBack, "Back" } };
        padinventory::addPrompts(top, out);
        terminals::addPrompts(top, out);
        if (const Screen* screen = padinventory::beltFocused(top) ? nullptr : screenOf(top)) {
            for (const Action& action : screen->actions) {
                if (auto* button = script::usableButton(top, action.field)) {
                    std::string label = script::string(button, "buttonText");
                    if (size_t marker = label.find("|&"); marker != std::string::npos) {
                        label.erase(marker, 2);
                    }
                    out.push_back({ action.action, label.empty() ? action.label : label });
                }
            }
        }
        if (extension::canScroll()) {
            out.push_back({ gamepad::Action::MenuScroll, "Scroll" });
        }
        if (tabIndex(top) >= 0) {
            out.push_back({ gamepad::Action::PreviousTab, "" });
            out.push_back({ gamepad::Action::NextTab, "Tabs" });
        }
        return out;
    }

    bool adjust(uint8_t key)
    {
        UObject* top = extension::topModal();
        if ((key == IK_Up || key == IK_Down) && script::isA(top, "MenuScreenSaveGame")) {
            auto* list = cast<extension::UListWindow>(script::object(top, "lstGames"));
            UObject* focus = extension::focusWindow();
            if (list && (focus == list || focus == script::object(top, "editName"))) {
                // MoveEditControl deselects the row and focuses the editor over it.
                // editRowId, not GetSelectedRow(), identifies the slot being edited.
                int index = list->indexOf(script::integer(top, "editRowId"));
                int next = index + (key == IK_Up ? -1 : 1);
                if (next >= 0 && next < static_cast<int>(list->rows.size())) {
                    list->moveToRow(next, true, true, false, false, true);
                    return true;
                }
            }
        }
        if ((key != IK_Left && key != IK_Right) || !script::isA(top, "MenuUIScreenWindow")) {
            return false;
        }
        extension::UWindow* choice = cast<extension::UWindow>(extension::focusWindow());
        while (choice && !script::isA(choice, "MenuUIChoice")) {
            choice = choice->parent();
        }
        if (!choice) {
            return false;
        }
        if (script::isA(choice, "MenuUIChoiceSlider")) {
            UObject* scale = script::object(script::object(choice, "btnSlider"), "winSlider");
            if (!scale) {
                return false;
            }
            int32_t tick = vm::Call(scale, Name("GetTickPosition")).result<int32_t>();
            int32_t ticks = vm::Call(scale, Name("GetNumTicks")).result<int32_t>();
            if ((key == IK_Left && tick <= 0) || (key == IK_Right && tick >= ticks - 1)) {
                return true;
            }
        }
        // MenuUIScreenWindow.VirtualKeyPressed cycles its current choice and clicks.
        *script::field<UObject*>(top, "currentChoice") = choice;
        return cast<extension::UWindow>(top)->virtualKeyPressed(key, false);
    }

    bool skipForNavigation(UObject* window)
    {
        auto* start = cast<extension::UWindow>(window);
        bool scale = script::isA(window, "ScaleWindow");
        for (auto* current = start; current; current = current->parent()) {
            if (script::isA(current, "PersonaNavBarBaseWindow") || (scale && script::isA(current, "MenuUIChoiceSlider"))) {
                return true;
            }
        }
        return false;
    }

    bool ownFocusLook(UObject* window)
    {
        return script::isA(window, "ConChoiceWindow");
    }
}
