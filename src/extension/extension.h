#pragma once

#include "core/math.h"
#include "engine/canvas.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dx {
    class UObject;

    namespace extension {
        struct ViewportRequest {
            float x = 0;
            float y = 0;
            float width = 0;
            float height = 0;
            Vector location;
            Rotator rotation;
            float fov = 90;
            UObject* originActor = nullptr;
            UObject* watchActor = nullptr;
            bool showActor = false;
            bool showWeapons = false;
            bool clearZ = true;
        };

        struct ActorDrawRequest {
            UObject* actor = nullptr;
            float x = 0;
            float y = 0;
            float width = 0;
            float height = 0;
            bool clearZ = false;
            bool constrain = false;
            bool unlit = false;
            float drawScale = 1;
            float scaleGlow = 1;
            UObject* skin = nullptr;
        };

        using ViewportDrawer = std::function<void(Canvas2D& canvas, const ViewportRequest& request)>;
        using ActorDrawer = std::function<void(Canvas2D& canvas, const ActorDrawRequest& request)>;
        using Projector = std::function<bool(const Vector& location, float& pixelX, float& pixelY)>;
        using SoundPlayer = std::function<void(UObject* sound, UObject* source, float volume, float pitch)>;
        using NavigationSkip = std::function<bool(UObject* window)>;

        // Part of a backdrop, in the window's own coordinates: a texture drawn masked in the
        // backdrop color, or a solid rectangle when the texture is null.
        struct BackdropTile {
            UObject* texture = nullptr;
            float x = 0;
            float y = 0;
            float width = 0;
            float height = 0;
        };

        // Asked before each window draws; returning true paints the tiles in that color behind
        // it, so a game window can be given a stronger background than its script draws.
        using WindowBackdrop = std::function<bool(UObject* window, Color& fill, std::vector<BackdropTile>& tiles)>;

        void registerNatives();
        void tick(float deltaSeconds);
        void render(Canvas2D& canvas);
        // Deletes the windows destroyed since the last call, as XWindow::SafeDestroy does once
        // a window is no longer locked, when no script is running. tick and render end with it.
        void releaseDestroyedWindows();
        bool keyEvent(uint8_t key, uint8_t action, float delta);
        bool charEvent(char character);
        void mouseMove(float x, float y);
        bool cursorPosition(float& x, float& y);
        bool mouseButton(uint8_t key, bool pressed);
        // Gamepad menu navigation. The arrow key goes to the focused control first so lists and
        // sliders still step through their items; otherwise focus moves to the nearest control
        // in that direction. Edit fields are skipped, so text entry stays on the keyboard.
        bool navigate(uint8_t arrowKey);
        // Marks the gamepad as the active input: the cursor hides and the focus is outlined
        // until the mouse moves again.
        void useGamepad();
        // Scrolls a step without the mouse: the panels holding no list, such as a description,
        // a conversation's text or the notes, read top to bottom as one page;
        // with none, the scroll area around the focused control, or failing that every one.
        bool scroll(bool up);
        // Whether scroll() would find anything to move, for showing a scroll prompt.
        bool canScroll();
        // For scripted runs: the screen on top, its focus, the controls gamepad focus can
        // reach and the shown ones it cannot, and what the right stick scrolls.
        std::string focusReport();
        // Controls gamepad focus should never land on, such as a tab bar the bumpers drive.
        void setNavigationSkip(NavigationSkip skip);
        // Controls that show gamepad focus themselves, such as a conversation's replies, which
        // get no outline.
        void setOwnFocusLook(NavigationSkip ownLook);
        // Screens where moving focus up from the top control or down from the bottom one wraps
        // round to the other end, such as the main menus.
        using NavigationWrap = std::function<bool(UObject* modal)>;
        void setNavigationWrap(NavigationWrap wrap);
        void setWindowBackdrop(WindowBackdrop backdrop);
        // Drawn over every window, under the gamepad's focus outline and the cursor: for
        // screens drawn natively over a window of their own, as the controller diagram is.
        using OverlayDrawer = std::function<void(Canvas2D& canvas)>;
        void setOverlayDrawer(OverlayDrawer drawer);
        // Offered every button press, by mouse or key, before the game's ButtonActivated
        // handlers see it; returning true consumes it.
        using ButtonHook = std::function<bool(UObject* button)>;
        void setButtonHook(ButtonHook hook);
        // Offered each mouse button release with the window under (or holding) the mouse,
        // before the game sees it; returning true holds it back from the game's scripts. The
        // mouse grab still ends, so a held-back drag does not stay attached to the mouse.
        using MouseReleaseHook = std::function<bool(UObject* window, uint8_t button)>;
        void setMouseReleaseHook(MouseReleaseHook hook);
        UObject* rootWindow();
        // Snapshots, the pictures behind the save screen and in each savegame, are taken from
        // the last frame shown, as the original reads it back from the render device. The game
        // gives the reader: the frame top row first in RGBA, false when there is none.
        using FrameReader = std::function<bool(std::vector<uint8_t>& rgba, int& width, int& height)>;
        void setFrameReader(FrameReader reader);
        // The last frame shown, scaled to the root window's snapshot size in gray, as a new
        // transient texture; null without one.
        UObject* generateSnapshot(bool filter);
        // RootWindow.SetSnapshotSize, which the original's save sets to 160 by 120 itself.
        void setSnapshotSize(int width, int height);
        bool wantsMouse();
        // True while a modal window (menu, persona screen, conversation) has the keyboard, even
        // when it hides the cursor, as conversations do while speech plays.
        bool menuOpen();
        // The modal window on top, such as the persona screen being shown, or null.
        UObject* topModal();

        struct Area {
            float x = 0;
            float y = 0;
            float width = 0;
            float height = 0;
        };

        // Where a window is, in the root window's own units, the ones scripts lay windows out
        // in; empty for a window that is not in the window tree.
        std::optional<Area> windowArea(UObject* window);
        UObject* focusWindow();
        void setFocusWindow(UObject* window);
        // The window under the cursor as of the last mouse move, or null.
        UObject* cursorWindow();
        // Framebuffer pixels per root window unit.
        float uiScale();
        // How far down the screen a window's visible content reaches, in framebuffer pixels:
        // the lowest bottom edge among its visible windows that have no children of their own,
        // each cut off at the clip windows it sits in.
        // Screens are often as tall as the view, so their own size says little.
        float contentBottom(UObject* window);

        void setViewportSize(float width, float height);
        void setUIScale(int scale);
        void setFrameCanvas(Canvas2D* canvas);
        void setViewportDrawer(ViewportDrawer drawer);
        void setActorDrawer(ActorDrawer drawer);
        void setProjector(Projector projector);
        void setSoundPlayer(SoundPlayer player);
        void reset();
    }
}
