#include "deusex/hud.h"

#include "core/vm.h"
#include "deusex/script.h"
#include "extension/extension.h"
#include "extension/windows.h"

namespace dx::hud {
    namespace {
        UObject* hudWindow()
        {
            return script::object(extension::rootWindow(), "hud");
        }
    }

    UObject* viewer()
    {
        return script::object(extension::rootWindow(), "parentPawn");
    }

    bool shown()
    {
        auto* hud = cast<extension::UWindow>(hudWindow());
        return hud && hud->visible();
    }

    bool atTitleMap()
    {
        UObject* root = extension::rootWindow();
        return root && vm::Call(root, Name("AtIntroMap")).resultBool();
    }

    void layout(bool gamepadMode)
    {
        static bool hidden = false;
        UObject* hud = hudWindow();
        if (!hud) {
            return;
        }
        if (gamepadMode) {
            for (const char* field : { "belt", "activeItems" }) {
                auto* bar = cast<extension::UWindow>(script::object(hud, field));
                if (bar && bar->visible()) {
                    bar->show(false);
                }
            }
            hidden = true;
        } else if (hidden) {
            // Back to the player's own choices for which HUD parts show.
            hidden = false;
            if (UObject* player = viewer()) {
                vm::Call(hud, Name("UpdateSettings")).obj(player).run();
            }
        }
    }
}
