#include "extension/windows.h"

#include "core/gc.h"

#include <algorithm>
#include <limits>

namespace dx::extension {
    void registerNatives()
    {
        registerFactories();
        registerWindowNatives();
        registerGCNatives();
        registerTextWindowNatives();
        registerContainerNatives();
        registerListNatives();
        registerScaleNatives();
        registerComputerNatives();
        registerFlagNatives();
        registerConSysNatives();
        registerTextParserNatives();
        registerReferences();
        gc::libraryReported("extension");
    }

    void tick(float deltaSeconds)
    {
        State& s = state();
        s.time += static_cast<double>(deltaSeconds);
        if (s.root && !s.root->destroyed) {
            s.root->tick(deltaSeconds);
            // A newly opened screen may leave focus somewhere the pad cannot reach, such as the
            // persona tab bar, where A would act on it unseen. Move it into the screen's own
            // controls. Edit fields keep focus while the player types.
            // A control that has not been laid out yet still counts, so focus put on a freshly
            // made button is not taken away before its first layout.
            URootWindow* root = s.root;
            UWindow* focus = root->focusWindow();
            bool reachable = focus && root->isTraversable(focus, true) && !(s.navigationSkip && s.navigationSkip(focus));
            if (root->gamepadFocus && menuOpen() && !cast<UEditWindow>(focus) && !reachable && !root->navigationCandidates().empty()) {
                root->setRootFocusWindow(nullptr);
                root->moveFocusToward(0, 0);
            }
        }
        releaseDestroyedWindows();
    }

    void releaseDestroyedWindows()
    {
        State& s = state();
        if (s.destroyedWindows.empty() || !gc::mayReleaseNow()) {
            return;
        }
        // XWindow::SafeDestroy deletes a window once nothing has it locked,
        // without looking for references to it: script that holds one clears its own. Ambrosia
        // frees it at the end of the tick or draw that destroyed it, once no script is running.
        std::vector<UObject*> windows = std::move(s.destroyedWindows);
        s.destroyedWindows.clear();
        gc::freeObjects(windows);
    }

    void setNavigationSkip(NavigationSkip skip)
    {
        state().navigationSkip = std::move(skip);
    }

    void setOwnFocusLook(NavigationSkip ownLook)
    {
        state().ownFocusLook = std::move(ownLook);
    }

    void setNavigationWrap(NavigationWrap wrap)
    {
        state().navigationWrap = std::move(wrap);
    }

    void setWindowBackdrop(WindowBackdrop backdrop)
    {
        state().windowBackdrop = std::move(backdrop);
    }

    void setOverlayDrawer(OverlayDrawer drawer)
    {
        state().overlayDrawer = std::move(drawer);
    }

    void setButtonHook(ButtonHook hook)
    {
        state().buttonHook = std::move(hook);
    }

    void setMouseReleaseHook(MouseReleaseHook hook)
    {
        state().mouseReleaseHook = std::move(hook);
    }

    UObject* rootWindow()
    {
        State& s = state();
        if (s.root && s.root->destroyed) {
            s.root = nullptr;
        }
        return s.root;
    }

    bool wantsMouse()
    {
        State& s = state();
        return s.root && !s.root->destroyed && s.root->isCursorVisible();
    }

    bool menuOpen()
    {
        State& s = state();
        return s.root && !s.root->destroyed && s.keyboardGrabs > 0 && s.root->isModalOpen();
    }

    UObject* topModal()
    {
        return menuOpen() ? state().root->topModal() : nullptr;
    }

    std::optional<Area> windowArea(UObject* object)
    {
        UWindow* window = cast<UWindow>(object);
        if (!window || window->destroyed || !window->root()) {
            return std::nullopt;
        }
        Area area;
        window->absolutePosition(area.x, area.y);
        area.width = window->width();
        area.height = window->height();
        return area;
    }

    float uiScale()
    {
        return virtualScale();
    }

    float contentBottom(UObject* object)
    {
        UWindow* top = cast<UWindow>(object);
        if (!top || top->destroyed || !top->root()) {
            return 0;
        }

        // Each window with the lowest edge it can be seen above: what a scroll area holds past
        // its clip window's bottom is hidden, and must not push the prompts down.
        struct Pending {
            UWindow* window;
            float limit;
        };

        float bottom = 0;
        std::vector<Pending> pending { { top, std::numeric_limits<float>::max() } };
        while (!pending.empty()) {
            auto [window, limit] = pending.back();
            pending.pop_back();
            if (!window->visible()) {
                continue;
            }
            float x = 0;
            float y = 0;
            window->absolutePosition(x, y);
            if (!window->firstChild()) {
                if (window->width() > 0 && window->height() > 0 && y < limit) {
                    bottom = std::max(bottom, std::min(y + window->height(), limit));
                }
                continue;
            }
            if (cast<UClipWindow>(window)) {
                limit = std::min(limit, y + window->height());
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                pending.push_back({ child, limit });
            }
        }
        return bottom * virtualScale();
    }

    UObject* focusWindow()
    {
        State& s = state();
        return s.root && !s.root->destroyed ? s.root->focusWindow() : nullptr;
    }

    UObject* cursorWindow()
    {
        State& s = state();
        UObject* window = s.root && !s.root->destroyed ? rootwindow::lastMouseWindow(s.root) : nullptr;
        return window && !window->destroyed ? window : nullptr;
    }

    void setFocusWindow(UObject* object)
    {
        State& s = state();
        UWindow* window = cast<UWindow>(object);
        if (s.root && !s.root->destroyed && window && !window->destroyed) {
            s.root->setRootFocusWindow(window);
        }
    }

    void setViewportSize(float width, float height)
    {
        state().viewportWidth = width;
        state().viewportHeight = height;
    }

    void setUIScale(int scale)
    {
        state().uiScale = scale;
    }

    void setFrameCanvas(Canvas2D* canvas)
    {
        state().frameCanvas = canvas;
        if (canvas) {
            setViewportSize(canvas->width(), canvas->height());
        }
    }

    void setViewportDrawer(ViewportDrawer drawer)
    {
        state().viewportDrawer = std::move(drawer);
    }

    void setActorDrawer(ActorDrawer drawer)
    {
        state().actorDrawer = std::move(drawer);
    }

    void setProjector(Projector projector)
    {
        state().projector = std::move(projector);
    }

    void setSoundPlayer(SoundPlayer player)
    {
        state().soundPlayer = std::move(player);
    }

    void reset()
    {
        State& s = state();
        if (s.root && !s.root->destroyed) {
            s.root->destroyWindowTree();
        }
        s.root = nullptr;
        s.keys.fill(false);
        s.keyboardGrabs = 0;
        // Everything destroyed so far hung off the tree that just went. The windows are deleted
        // at the next collection, once the level change is done.
        gc::releaseLater(s.destroyedWindows);
        s.destroyedWindows.clear();
    }
}
