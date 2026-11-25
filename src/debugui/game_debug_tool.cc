#include "debugui/game_debug_tool.h"
#include "debugui/imgui_setup.h"

#include "core/objects.h"
#include "core/vm.h"

#include <imgui.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace dx::debugui {
    namespace {
        // The original game's own debugging: everything here sends a console command a player
        // could type, and every tick box reads back what the script holds rather than keeping a
        // copy, so it shows what the console did too.
        struct GameDebug {
            Overlay* overlay = nullptr;
            char viewClass[128] = "ScriptedPawn";
            char summon[128] = "Cat";
            char event[128] = "";
            char state[64] = "Wandering";
            std::string lastCommand;
            std::string lastReply;

            void run(const std::string& command)
            {
                lastCommand = command;
                lastReply.clear();
                overlay->runAfterFrame(command, [this](const std::string& reply) {
                    lastReply = reply;
                });
            }
        };

        UObject* objectProperty(UObject* object, const char* name)
        {
            if (!object || object->destroyed || !object->data) {
                return nullptr;
            }
            UProperty* property = object->cls ? object->cls->findProperty(Name(name)) : nullptr;
            if (!property || property->type != PropType::Object) {
                return nullptr;
            }
            return *reinterpret_cast<UObject**>(object->data + property->offset);
        }

        // A script variable as the console's get shows it, or empty when there is none.
        std::string propertyText(UObject* object, const char* name)
        {
            if (!object || object->destroyed || !object->data || !object->cls) {
                return {};
            }
            UProperty* property = object->cls->findProperty(Name(name));
            return property ? property->toText(object->data + property->offset) : std::string();
        }

        bool propertyTrue(UObject* object, const char* name)
        {
            return propertyText(object, name) == "True";
        }

        // Calls a script function that takes nothing; false when the object has no such function.
        bool callScript(UObject* object, const char* name, vm::Call*& call, std::unique_ptr<vm::Call>& holder)
        {
            if (!object || object->destroyed || !vm::findFunction(object, Name(name))) {
                return false;
            }
            holder = std::make_unique<vm::Call>(object, Name(name));
            if (!holder->valid()) {
                return false;
            }
            holder->run();
            call = holder.get();
            return call->returnValue() != nullptr;
        }

        // One of the actor display's Is...Visible functions; nothing when it cannot be asked.
        int askVisible(UObject* display, const char* function)
        {
            std::unique_ptr<vm::Call> holder;
            vm::Call* call = nullptr;
            if (!callScript(display, function, call, holder)) {
                return -1;
            }
            return *reinterpret_cast<uint32_t*>(call->returnValue()) != 0 ? 1 : 0;
        }

        std::string viewClassName(UObject* display)
        {
            std::unique_ptr<vm::Call> holder;
            vm::Call* call = nullptr;
            if (!callScript(display, "GetViewClass", call, holder)) {
                return "?";
            }
            UObject* cls = *reinterpret_cast<UObject**>(call->returnValue());
            return cls ? cls->name.str() : std::string("None");
        }

        void commandButton(GameDebug& tool, const char* label, const std::string& command)
        {
            if (ImGui::Button(label)) {
                tool.run(command);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", command.c_str());
            }
        }

        // A tick box that shows `on` and, when clicked, runs the command for the other value.
        void commandBox(GameDebug& tool, const char* label, int on, const std::string& command)
        {
            bool value = on == 1;
            ImGui::BeginDisabled(on < 0);
            if (ImGui::Checkbox(label, &value)) {
                tool.run(command + (value ? " True" : " False"));
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s %s", command.c_str(), value ? "False" : "True");
            }
        }

        struct DisplaySwitch {
            const char* label;
            // The DeusExPlayer exec function that sets it.
            const char* command;
            // The ActorDisplayWindow function that says whether it is on.
            const char* query;
        };

        constexpr DisplaySwitch displaySwitches[] = {
            { "Eyes", "ShowEyes", "AreEyesVisible" },
            { "Area", "ShowArea", "IsAreaVisible" },
            { "Cylinder", "ShowCylinder", "IsCylinderVisible" },
            { "Mesh", "ShowMesh", "IsMeshVisible" },
            { "Zone", "ShowZone", "IsZoneVisible" },
            { "Line of sight", "ShowLOS", "IsLOSVisible" },
            { "Visibility", "ShowVisibility", "IsVisibilityVisible" },
            { "State", "ShowState", "IsStateVisible" },
            { "Enemy", "ShowEnemy", "IsEnemyVisible" },
            { "Enemy response", "ShowEnemyResponse", "IsEnemyResponseVisible" },
            { "Instigator", "ShowInstigator", "IsInstigatorVisible" },
            { "Base", "ShowBase", "IsBaseVisible" },
            { "Bind name", "ShowBindName", "IsBindNameVisible" },
            { "Light", "ShowLight", "IsLightVisible" },
            { "Distance", "ShowDist", "IsDistVisible" },
            { "Position", "ShowPos", "IsPosVisible" },
            { "Health", "ShowHealth", "IsHealthVisible" },
            { "Mass", "ShowMass", "IsMassVisible" },
            { "Physics", "ShowPhysics", "ArePhysicsVisible" },
            { "Velocity", "ShowVelocity", "IsVelocityVisible" },
            { "Acceleration", "ShowAcceleration", "IsAccelerationVisible" },
        };

        void cheats(GameDebug& tool, UObject* player, bool enabled)
        {
            // As a player turns them on: the console's set, on the player's own class.
            std::string className = player->cls->outer ? player->cls->outer->name.str() + "." + player->cls->name.str() : player->cls->name.str();
            ImGui::TextColored(enabled ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Cheats are %s", enabled ? "on" : "off");
            ImGui::SameLine();
            commandButton(tool, enabled ? "Turn cheats off" : "Turn cheats on", "set " + className + " bCheatsEnabled " + (enabled ? "False" : "True"));
            if (!enabled) {
                helpMarker("Most of the commands below do nothing until these are on, as in the original.");
            }
        }

        void actorDisplay(GameDebug& tool, UObject* display)
        {
            if (!display) {
                ImGui::TextDisabled("No actor display: the root window is not Deus Ex's.");
                return;
            }
            std::string shown = viewClassName(display);
            ImGui::Text("Showing: %s", shown.c_str());
            ImGui::SetNextItemWidth(scaled(220));
            bool entered = ImGui::InputText("Class", tool.viewClass, sizeof(tool.viewClass), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Show class") || entered) {
                tool.run(std::string("ShowClass ") + tool.viewClass);
            }
            ImGui::SameLine();
            commandButton(tool, "Show none", "ShowClass None");
            // ShowData has no Is...Visible function; its variable is what the script reads.
            commandBox(tool, "Data (everything below)", propertyTrue(display, "bShowData") ? 1 : 0, "ShowData");
            if (ImGui::BeginTable("switches", 4, ImGuiTableFlags_SizingStretchSame)) {
                for (const DisplaySwitch& entry : displaySwitches) {
                    ImGui::TableNextColumn();
                    commandBox(tool, entry.label, askVisible(display, entry.query), entry.command);
                }
                ImGui::EndTable();
            }
        }

        void toolWindows(GameDebug& tool, UObject* root)
        {
            commandButton(tool, "Behind the curtain", "Legend");
            ImGui::SameLine();
            commandButton(tool, "Edit flags", "EditFlags");
            ImGui::SameLine();
            commandButton(tool, "Load map", "LoadMap");
            ImGui::SameLine();
            commandButton(tool, "Invoke conversation", "InvokeConWindow");
            helpMarker("The game's windows take the mouse once the overlay is passive (Overlay menu).");
            commandBox(tool, "Window statistics", root ? (propertyTrue(root, "bShowStats") ? 1 : 0) : -1, "WinStats");
            ImGui::SameLine();
            commandBox(tool, "Window frames", root ? (propertyTrue(root, "bShowFrames") ? 1 : 0) : -1, "WinFrames");
        }

        void playerCheats(GameDebug& tool, UObject* player)
        {
            ImGui::Text("Player state: %s, physics %s", vm::stateName(player).c_str(), propertyText(player, "Physics").c_str());
            bool god = propertyText(player, "ReducedDamageType") == "All";
            if (ImGui::Checkbox("God", &god)) {
                tool.run("God");
            }
            ImGui::SameLine();
            commandBox(tool, "Behind view", propertyTrue(player, "bBehindView") ? 1 : 0, "BehindView");
            ImGui::SameLine();
            commandButton(tool, "Walk", "Walk");
            ImGui::SameLine();
            commandButton(tool, "Fly", "Fly");
            ImGui::SameLine();
            commandButton(tool, "Ghost", "Ghost");
            ImGui::SameLine();
            commandButton(tool, "Warren's EMP field", "IAmWarren");

            ImGui::TextUnformatted("At what the player looks at:");
            ImGui::SameLine();
            commandButton(tool, "Tantalus", "Tantalus");
            ImGui::SameLine();
            commandButton(tool, "Open sesame", "OpenSesame");
            ImGui::SameLine();
            commandButton(tool, "Where actor", "WhereActor");
            ImGui::SameLine();
            commandButton(tool, "Where am I", "WhereActor 1");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(scaled(120));
            ImGui::InputText("##state", tool.state, sizeof(tool.state));
            ImGui::SameLine();
            commandButton(tool, "Set state", std::string("SetState ") + tool.state);

            static const char* const gifts[] = { "AllHealth", "AllEnergy", "AllCredits", "AllSkills", "AllSkillPoints", "AllAugs", "AllWeapons", "AllAmmo", "AllImages" };
            bool first = true;
            for (const char* gift : gifts) {
                if (!first) {
                    ImGui::SameLine();
                }
                first = false;
                commandButton(tool, gift, gift);
            }
            commandButton(tool, "Dump player info to the log", "DXDumpInfo");

            ImGui::SetNextItemWidth(scaled(220));
            ImGui::InputText("##summon", tool.summon, sizeof(tool.summon));
            ImGui::SameLine();
            commandButton(tool, "Summon", std::string("Summon ") + tool.summon);
            ImGui::SetNextItemWidth(scaled(220));
            ImGui::InputText("##event", tool.event, sizeof(tool.event));
            ImGui::SameLine();
            commandButton(tool, "Trigger event", std::string("Trig ") + tool.event);
            ImGui::SameLine();
            commandButton(tool, "Untrigger event", std::string("UnTrig ") + tool.event);
        }

        void draw(GameDebug& tool, const Frame& frame)
        {
            UObject* player = frame.player;
            if (!player || player->destroyed || !player->data || !player->cls) {
                ImGui::TextDisabled("No player.");
                return;
            }
            bool enabled = propertyTrue(player, "bCheatsEnabled");
            cheats(tool, player, enabled);
            UObject* root = objectProperty(player, "RootWindow");
            UObject* display = objectProperty(root, "actorDisplay");
            if (ImGui::CollapsingHeader("Actor display", ImGuiTreeNodeFlags_DefaultOpen)) {
                actorDisplay(tool, display);
            }
            if (ImGui::CollapsingHeader("Tool windows", ImGuiTreeNodeFlags_DefaultOpen)) {
                toolWindows(tool, root);
            }
            if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
                playerCheats(tool, player);
            }
            ImGui::Separator();
            if (tool.lastCommand.empty()) {
                ImGui::TextDisabled("Hover a control to see the console command it sends.");
            } else {
                ImGui::Text("Sent: %s", tool.lastCommand.c_str());
                if (!tool.lastReply.empty()) {
                    ImGui::TextWrapped("Reply: %s", tool.lastReply.c_str());
                }
            }
        }
    }

    void addGameDebugTool(Overlay& overlay)
    {
        auto tool = std::make_shared<GameDebug>();
        tool->overlay = &overlay;
        Tool entry;
        entry.menu = "Game";
        entry.title = "Game debug";
        entry.name = "gamedebug";
        entry.width = 840;
        entry.height = 560;
        entry.draw = [tool](const Frame& frame) {
            draw(*tool, frame);
        };
        overlay.addTool(std::move(entry));
    }
}
