#include "extension/windows.h"

#include "core/scoped_value.h"
#include "extension/internal.h"
#include "extension/window_internal.h"

#include <algorithm>
#include <cmath>

namespace dx::extension {
    void UWindow::drawBackground(UGC* gc)
    {
        if (!window::bDrawRawBackground(this)) {
            return;
        }
        UObject* background = window::Background(this);
        if (!background) {
            return;
        }
        float x = 0;
        float y = 0;
        float w = width();
        float h = height();
        if (!parent()) {
            x = -usedX;
            y = -usedY;
            w = state().viewportWidth / virtualScale();
            h = state().viewportHeight / virtualScale();
        }
        gc->setStyle(window::backgroundStyle(this));
        gc::bSmoothed(gc) = window::bSmoothBackground(this);
        gc::tileColor(gc) = window::tileColor(this);
        float startX = x;
        gc->bleed(x, w);
        if (window::bStretchBackground(this)) {
            gc->drawStretched(x, y, w, h, 0, 0, textureWidth(background), textureHeight(background), background);
        } else {
            gc->drawPattern(x, y, w, h, x - startX, 0, background);
        }
        gc->resetState(this, gc->offsetX, gc->offsetY);
    }

    void UWindow::scriptDrawWindow(UGC* gc)
    {
        static const Name eventName("DrawWindow");
        vm::Call call(this, eventName);
        call.obj(gc);
        runVoid(call);
    }

    void UWindow::drawWindow(UGC* gc)
    {
        drawBackground(gc);
        scriptDrawWindow(gc);
    }

    void UWindow::postDrawWindow(UGC* gc)
    {
        static const Name eventName("PostDrawWindow");
        vm::Call call(this, eventName);
        call.obj(gc);
        runVoid(call);
    }

    void URootWindow::postDrawWindow(UGC* gc)
    {
        UModalWindow::postDrawWindow(gc);
        // After every window, before the focus outline and the cursor, so both stay on top.
        if (const OverlayDrawer& overlay = state().overlayDrawer; overlay && state().canvas) {
            overlay(*state().canvas);
        }
        if (gamepadFocus && isModalOpen()) {
            UWindow* focus = focusWindow();
            std::vector<UWindow*> candidates = focus ? navigationCandidates() : std::vector<UWindow*> {};
            // Lists outline their focused row themselves, and some controls light up instead.
            const NavigationSkip& ownLook = state().ownFocusLook;
            if (focus && !cast<UListWindow>(focus) && !(ownLook && ownLook(focus)) && std::find(candidates.begin(), candidates.end(), focus) != candidates.end()) {
                // Clip to the ancestors so a control inside a scrolled area is outlined only
                // where it shows.
                float left = 0;
                float top = 0;
                focus->absolutePosition(left, top);
                float right = left + focus->width();
                float bottom = top + focus->height();
                for (UWindow* ancestor = focus->parent(); ancestor && ancestor != this; ancestor = ancestor->parent()) {
                    // Only clip windows hide what lies outside them; plain containers are often
                    // left unsized.
                    if (!cast<UClipWindow>(ancestor)) {
                        continue;
                    }
                    float ax = 0;
                    float ay = 0;
                    ancestor->absolutePosition(ax, ay);
                    left = std::max(left, ax);
                    top = std::max(top, ay);
                    right = std::min(right, ax + ancestor->width());
                    bottom = std::min(bottom, ay + ancestor->height());
                }
                if (right > left && bottom > top) {
                    gc->resetState(this, usedX, usedY);
                    gc->setStyle(DSTY_Normal);
                    gc::tileColor(gc) = { 255, 255, 255, 255 };
                    gc->drawBox(left - usedX - 1, top - usedY - 1, right - left + 2, bottom - top + 2, 1, nullptr);
                }
            }
            return;
        }
        if (!isCursorVisible()) {
            return;
        }
        float relativeX = 0;
        float relativeY = 0;
        UWindow* focus = cursorFocus(relativeX, relativeY);
        UObject* cursor = nullptr;
        UObject* shadow = nullptr;
        Color color { 255, 255, 255, 255 };
        float hotX = 0;
        float hotY = 0;
        float focusX = 0;
        float focusY = 0;
        if (focus) {
            focus->absolutePosition(focusX, focusY);
        }
        for (UWindow* current = focus; current; current = current->parent()) {
            // Scripts swap the cursor through CursorRequested; the inventory uses it to show the
            // icon of the item being dragged.
            vm::Call request(current, Name("CursorRequested"));
            if (request.valid()) {
                float currentX = 0;
                float currentY = 0;
                current->absolutePosition(currentX, currentY);
                request.obj(focus).f(relativeX + focusX - currentX).f(relativeY + focusY - currentY);
                request.f(window::defaultHotX(current)).f(window::defaultHotY(current)).skip().obj(window::defaultCursorShadow(current));
                request.run();
                UObject* requested = request.returnValue() ? *reinterpret_cast<UObject**>(request.returnValue()) : nullptr;
                if (requested) {
                    cursor = requested;
                    hotX = *reinterpret_cast<float*>(request.parameter(3));
                    hotY = *reinterpret_cast<float*>(request.parameter(4));
                    Color custom = *reinterpret_cast<Color*>(request.parameter(5));
                    if (custom.r || custom.g || custom.b) {
                        color = custom;
                    }
                    shadow = *reinterpret_cast<UObject**>(request.parameter(6));
                    break;
                }
            }
            if (window::defaultCursor(current)) {
                cursor = window::defaultCursor(current);
                shadow = window::defaultCursorShadow(current);
                hotX = window::defaultHotX(current);
                hotY = window::defaultHotY(current);
                Color custom = window::defaultCursorColor(current);
                if (custom.r || custom.g || custom.b) {
                    color = custom;
                }
                break;
            }
        }
        if (!cursor) {
            return;
        }
        float cursorWidth = textureWidth(cursor);
        float cursorHeight = textureHeight(cursor);
        if (hotX < 0 || hotX >= cursorWidth) {
            hotX = std::floor(cursorWidth * 0.5f) - 1;
        }
        if (hotY < 0 || hotY >= cursorHeight) {
            hotY = std::floor(cursorHeight * 0.5f) - 1;
        }
        gc->resetState(this, usedX, usedY);
        if (shadow) {
            gc->setStyle(DSTY_Modulated);
            gc::tileColor(gc) = { 255, 255, 255, 255 };
            gc->drawIcon(mouseX() - hotX, mouseY() - hotY, shadow);
        }
        gc->setStyle(DSTY_Masked);
        gc::tileColor(gc) = color;
        gc->drawIcon(mouseX() - hotX, mouseY() - hotY, cursor);
    }

