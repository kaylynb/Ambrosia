#include "core/objects.h"
#include "core/settings.h"
#include "deusex/deusex.h"
#include "deusex/script.h"
#include "engine/input.h"
#include "extension/windows.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dx::deusex {
    namespace {
        using namespace extension;

        class ConversationWindow : public UModalWindow {
        public:
            void tick(float delta) override
            {
                UModalWindow::tick(delta);
                if (!destroyed && visible() && centered != settings::get(settings::centeredConversations)) {
                    window::bNeedsReconfigure(this) = 1;
                }
            }

            void configurationChanged() override
            {
                // Let ConWindowActive position both bars and the camera first, including
                // the slide-in/out animation. Disabling our layout restores that geometry.
                UModalWindow::configurationChanged();
                centered = settings::get(settings::centeredConversations);
                auto* panel = cast<UTileWindow>(script::object(this, "lowerConWindow"));
                if (!panel) {
                    return;
                }
                panel->arrangeRows = nullptr;
                if (!centered) {
                    offset = overflow = 0;
                    reveal = true;
                    for (auto* row = panel->firstChild(); row; row = row->nextSibling()) {
                        if (auto* button = cast<UButtonWindow>(row)) {
                            button->hideReplyMarker = false;
                        }
                    }
                    return;
                }
                panel->arrangeRows = [](UTileWindow* tile) {
                    static_cast<ConversationWindow*>(tile->parent())->rows(tile, true);
                };
                // The letterbox keeps the height the conversation script gave it, so the
                // viewport and everything above it stay put. Only the rows scroll.
                float content = rows(panel, false);
                overflow = std::max(0.0f, content - panel->height());
                offset = std::clamp(offset, 0.0f, overflow);
            }

            void postDrawWindow(UGC* gc) override
            {
                UModalWindow::postDrawWindow(gc);
                if (destroyed) {
                    return;
                }
                auto* panel = cast<UTileWindow>(script::object(this, "lowerConWindow"));
                if (!centered || overflow <= 0.5f || !panel || panel->destroyed || !panel->visible()) {
                    return;
                }
                float x = gc->offsetX + panel->x() + panel->width() - 5;
                float top = gc->offsetY + panel->y() + 3;
                float bottom = gc->offsetY + panel->y() + panel->height() - 3;
                if (bottom <= top || panel->width() < 24) {
                    return;
                }
                // A slim scrollbar shows that the letterbox holds more than it shows.
                constexpr float width = 3;
                gc->drawRect(x, top, width, bottom - top, Color { 255, 255, 255, 48 }, BlendStyle::Normal);
                float track = bottom - top;
                float content = panel->height() + overflow;
                float thumb = std::min(track, std::max(12.0f, track * panel->height() / content));
                float travel = std::max(0.0f, track - thumb);
                float thumbY = top + (offset / overflow) * travel;
                gc->drawRect(x, thumbY, width, thumb, Color { 255, 255, 255, 190 }, BlendStyle::Normal);
                auto arrow = [&](float y, bool up) {
                    for (int i = 0; i < 3; i++) {
                        float w = 2.0f + i;
                        gc->drawRect(x + (width - w) * 0.5f, up ? y + i * 2 : y + (2 - i) * 2, w, 2, Color { 255, 255, 255, 180 }, BlendStyle::Normal);
                    }
                };
                if (offset > 0.5f) {
                    arrow(top + 1, true);
                }
                if (offset < overflow - 0.5f) {
                    arrow(bottom - 7, false);
                }
            }

            void focusEnteredDescendant(UWindow* child) override
            {
                UModalWindow::focusEnteredDescendant(child);
                if (centered) {
                    reveal = true;
                    window::bNeedsReconfigure(this) = 1;
                }
            }

            bool scrollContent(int direction) override
            {
                if (!centered || overflow <= 0) {
                    return false;
                }
                if (direction) {
                    offset = std::clamp(offset + static_cast<float>(direction) * step, 0.0f, overflow);
                    reveal = false;
                    window::bNeedsReconfigure(this) = 1;
                }
                return true;
            }

            bool virtualKeyPressed(uint8_t key, bool repeat) override
            {
                if ((key == IK_PageUp || key == IK_PageDown) && scrollContent(key == IK_PageUp ? -1 : 1)) {
                    return true;
                }
                return UModalWindow::virtualKeyPressed(key, repeat);
            }

        private:
            bool centered = false;
            bool reveal = true;
            float offset = 0;
            float overflow = 0;
            float step = 16;

            float rows(UTileWindow* panel, bool place)
            {
                float y = 2;
                std::vector<UTileWindow::Hold> placements;
                auto* focus = root() ? root()->focusWindow() : nullptr;
                for (auto* row = panel->firstChild(); row; row = row->nextSibling()) {
                    if (!row->visible()) {
                        continue;
                    }
                    auto* speech = script::object(row, "txtSpeech");
                    if (auto* button = cast<UButtonWindow>(row)) {
                        button->hideReplyMarker = true;
                    }
                    auto* font = window::normalFont(speech ? speech : row);
                    float em = std::max(8.0f, fontHeight(font));
                    step = 2 * em;
                    float width = std::max(1.0f, std::min(panel->width() - 12, 38 * em));
                    if (speech) {
                        float natural, h;
                        row->queryPreferredSize(natural, h);
                        width = std::clamp(std::ceil(natural), 1.0f, width);
                    }
                    float h = std::max(1.0f, std::ceil(row->queryPreferredHeight(width)));
                    if (place) {
                        placements.push_back({ row, std::round((panel->width() - width) / 2), y, width, h });
                        if (reveal && focus == row) {
                            offset = std::clamp(std::min(y, std::max(offset, y + h + 2 - panel->height())), 0.0f, overflow);
                        }
                    }
                    y += h + 2;
                }
                if (place) {
                    for (const auto& r : placements) {
                        r.window->configureChild(r.x, r.y - offset, r.width, r.height);
                    }
                    reveal = false;
                }
                // The last row ends two pixels before the cursor, not at it.
                return y - 2;
            }
        };
    }

    void registerConversationLayout()
    {
        gc::reportsReferences<ConversationWindow>();
        objects::registerFactory(Name("ConWindowActive"), [] { return new ConversationWindow(); });
    }
}
