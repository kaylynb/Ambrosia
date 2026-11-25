#pragma once

#include "core/run_steps.h"
#include "core/settings.h"
#include "core/url.h"
#include "debugui/overlay.h"
#include "deusex/deusex.h"
#include "engine/resolutions.h"
#include "engine/world.h"
#include "game/gamepad_input.h"
#include "game/options.h"
#include "game/platform.h"
#include "game/system_keyboard.h"
#include "game/touch_cursor.h"
#include "render/scene_renderer.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace dx::game {
    // A Holder for the objects it makes and keeps itself for the whole session, as the original's
    // UGameEngine keeps its client's viewports: the viewport, its console and the canvas. The
    // world, the renderer and the pad hand over their own pointers.
    class Game final : public dx::gc::Holder {
    public:
        Game(SDL_Window* target, const Options& settings)
            : window(target)
            , options(settings)
            , steps(settings.steps)
            , pad({
                  .player = [this] { return world ? world->player() : nullptr; },
                  .sendKey = [this](uint8_t key, bool pressed) { sendKey(key, pressed); },
                  .editText = systemkeyboard::available() ? std::function<bool()>(systemkeyboard::editFocused) : std::function<bool()>([this] { return editOnScreen(); }),
                  .openDevices = !settings.hidden,
              })
            , touch({
                  .wantsCursor = [] { return dx::extension::wantsMouse(); },
                  .move = [](float x, float y) { dx::extension::mouseMove(x, y); },
                  .button = [this](bool pressed) {
                      if (!dx::extension::wantsMouse() || !dx::extension::mouseButton(dx::IK_LeftMouse, pressed)) {
                          handleKey(dx::IK_LeftMouse, pressed);
                      } },
                  // Tapping a text field types in it, as A does.
                  .released = [] { systemkeyboard::edit(dx::extension::cursorWindow()); },
              })
        {
        }

        void addReferences(dx::ReferenceVisitor& references) override
        {
            references(canvasObject);
            references(viewport);
        }

        // Whether the window is full screen now.
        bool isFullscreen() const;
        void saveVideoMode();
        void centreWindow();
        void windowResized(int width, int height);
        void saveSettledResize();
        bool exclusiveFullscreen();
        dx::resolutions::Size desktopSize();
        dx::resolutions::Size windowRoom();
        dx::resolutions::Size windowedSize();
        dx::resolutions::Size fullscreenSize();
        void applyVideoMode(bool fullscreen, int width, int height);
        std::string availableResolutions();
        float displayFov(float fov, int width, int height);
        void loadDeusExClasses();
        static void needProperties(dx::deusex::MenuPatch& patch, dx::UClass* cls, std::initializer_list<const char*> names, int count = 1);
        template <typename T>
        static T& defaultField(dx::UObject* defaults, const char* name, int32_t index = 0);
        dx::UClass* createOptionsScreen(const char* className, const char* title);
        // infoWidth: the value button's width, which each screen's art is drawn for.
        dx::UClass* addChoice(dx::UClass* screen, int index, const char* parentPath, const char* name, const char* action, const char* help, const dx::settings::Setting& setting, const std::vector<const char*>& values, int infoWidth);
        void addEnumChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Int& setting, const std::vector<const char*>& values, int infoWidth = 88);
        void addSwitchChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Bool& setting, int infoWidth = 88, const std::vector<const char*>& values = { "Off", "On" });
        void addSliderChoice(dx::UClass* screen, int index, const char* name, const char* action, const char* help, const dx::settings::Int& setting, int lowest, int highest);
        void addMixerChoice();
        void addExtraOptions();
        void sizeOptionsScreen(dx::UObject* defaults, int rows);
        void addControllerOptions();
        void applyControllerOptions();
        void applyRenderOptions();
        void limitFrameRate();
        void removeUnavailableOptions();
        void addFieldOfViewChoice();
        bool start();
        bool project(const dx::Vector& location, float& pixelX, float& pixelY);
        bool openMap(const std::string& url, bool carryItems = false);
        std::string entryMap();
        void returnToEntryMap(const std::string& failed);
        dx::UObject* canvas(int width, int height);
        dx::UObject* scriptConsole();
        bool consoleKeyEvent(uint8_t key, uint8_t action, float delta);
        bool consoleTyping();
        // Types the system clipboard's text into the console's line, as if each character were keyed.
        void pasteIntoConsole();
        void charEvent(char c);
        void handleKey(uint8_t key, bool pressed);
        void processEvents();
        void repeatKey(uint8_t key);
        void sendKey(uint8_t key, bool pressed);
        void paintProgress();
        void paintLoading();
        void handleTravel();
        void render(int width, int height);
        void scriptedAction(const std::string& action);
        // Shows the developer overlay passive or interactive, or hides it, handing the mouse to it
        // or back to the game.
        void setOverlayMode(dx::debugui::Overlay::Mode mode);
        void drawOverlay(int width, int height);
        // Where text input opens an on-screen keyboard (platform::onScreenKeyboard): starts
        // editing the focused text field, true when it is one; and each frame keeps text input
        // on only while that field keeps focus, the console is typing or the overlay is in use.
        bool editOnScreen();
        void updateTextInput();

        // The memory counters in the log: "memstats <label>" for the soak harness, "memlog" after
        // each level change (logLevelMemory), with what that collection did, and at quitting.
        struct MemoryTotals {
            uint64_t process = 0;
            size_t objects = 0;
            uint64_t data = 0;
        };

        MemoryTotals printMemoryStats(const std::string& label, const char* prefix);
        void logLevelMemory(const std::string& map, bool exiting = false);
        // The process's memory in use: private bytes on Windows, resident bytes on Linux, the
        // used memory of the process on the console. 0 where unknown.
        static uint64_t processMemoryBytes();
        // The process's memory at the end of the level being left, before its collection, for
        // memlog's process_before.
        uint64_t processBeforeTravel = 0;
        // Puts the player where the camera action holds them for this frame.
        void holdCamera();
        // The view test action: holds the player looking at an actor from a clear spot near it.
        void viewActor(const std::string& argument);
        void begin();
        int run();
        void step();

    private:
        SDL_Window* window;
        Options options;
        // --do and --script, run after each frame (game_scripted.cc has the actions).
        dx::steps::Runner steps;
        dx::game::GamepadInput pad;
        dx::game::TouchCursor touch;
        std::unique_ptr<dx::SceneRenderer> renderer;
        std::unique_ptr<dx::World> world;
        // The developer overlay. Its ImGui context is made only when it first opens, and goes
        // with the game, before main destroys the GL context.
        std::unique_ptr<dx::debugui::Overlay> overlay;
        dx::UObject* canvasObject = nullptr;
        // Made by the first level's login and handed to every level after (World::viewport).
        // Its console is reached through its Console property, so is kept with it.
        dx::UObject* viewport = nullptr;
        std::string pendingMap;
        // A map opened by its path at startup, which travel cannot find by name.
        std::string fileMap;
        int32_t pendingTravelType = dx::TRAVEL_Partial;
        std::string pendingShot;

        // For measuring a fixed or scripted view: the "camera" test action holds the player at a
        // place and rotation every frame, still, spinning by a yaw step a frame, or moving between
        // two places and back over a number of frames, counted from the action's frame.
        struct CameraHold {
            bool active = false;
            dx::Vector from;
            dx::Rotator fromRotation;
            dx::Vector to;
            dx::Rotator toRotation;
            int pathFrames = 0;
            int spinPerFrame = 0;
            int startFrame = 0;
            // The frame it lets go by itself, as the view action does; -1 to hold until
            // "camera free".
            int releaseFrame = -1;
            bool reportedFailure = false;
        } cameraHold;

        dx::Vector viewLocation;
        dx::Rotator viewRotation;
        float viewFov = 90;
        int frameWidth = 1;
        double viewportTime = 0;
        int frameHeight = 1;
        bool running = true;
        uint64_t last = 0;
        int frame = 0;
        bool guiMode = true;
        // True while paintProgress draws its frame.
        bool progressFrame = false;
        int appliedVSync = -1;
        uint64_t frameDeadline = 0;
        int monitorHz = 60;
        int appliedConversationFont = -1;
        // The last windowed size the game set or saved, to tell a user's resize from its own.
        dx::resolutions::Size appliedWindow;
        // When a user's resize is saved, or 0 with none waiting.
        uint64_t resizeSaveAt = 0;
        // The text field being edited on the on-screen keyboard, only ever compared with the
        // focused window; and whether text input is on for it.
        dx::UObject* editingField = nullptr;
        bool textInputOn = false;
    };
}
