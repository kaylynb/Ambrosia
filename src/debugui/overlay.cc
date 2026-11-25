#include "debugui/overlay.h"

#include "debugui/imgui_setup.h"
#include "engine/console.h"

#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace dx::debugui {
    Overlay::Overlay(SDL_Window* target)
        : window(target)
    {
        addCommand("open", [this](const std::string& value) {
            return openCommand(value, true);
        });
        addCommand("close", [this](const std::string& value) {
            return openCommand(value, false);
        });
        addCommand("scale", [this](const std::string& value) {
            return scaleCommand(value);
        });
    }

    Overlay::~Overlay()
    {
        if (started) {
            // Saves the settings file, which still reads the tools.
            stopImGui();
        }
    }

    void Overlay::addTool(Tool tool)
    {
        ToolEntry entry;
        entry.open = tool.openAtFirst;
        entry.tool = std::move(tool);
        tools.push_back(std::move(entry));
    }

    Overlay::ToolEntry* Overlay::findTool(const std::string& name)
    {
        for (ToolEntry& entry : tools) {
            if (entry.tool.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    void Overlay::setOpen(ToolEntry& entry, bool open)
    {
        if (!started) {
            entry.chosen = true;
        }
        if (entry.open == open) {
            return;
        }
        entry.open = open;
        if (!open && entry.tool.closed) {
            entry.tool.closed();
        }
        if (started) {
            ImGui::MarkIniSettingsDirty();
        }
    }

    bool Overlay::setToolOpen(const std::string& name, bool open)
    {
        ToolEntry* entry = findTool(name);
        if (!entry) {
            return false;
        }
        setOpen(*entry, open);
        return true;
    }

    bool Overlay::isToolOpen(const std::string& name) const
    {
        for (const ToolEntry& entry : tools) {
            if (entry.tool.name == name) {
                return entry.open;
            }
        }
        return false;
    }

    std::vector<std::string> Overlay::toolNames() const
    {
        std::vector<std::string> names;
        for (const ToolEntry& entry : tools) {
            names.push_back(entry.tool.name);
        }
        return names;
    }

    std::string Overlay::openCommand(const std::string& value, bool open)
    {
        std::string done = open ? "opened" : "closed";
        std::string hidden = current == Mode::Hidden ? " (the overlay is hidden; DebugOverlay shows it)" : "";
        if (value == "all") {
            for (ToolEntry& entry : tools) {
                setOpen(entry, open);
            }
            return done + " every tool" + hidden;
        }
        if (setToolOpen(value, open)) {
            return done + " " + value + hidden;
        }
        std::string names;
        for (const std::string& name : toolNames()) {
            names += " " + name;
        }
        return std::string("DebugOverlay ") + (open ? "open" : "close") + " <tool>, with all or a tool from:" + names;
    }

    std::string Overlay::scaleCommand(const std::string& value)
    {
        if (value == "auto") {
            scaleChoice = 0;
        } else {
            char* end = nullptr;
            float chosen = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || chosen < 0.5f || chosen > 4.0f) {
                return "DebugOverlay scale <auto, or 0.5 to 4>";
            }
            scaleChoice = chosen;
        }
        if (started) {
            ImGui::MarkIniSettingsDirty();
        }
        return scaleChoice > 0 ? "overlay scale " + value : "overlay scale auto";
    }

    void Overlay::setSettingsFile(std::string path)
    {
        settingsFile = std::move(path);
    }

    void Overlay::onClose(std::function<void()> closed)
    {
        closers.push_back(std::move(closed));
    }

    void Overlay::addSwitch(std::string name, std::function<void(bool)> set)
    {
        switches.emplace_back(std::move(name), std::move(set));
    }

    bool Overlay::setSwitch(const std::string& name, bool on)
    {
        for (const auto& [switchName, set] : switches) {
            if (switchName == name) {
                set(on);
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> Overlay::switchNames() const
    {
        std::vector<std::string> names;
        for (const auto& entry : switches) {
            names.push_back(entry.first);
        }
        return names;
    }

    void Overlay::addCommand(std::string name, std::function<std::string(const std::string&)> run)
    {
        commands.emplace_back(std::move(name), std::move(run));
    }

    bool Overlay::runCommand(const std::string& name, const std::string& value, std::string& reply)
    {
        for (const auto& [commandName, run] : commands) {
            if (commandName == name) {
                reply = run(value);
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> Overlay::commandNames() const
    {
        std::vector<std::string> names;
        for (const auto& entry : commands) {
            names.push_back(entry.first);
        }
        return names;
    }

    void Overlay::addDrawer(std::function<void(const Frame&)> draw)
    {
        drawers.push_back(std::move(draw));
    }

    void Overlay::runAfterFrame(std::string command, std::function<void(const std::string&)> replied)
    {
        pendingCommands.emplace_back(std::move(command), std::move(replied));
    }

    bool toScreen(const Frame& frame, const Vector& point, float& x, float& y)
    {
        // As the game projects its HUD's points: the field of view spans the width.
        Coords axes = rotationAxes(frame.viewRotation);
        Vector delta = point - frame.viewLocation;
        float forward = delta.dot(axes.xAxis);
        if (forward <= 1.0f) {
            return false;
        }
        ImVec2 size = ImGui::GetIO().DisplaySize;
        float halfWidth = size.x * 0.5f;
        float scale = halfWidth / std::tan(frame.fov * Pi / 360.0f);
        x = halfWidth + delta.dot(axes.yAxis) / forward * scale;
        y = size.y * 0.5f - delta.dot(axes.zAxis) / forward * scale;
        return true;
    }

    void Overlay::start()
    {
        startImGui(window, SDL_GL_GetCurrentContext());
        started = true;
        // Which tools are open goes in the settings file beside ImGui's own windows, as
        // "[AmbrosiaTool][name]" entries with an Open line.
        ImGuiSettingsHandler handler;
        handler.TypeName = "AmbrosiaTool";
        handler.TypeHash = ImHashStr("AmbrosiaTool");
        handler.UserData = this;
        handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* self, const char* name) -> void* {
            return static_cast<Overlay*>(self->UserData)->findTool(name);
        };
        handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* found, const char* line) {
            auto* entry = static_cast<ToolEntry*>(found);
            int open = 0;
            if (std::sscanf(line, "Open=%d", &open) == 1 && !entry->chosen) {
                entry->open = open != 0;
            }
        };
        handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out) {
            for (const ToolEntry& entry : static_cast<Overlay*>(self->UserData)->tools) {
                out->appendf("[AmbrosiaTool][%s]\nOpen=%d\n\n", entry.tool.name.c_str(), entry.open ? 1 : 0);
            }
        };
        ImGui::AddSettingsHandler(&handler);
        // The overlay's own choices, as "[AmbrosiaOverlay][Settings]".
        ImGuiSettingsHandler own;
        own.TypeName = "AmbrosiaOverlay";
        own.TypeHash = ImHashStr("AmbrosiaOverlay");
        own.UserData = this;
        own.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* self, const char*) -> void* {
            return self->UserData;
        };
        own.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* found, const char* line) {
            float chosen = 0;
            if (std::sscanf(line, "Scale=%f", &chosen) == 1 && (chosen == 0 || (chosen >= 0.5f && chosen <= 4.0f))) {
                static_cast<Overlay*>(found)->scaleChoice = chosen;
            }
        };
        own.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out) {
            out->appendf("[AmbrosiaOverlay][Settings]\nScale=%g\n\n", static_cast<double>(static_cast<Overlay*>(self->UserData)->scaleChoice));
        };
        ImGui::AddSettingsHandler(&own);
        if (!settingsFile.empty()) {
            ImGui::GetIO().IniFilename = settingsFile.c_str();
            ImGui::LoadIniSettingsFromDisk(settingsFile.c_str());
        }
    }

    Overlay::Mode Overlay::mode() const
    {
        return current;
    }

    void Overlay::setMode(Mode mode)
    {
        if (mode != Mode::Hidden && !started) {
            start();
        }
        if (looking && mode != Mode::Interactive) {
            stopLooking();
        }
        if (current != Mode::Hidden && mode == Mode::Hidden) {
            for (const std::function<void()>& closed : closers) {
                closed();
            }
        }
        current = mode;
    }

    bool Overlay::isOpen() const
    {
        return current != Mode::Hidden;
    }

    void Overlay::setOpen(bool value)
    {
        setMode(value ? Mode::Interactive : Mode::Hidden);
    }

    void Overlay::setKeyHint(std::string key)
    {
        keyHint = std::move(key);
    }

    void Overlay::startLooking()
    {
        looking = true;
        SDL_GetMouseState(&lookX, &lookY);
        SDL_SetWindowRelativeMouseMode(window, true);
    }

    void Overlay::stopLooking()
    {
        looking = false;
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_WarpMouseInWindow(window, lookX, lookY);
    }

    bool Overlay::event(const SDL_Event& event)
    {
        if (current != Mode::Interactive) {
            // Releases and focus changes still reach ImGui, so a key or button that was down
            // when the overlay went passive is not left held for when it is interactive again.
            if (started && (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            return false;
        }
        if (looking) {
            // The game turns with the mouse until the right button is let go (ImGui saw it
            // pressed, so it sees the release too) or the window loses focus.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                stopLooking();
                ImGui_ImplSDL3_ProcessEvent(&event);
                return true;
            }
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                stopLooking();
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_WHEEL) {
                return true;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                return false;
            }
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        const ImGuiIO& io = ImGui::GetIO();
        switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_RIGHT && !io.WantCaptureMouse) {
                startLooking();
            }
            return true;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
            return true;
        case SDL_EVENT_KEY_DOWN:
            return io.WantCaptureKeyboard;
        case SDL_EVENT_TEXT_INPUT:
            return io.WantTextInput;
        default:
            return false;
        }
    }

    void Overlay::menuBar(bool passive)
    {
        if (!ImGui::BeginMainMenuBar()) {
            return;
        }
        // Each menu once, in the order its first tool was added.
        std::vector<std::string> menus;
        for (const ToolEntry& entry : tools) {
            if (std::find(menus.begin(), menus.end(), entry.tool.menu) == menus.end()) {
                menus.push_back(entry.tool.menu);
            }
        }
        for (const std::string& menu : menus) {
            if (!ImGui::BeginMenu(menu.c_str())) {
                continue;
            }
            for (ToolEntry& entry : tools) {
                if (entry.tool.menu != menu) {
                    continue;
                }
                bool open = entry.open;
                std::string shortcut = "open " + entry.tool.name;
                if (ImGui::MenuItem(entry.tool.title.c_str(), shortcut.c_str(), &open)) {
                    setOpen(entry, open);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Overlay")) {
            if (ImGui::MenuItem("Close every tool", "close all")) {
                for (ToolEntry& entry : tools) {
                    setOpen(entry, false);
                }
            }
            if (ImGui::BeginMenu("Scale")) {
                std::string automatic = "Auto (" + std::to_string(static_cast<int>(std::lround(autoScale(window) * 100))) + "%)";
                if (ImGui::MenuItem(automatic.c_str(), "scale auto", scaleChoice == 0)) {
                    scaleCommand("auto");
                }
                for (float choice : scaleChoices()) {
                    std::string label = std::to_string(static_cast<int>(std::lround(choice * 100))) + "%";
                    if (ImGui::MenuItem(label.c_str(), nullptr, scaleChoice == choice)) {
                        scaleChoice = choice;
                        ImGui::MarkIniSettingsDirty();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Through the console, as the key would, so the game hands the mouse back.
            if (ImGui::MenuItem("Passive", "DebugOverlay passive")) {
                runAfterFrame("DebugOverlay passive");
            }
            if (ImGui::MenuItem("Hide", "DebugOverlay off")) {
                runAfterFrame("DebugOverlay off");
            }
            ImGui::EndMenu();
        }
        std::string key = keyHint.empty() ? "DebugOverlay" : keyHint + " (DebugOverlay)";
        std::string hint = passive ? "PASSIVE: the game has the mouse and keys; " + key + " makes the overlay interactive" : "INTERACTIVE: hold the right button over the game to look around; " + key + " makes it passive";
        float width = ImGui::CalcTextSize(hint.c_str()).x;
        float right = ImGui::GetWindowWidth() - width - ImGui::GetStyle().ItemSpacing.x * 2;
        if (right > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(right);
        }
        ImGui::TextColored(passive ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", hint.c_str());
        ImGui::EndMainMenuBar();
    }

    void Overlay::draw(const Frame& frame, int width, int height)
    {
        if (current == Mode::Hidden) {
            return;
        }
        bool passive = current == Mode::Passive;
        ImGuiIO& io = ImGui::GetIO();
        // Passive, or looking, the overlay leaves the mouse and the cursor to the game.
        if (passive || looking) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
        } else {
            io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
        }
        setScale(scaleChoice > 0 ? scaleChoice : autoScale(window));
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, passive ? 0.8f : 1.0f);
        menuBar(passive);
        float top = ImGui::GetFrameHeight() + scaled(8.0f);
        int index = 0;
        for (ToolEntry& entry : tools) {
            int slot = index++;
            if (!entry.open) {
                continue;
            }
            // The first time, each tool a little further down and right of the last.
            float offset = static_cast<float>(slot) * scaled(28.0f);
            float margin = scaled(16.0f);
            ImGui::SetNextWindowPos(ImVec2(margin + offset, top + offset), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(std::min(scaled(entry.tool.width), static_cast<float>(width) - 2 * margin), std::min(scaled(entry.tool.height), std::max(scaled(200.0f), static_cast<float>(height) - top - margin))), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(passive ? 0.55f : 0.85f);
            std::string id = entry.tool.title + "###" + entry.tool.name;
            bool open = true;
            if (ImGui::Begin(id.c_str(), &open, passive ? ImGuiWindowFlags_NoInputs : 0)) {
                ImGui::PushID(entry.tool.name.c_str());
                entry.tool.draw(frame);
                ImGui::PopID();
            }
            ImGui::End();
            if (!open) {
                setOpen(entry, false);
            }
        }
        ImGui::PopStyleVar();
        for (const std::function<void(const Frame&)>& drawer : drawers) {
            drawer(frame);
        }
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::vector<std::pair<std::string, std::function<void(const std::string&)>>> pending;
        pending.swap(pendingCommands);
        for (const auto& [command, replied] : pending) {
            std::string reply = console::execute(frame.player, command);
            if (replied) {
                replied(reply);
            }
        }
    }
}
