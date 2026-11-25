#pragma once

#include "core/objects.h"
#include "core/vm.h"
#include "deusex/gamepad_layout.h"
#include "deusex/references.h"
#include "deusex/script.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <string>

namespace dx::test {
    // Opens a screen with the Controller Layout row in it, as the game's Gamepad screen has (that
    // screen is the game's to make; padlayout::install makes the row), and gives back the row's
    // button, or null.
    inline UObject* openLayoutRow()
    {
        UObject* root = extension::rootWindow();
        UClass* row = objects::findClass(std::string("DeusEx.") + padlayout::layoutChoice);
        UClass* options = objects::findClass("DeusEx.MenuScreenOptions");
        if (!root || !row || !options) {
            return nullptr;
        }
        // Held, as the game's own screens are by the menus that open them, so it lasts through a
        // level change as every class made at run time must (gc_level_test).
        static deusex::HeldObject<UClass> held;
        UClass* screen = objects::findClass("DeusEx.MenuScreenLayoutRowTest");
        if (!screen) {
            screen = objects::createSubclass(options, Name("MenuScreenLayoutRowTest"));
            UProperty* choices = screen->findProperty(Name("choices"));
            for (int i = 0; i < choices->arrayDim; i++) {
                *reinterpret_cast<UObject**>(screen->defaultObject->propertyAddress(Name("choices"), i)) = i == 0 ? row : nullptr;
            }
        }
        held.object = screen;
        vm::Call(root, Name("InvokeMenuScreen")).obj(screen).run();
        auto* client = cast<extension::UWindow>(script::object(extension::topModal(), "winClient"));
        for (auto* choice = client ? client->firstChild() : nullptr; choice; choice = choice->nextSibling()) {
            if (script::isA(choice, padlayout::layoutChoice)) {
                return script::object(choice, "btnAction");
            }
        }
        return nullptr;
    }
}
