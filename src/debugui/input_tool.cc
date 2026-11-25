#include "debugui/input_tool.h"
#include "debugui/imgui_setup.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <utility>

namespace dx::debugui {
    namespace {
        const char* typeName(SDL_GamepadType type)
        {
            const char* name = SDL_GetGamepadStringForType(type);
            return name ? name : "unknown";
        }

        // A stick or trigger value as a bar from -1 to 1 (sticks) or 0 to 1 (triggers).
        void axisBar(const char* label, float value, bool trigger)
        {
            float shown = trigger ? value : (value + 1.0f) * 0.5f;
            char text[32];
            SDL_snprintf(text, sizeof(text), "%+.3f", static_cast<double>(value));
            ImGui::ProgressBar(shown, ImVec2(scaled(180), 0), text);
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        }

        void pad(const InputState::Pad& entry)
        {
            SDL_Gamepad* device = entry.pad;
            const char* name = SDL_GetGamepadName(device);
            std::string title = std::string(name ? name : "(no name)") + "##" + std::to_string(SDL_GetGamepadID(device));
            if (!ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            ImGui::Text("Id %u; SDL type %s (real type %s); prompt style %s", static_cast<unsigned>(SDL_GetGamepadID(device)), typeName(SDL_GetGamepadType(device)), typeName(SDL_GetRealGamepadType(device)), entry.promptStyle.c_str());
            ImGui::Text("%s; %s", entry.inPlay ? "in play" : "not in play", entry.prompts ? "the prompts follow it" : "the prompts follow another pad");
            ImGui::TextUnformatted("Buttons held:");
            bool any = false;
            for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
                if (SDL_GetGamepadButton(device, static_cast<SDL_GamepadButton>(button))) {
                    const char* buttonName = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(button));
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", buttonName ? buttonName : "?");
                    any = true;
                }
            }
            if (!any) {
                ImGui::SameLine();
                ImGui::TextDisabled("none");
            }
            for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; axis++) {
                SDL_GamepadAxis which = static_cast<SDL_GamepadAxis>(axis);
                float value = static_cast<float>(SDL_GetGamepadAxis(device, which)) / 32767.0f;
                const char* axisName = SDL_GetGamepadStringForAxis(which);
                axisBar(axisName ? axisName : "?", value, which == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || which == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            }
            if (!SDL_GamepadHasSensor(device, SDL_SENSOR_GYRO)) {
                ImGui::TextDisabled("No gyro sensor");
            } else if (!SDL_GamepadSensorEnabled(device, SDL_SENSOR_GYRO)) {
                ImGui::TextDisabled("Gyro sensor present, off (the game turns it on with gyro aiming)");
            } else {
                float data[3] = {};
                if (SDL_GetGamepadSensorData(device, SDL_SENSOR_GYRO, data, 3)) {
                    ImGui::Text("Gyro: %+.3f %+.3f %+.3f rad/s (pitch, yaw, roll)", static_cast<double>(data[0]), static_cast<double>(data[1]), static_cast<double>(data[2]));
                }
            }
        }

        void draw(const InputState& state)
        {
            ImGui::Text("Last used: %s; prompts drawn: %s", state.padLastUsed ? "the pad" : "the keyboard and mouse", state.shownStyle.c_str());
            if (state.gyroRead) {
                ImGui::Text("Gyro aiming %s; last rate from the pad in play %+.3f pitch, %+.3f yaw, %+.3f roll rad/s", state.gyroAiming ? "on" : "off", static_cast<double>(state.gyroPitch), static_cast<double>(state.gyroYaw), static_cast<double>(state.gyroRoll));
            } else {
                ImGui::Text("Gyro aiming %s; no rate read yet", state.gyroAiming ? "on" : "off");
            }
            // Held level, down is about (0, -1, 0); tipped up toward the player, z grows. If it
            // stays at (0, -1, 0) however the pad is held, no accelerometer readings arrive.
            ImGui::Text("Down in the pad's axes: %+.2f %+.2f %+.2f", static_cast<double>(state.down[0]), static_cast<double>(state.down[1]), static_cast<double>(state.down[2]));
            ImGui::Text("Drift taken off: %+.4f %+.4f %+.4f rad/s (pitch, yaw, roll); calibration %s", static_cast<double>(state.drift[0]), static_cast<double>(state.drift[1]), static_cast<double>(state.drift[2]), state.calibration.c_str());
            ImGui::Text("Gyro kept back to pay out evenly: %.1f ms", static_cast<double>(state.gyroKeptBack) * 1000.0);
            if (state.touches.empty()) {
                ImGui::TextDisabled("Touch: no fingers reported");
            } else {
                for (const InputState::Touch& touch : state.touches) {
                    ImGui::Text("Touch %lld at %.0f, %.0f", static_cast<long long>(touch.id), static_cast<double>(touch.x), static_cast<double>(touch.y));
                }
            }
            ImGui::Separator();
            if (state.pads.empty()) {
                ImGui::TextDisabled("No gamepads open.");
            }
            for (const InputState::Pad& entry : state.pads) {
                pad(entry);
            }
        }
    }

    void addInputTool(Overlay& overlay, std::function<InputState()> read)
    {
        auto reader = std::make_shared<std::function<InputState()>>(std::move(read));
        Tool tool;
        tool.menu = "Game";
        tool.title = "Input";
        tool.name = "input";
        tool.width = 640;
        tool.height = 620;
        tool.draw = [reader](const Frame&) {
            draw((*reader)());
        };
        overlay.addTool(std::move(tool));
    }
}
