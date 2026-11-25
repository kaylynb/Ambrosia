#pragma once

#include "core/math.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace dx {
    class SceneRenderer;
    class UObject;
    class World;
}

// A developer overlay drawn over the game with Dear ImGui, for debugging while playing: a menu
// bar whose menus open one window per tool. It is made the first time it opens, so a game that
// never opens it does nothing different.
namespace dx::debugui {
    // What a tool sees each frame.
    struct Frame {
        World* world = nullptr;
        UObject* player = nullptr;
        // Where the game drew the view from, which is where the sound is heard from.
        Vector viewLocation;
        Rotator viewRotation;
        // The view's horizontal field of view in degrees.
        float fov = 90;
        // For tools that draw into the level (SceneRenderer::setDebugMeshes); what they set
        // shows from the next frame.
        SceneRenderer* renderer = nullptr;
    };

    // Where a point in the level shows on screen, in ImGui's display coordinates; false when it is
    // not in front of the view. For drawing into ImGui's background draw list.
    bool toScreen(const Frame& frame, const Vector& point, float& x, float& y);

    // One tool: a window listed under a menu of the menu bar.
    struct Tool {
        // The menu bar's menu it is listed under, such as "Audio".
        std::string menu;
        // The window's title and the menu item.
        std::string title;
        // The name the console opens it by, "DebugOverlay open <name>": lower case, no spaces.
        std::string name;
        // Draws the window's contents, each frame the window is open and not collapsed.
        std::function<void(const Frame&)> draw;
        // Called when the window is closed, for taking away what it drew in the level; may be
        // empty.
        std::function<void()> closed;
        // The window's size the first time it opens, at scale 1.
        float width = 640;
        float height = 480;
        // Whether it is open the first time the overlay opens, before the settings file says.
        bool openAtFirst = false;
    };

    class Overlay {
    public:
        explicit Overlay(SDL_Window* window);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        // Tools are listed in the order added, their menus in the order first named.
        void addTool(Tool tool);
        // Opens or closes a tool's window by its console name; false for an unknown name.
        bool setToolOpen(const std::string& name, bool open);
        bool isToolOpen(const std::string& name) const;
        std::vector<std::string> toolNames() const;
        // Where ImGui keeps where each window is and which tools are open, or empty to keep
        // nothing (as for hidden runs, so each starts the same). Set before the overlay first
        // opens; it is read then.
        void setSettingsFile(std::string path);
        // Called when the overlay closes, for tools to take away what they drew in the level.
        void onClose(std::function<void()> closed);
        // A tool's switch that the console can set as "DebugOverlay <name> on|off", for
        // scripted runs. setSwitch is false for an unknown name.
        void addSwitch(std::string name, std::function<void(bool)> set);
        bool setSwitch(const std::string& name, bool on);
        std::vector<std::string> switchNames() const;
        // A tool's command the console can run as "DebugOverlay <name> <value>" (lowercased),
        // for values other than on and off; runCommand is false for an unknown name and otherwise
        // gives the command's reply. The overlay has two of its own: "open <tool>" and
        // "close <tool>", where the tool may also be "all".
        void addCommand(std::string name, std::function<std::string(const std::string&)> run);
        bool runCommand(const std::string& name, const std::string& value, std::string& reply);
        std::vector<std::string> commandNames() const;
        // Called every frame the overlay is shown, passive or interactive, after the windows and
        // whether or not any is open: for drawing over the level with
        // ImGui::GetBackgroundDrawList().
        void addDrawer(std::function<void(const Frame&)> draw);
        // Runs a console command once the overlay's frame is drawn, as if typed at the console,
        // and hands its reply to replied (which may be empty). For tools whose buttons do what a
        // player could type: run from inside a window, the command could open screens or change
        // the level partway through the overlay's frame.
        void runAfterFrame(std::string command, std::function<void(const std::string&)> replied = {});
        // Hidden: not drawn. Passive: drawn and live, but the game has the mouse and keyboard
        // exactly as in normal play and the overlay takes no input. Interactive: the cursor is
        // free and the overlay takes the mouse; holding the right button over the game (not over
        // an overlay window) looks around until it is let go, as the editor's viewports do.
        enum class Mode {
            Hidden,
            Passive,
            Interactive,
        };
        Mode mode() const;
        void setMode(Mode mode);
        bool isOpen() const;
        // Open is interactive; closed is hidden.
        void setOpen(bool open);
        // The key bound to DebugOverlay, for the overlay's label, or empty for none.
        void setKeyHint(std::string key);
        // Hands an event to the overlay. True when the game should not see it. Interactive:
        // every mouse movement, press and wheel turn (releases still reach the game, so nothing
        // stays held), and keys and text while an overlay control takes typing; while looking
        // with the right button, the movements go to the game. Hidden or passive: nothing.
        bool event(const SDL_Event& event);
        // Draws the overlay into the window's framebuffer, over the finished frame.
        void draw(const Frame& frame, int width, int height);

    private:
        struct ToolEntry {
            Tool tool;
            bool open = false;
            // Opened or closed before the settings file was read, which then leaves it be.
            bool chosen = false;
        };

        ToolEntry* findTool(const std::string& name);
        void setOpen(ToolEntry& entry, bool open);
        std::string openCommand(const std::string& value, bool open);
        std::string scaleCommand(const std::string& value);
        void start();
        void menuBar(bool passive);
        void startLooking();
        void stopLooking();

        SDL_Window* window;
        bool started = false;
        Mode current = Mode::Hidden;
        // Looking around with the right button held, and where the cursor was.
        bool looking = false;
        float lookX = 0;
        float lookY = 0;
        std::string keyHint;
        std::string settingsFile;
        // The scale chosen in the Overlay menu, or 0 for Auto (debugui::autoScale); saved in the
        // settings file.
        float scaleChoice = 0;
        // Console commands asked for during the frame, run once it is drawn.
        std::vector<std::pair<std::string, std::function<void(const std::string&)>>> pendingCommands;
        std::vector<ToolEntry> tools;
        std::vector<std::function<void()>> closers;
        std::vector<std::pair<std::string, std::function<void(bool)>>> switches;
        std::vector<std::pair<std::string, std::function<std::string(const std::string&)>>> commands;
        std::vector<std::function<void(const Frame&)>> drawers;
    };
}