    namespace {
        void drawTree(UWindow* window, float offsetX, float offsetY)
        {
            if (!window->visible() || window->destroyed) {
                return;
            }
            UGC* gc = sharedGC();
            offsetX += window->usedX;
            offsetY += window->usedY;
            // XWindow::DrawTree pushes the GC before
            // drawing a window and pops back to that level afterwards, so
            // unbalanced script PushGC calls end with the window.
            size_t clipDepth = gc->clipStack.size();
            size_t savedDepth = gc->savedStates.size();
            bool clipped = window->parent() != nullptr;
            if (clipped && spansSafeArea(window, offsetX)) {
                gc->pushClip(0, offsetY, state().viewportWidth / virtualScale(), window->height());
            } else if (clipped) {
                gc->pushClip(offsetX, offsetY, window->width(), window->height());
            }
            gc->resetState(window, offsetX, offsetY);
            const WindowBackdrop& backdropHook = state().windowBackdrop;
            Color backdrop {};
            std::vector<BackdropTile> tiles;
            if (backdropHook && backdropHook(window, backdrop, tiles)) {
                for (const BackdropTile& tile : tiles) {
                    float x = offsetX + tile.x;
                    float y = offsetY + tile.y;
                    if (tile.texture) {
                        gc->drawTile(tile.texture, x, y, tile.width, tile.height, 0, 0, textureWidth(tile.texture), textureHeight(tile.texture), backdrop, BlendStyle::Masked, false);
                    } else {
                        gc->drawRect(x, y, tile.width, tile.height, backdrop, BlendStyle::Normal);
                    }
                }
            }
            window->drawWindow(gc);
            UWindow* child = window->firstChild();
            UWindow* previous = nullptr;
            while (child) {
                UWindow* following = child->nextSibling();
                drawTree(child, offsetX, offsetY);
                UWindow* next = nextChild(window, child, following, previous);
                if (attachedTo(child, window)) {
                    previous = child;
                }
                child = next;
            }
            if (!window->destroyed) {
                gc->resetState(window, offsetX, offsetY);
                window->postDrawWindow(gc);
            }
            if (gc->savedStates.size() > savedDepth) {
                gc->savedStates.erase(gc->savedStates.begin() + static_cast<ptrdiff_t>(savedDepth), gc->savedStates.end());
            }
            while (gc->clipStack.size() > clipDepth) {
                gc->popClip();
            }
        }

