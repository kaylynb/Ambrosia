#include "deusex/reading.h"

#include "core/vm.h"
#include "deusex/hud.h"
#include "deusex/known_logins.h"
#include "deusex/script.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "extension/windows.h"

#include <cstdlib>
#include <string>

namespace dx::reading {
    namespace {
        int32_t selectedRow(UObject* list)
        {
            return list ? vm::Call(list, Name("GetSelectedRow")).result<int32_t>(-1) : -1;
        }

        void computerScreen(UObject* top)
        {
            UObject* screen = script::isA(top, "NetworkTerminal") ? script::object(top, "winComputer") : nullptr;
            if (script::isA(screen, "ComputerScreenEmail")) {
                UObject* list = script::object(screen, "lstEmail");
                int32_t row = selectedRow(list);
                if (row >= 0) {
                    // The row's third column holds the email's index.
                    std::string index = vm::Call(list, Name("GetField")).i(row).i(2).result<std::string>();
                    if (!index.empty()) {
                        knownlogins::markRead("text:" + script::member(screen, "emailInfo", std::atoi(index.c_str()), "emailName"));
                    }
                }
            } else if (script::isA(screen, "ComputerScreenBulletins")) {
                UObject* list = script::object(screen, "lstBulletins");
                int32_t row = selectedRow(list);
                if (row >= 0) {
                    int32_t index = vm::Call(list, Name("RowIdToIndex")).i(row).result<int32_t>(-1);
                    knownlogins::markRead("text:" + script::member(screen, "fileInfo", index, "fileName"));
                }
            }
        }

        void conversations()
        {
            UObject* player = hud::viewer();
            for (const char* field : { "conPlay", "dataLinkPlay" }) {
                std::string name = script::text(script::object(script::object(player, field), "con"), "conName");
                if (!name.empty() && name != "None") {
                    knownlogins::markRead("con:" + name);
                }
            }
        }

        // A datacube, book or newspaper shows in the HUD's information window; the device
        // showing it is found once each time the window opens.
        void informationDevices()
        {
            static bool wasShown = false;
            auto* info = cast<extension::UWindow>(script::object(script::object(extension::rootWindow(), "hud"), "info"));
            bool shown = info && info->visible();
            if (shown && !wasShown && World::hasActive()) {
                for (UObject* actor : World::active().actors) {
                    if (actor && !actor->destroyed && script::object(actor, "infoWindow") == info && script::isA(actor, "InformationDevices")) {
                        knownlogins::markRead("text:" + script::text(actor, "textTag"));
                    }
                }
            }
            wasShown = shown;
        }
    }

    void tick()
    {
        computerScreen(extension::topModal());
        conversations();
        informationDevices();
    }
}