        // The window frames the root window's ShowFrames turns on (bShowFrames, the WinFrames
        // cheat): an outline round every visible window, coloured by how deep it is in the
        // tree, and white round the focus window.
        void drawFrames(UGC* gc, UWindow* window, UObject* focus, float offsetX, float offsetY, int depth)
        {
            if (!window->visible() || window->destroyed) {
                return;
            }
            static const Color colours[] = {
                { 255, 64, 64, 255 },
                { 64, 255, 64, 255 },
                { 64, 128, 255, 255 },
                { 255, 255, 64, 255 },
                { 64, 255, 255, 255 },
                { 255, 64, 255, 255 },
            };
            offsetX += window->usedX;
            offsetY += window->usedY;
            Color colour = window == focus ? Color { 255, 255, 255, 255 } : colours[depth % 6];
            float w = window->width();
            float h = window->height();
            if (w > 0 && h > 0) {
                gc->drawRect(offsetX, offsetY, w, 1, colour, BlendStyle::Normal);
                gc->drawRect(offsetX, offsetY + h - 1, w, 1, colour, BlendStyle::Normal);
                gc->drawRect(offsetX, offsetY, 1, h, colour, BlendStyle::Normal);
                gc->drawRect(offsetX + w - 1, offsetY, 1, h, colour, BlendStyle::Normal);
            }
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                drawFrames(gc, child, focus, offsetX, offsetY, depth + 1);
            }
        }

        void countWindows(UWindow* window, bool shown, int& total, int& visible)
        {
            total++;
            shown = shown && window->visible() && !window->destroyed;
            visible += shown ? 1 : 0;
            for (UWindow* child = window->firstChild(); child; child = child->nextSibling()) {
                countWindows(child, shown, total, visible);
            }
        }

        std::string windowLabel(UObject* window)
        {
            if (!window || window->destroyed) {
                return "None";
            }
            return window->cls ? window->cls->name.str() + " " + window->name.str() : window->name.str();
        }

        // The statistics the root window's ShowStats turns on (bShowStats, the WinStats cheat),
        // in the top left corner: how many windows there are and how many show, and which
        // window has the focus, the mouse and the grab.
        void drawStats(UGC* gc, URootWindow* root)
        {
            int total = 0;
            int visible = 0;
            countWindows(root, true, total, visible);
            std::string lines[] = {
                "Windows: " + std::to_string(total) + ", visible: " + std::to_string(visible),
                "Focus: " + windowLabel(rootwindow::FocusWindow(root)),
                "Mouse: " + windowLabel(rootwindow::lastMouseWindow(root)),
                "Grab: " + windowLabel(rootwindow::grabbedWindow(root)),
            };
            gc->resetState(root, 0, 0);
            gc->setStyle(DSTY_Normal);
            float y = 4;
            for (const std::string& line : lines) {
                gc->drawRect(2, y - 1, 300, 12, { 0, 0, 0, 255 }, BlendStyle::Normal);
                gc::TextColor(gc) = { 255, 255, 255, 255 };
                gc->drawText(4, y, 296, 12, line);
                y += 12;
            }
        }
    }

    void drawRoot(Canvas2D& canvas)
    {
        State& s = state();
        URootWindow* rootWindow = s.root;
        UGC* gc = sharedGC();
        if (!rootWindow || rootWindow->destroyed || !gc) {
            return;
        }
        s.viewportWidth = canvas.width();
        s.viewportHeight = canvas.height();
        ScopedValue<Canvas2D*> drawingTo(s.canvas, &canvas);
        rootWindow->updateLayout();
        float scale = virtualScale();
        gc->resetClip(0, 0, canvas.width() / scale, canvas.height() / scale);
        gc->savedStates.clear();
        UObject* raw = rootwindow::rawBackground(rootWindow);
        if (raw && !rootwindow::bRender(rootWindow)) {
            gc->resetState(rootWindow, 0, 0);
            gc->setStyle(DSTY_Normal);
            gc::tileColor(gc) = rootwindow::rawColor(rootWindow);
            float w = rootwindow::rawBackgroundWidth(rootWindow);
            float h = rootwindow::rawBackgroundHeight(rootWindow);
            gc->drawStretched(0, 0, canvas.width() / scale, canvas.height() / scale, 0, 0, w > 0 ? w : textureWidth(raw), h > 0 ? h : textureHeight(raw), raw);
        }
        drawTree(rootWindow, 0, 0);
        if (rootwindow::bShowFrames(rootWindow)) {
            gc->resetState(rootWindow, 0, 0);
            drawFrames(gc, rootWindow, rootwindow::FocusWindow(rootWindow), 0, 0, 0);
        }
        if (rootwindow::bShowStats(rootWindow)) {
            drawStats(gc, rootWindow);
        }
    }

    void render(Canvas2D& canvas)
    {
        drawRoot(canvas);
        releaseDestroyedWindows();
    }
}
