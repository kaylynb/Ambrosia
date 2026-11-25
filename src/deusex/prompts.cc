#include "deusex/prompts.h"

#include "core/vm.h"
#include "deusex/glyphs.h"
#include "deusex/hud.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "extension/extension.h"

#include <cmath>

namespace dx::prompts {
    namespace {
        std::string labelOf(const Prompt& prompt)
        {
            return prompt.key.empty() ? prompt.label : prompt.key + "  " + prompt.label;
        }

        // A row of prompts on a translucent panel, centered across the canvas. Text is at the
        // game's own UI scale; glyphs a little taller, at whole multiples of their 16 pixels
        // so they stay crisp. place gets the panel's height and the gap between prompts and
        // returns the panel's top.
        template <typename Place>
        void drawRow(Canvas2D& canvas, const std::vector<Prompt>& prompts, UObject* player, bool menu, Place place)
        {
            UFont* font = ui::font("DeusExUI.FontMenuHeaders");
            if (prompts.empty() || !font) {
                return;
            }
            float scale = extension::uiScale();
            float lineHeight = textHeight(font, scale);
            float size = glyphs::size(lineHeight * 1.5f);
            float gap = std::round(size * 0.3f);
            float slashWidth = textWidth(font, "/", 0, scale);
            float alternativeGap = std::round(gap * 0.5f);
            // A keyboard prompt has no glyph, its key being spelt out in the label.
            auto glyphWidth = [size, gap, slashWidth, alternativeGap](const Prompt& prompt) {
                if (!prompt.key.empty()) {
                    return 0.0f;
                }
                float extra = gamepad::shown(prompt.action, gamepad::alternative(prompt.action)).button == gamepad::ButtonCount ? 0.0f : size + slashWidth + alternativeGap * 2;
                return size + extra + gap;
            };
            float total = 0;
            for (const Prompt& prompt : prompts) {
                total += prompt.label.empty() ? size + gap * 0.5f : glyphWidth(prompt) + textWidth(font, labelOf(prompt), 0, scale) + gap * 3;
            }
            total -= gap * 3;
            ui::Theme theme = ui::theme(player, menu);
            LinearColor panel = theme.panel;
            panel.a = 0.45f;
            float panelHeight = size + gap * 2;
            float x = std::round((canvas.width() - total) * 0.5f);
            float panelY = std::round(place(panelHeight, gap));
            float y = panelY + gap;
            canvas.drawRect(x - gap * 2, panelY, total + gap * 4, panelHeight, panel, BlendStyle::Normal);
            float textY = std::round(y + (size - lineHeight) * 0.5f);
            for (const Prompt& prompt : prompts) {
                if (prompt.key.empty()) {
                    glyphs::draw(canvas, glyphs::image(prompt.action), x, y, size, theme.text);
                    if (gamepad::Control alternative = gamepad::shown(prompt.action, gamepad::alternative(prompt.action)); alternative.button != gamepad::ButtonCount) {
                        float slashX = x + size + alternativeGap;
                        drawText(canvas, font, "/", slashX, textY, theme.text, BlendStyle::Masked, 0, scale);
                        glyphs::draw(canvas, glyphs::image(alternative), slashX + slashWidth + alternativeGap, y, size, theme.text);
                    }
                }
                if (prompt.label.empty()) {
                    x += size + gap * 0.5f;
                    continue;
                }
                std::string label = labelOf(prompt);
                x += glyphWidth(prompt);
                drawText(canvas, font, label, x, textY, theme.text, BlendStyle::Masked, 0, scale);
                x += textWidth(font, label, 0, scale) + gap * 3;
            }
        }
    }

    const char* interactionLabel(UObject* target, UObject* player)
    {
        if (script::isA(target, "Inventory")) {
            return "Pick up";
        }
        if (script::isA(target, "DeusExCarcass")) {
            // As DeusExCarcass.Frob: a body with nothing left on it is picked up instead, when
            // the player's hands are free. Animals and invincible bodies never are.
            bool carry = !script::object(target, "Inventory") && !script::object(player, "inHand") && !script::flag(target, "bAnimalCarcass") && !script::flag(target, "bInvincible");
            return carry ? "Pick up" : "Search";
        }
        if (script::isA(target, "MedicalBot") || script::isA(target, "RepairBot")) {
            return "Use";
        }
        if (script::isA(target, "ScriptedPawn")) {
            return "Talk";
        }
        // Doors say Use as well: whether one is open can't be told reliably from its key
        // frames, and many movers are lifts or panels rather than doors.
        return "Use";
    }

    void drawNotice(Canvas2D& canvas, const std::string& text)
    {
        UFont* font = ui::font("DeusExUI.FontMenuHeaders");
        if (text.empty() || !font) {
            return;
        }
        float scale = extension::uiScale();
        float lineHeight = textHeight(font, scale);
        float gap = std::round(lineHeight * 0.6f);
        float width = textWidth(font, text, 0, scale);
        ui::Theme theme = ui::theme(hud::viewer(), true);
        LinearColor panel = theme.panel;
        panel.a = 0.8f;
        float x = std::round((canvas.width() - width) * 0.5f);
        // At the top edge, over the title of a menu at most, clear of its buttons.
        float y = gap;
        canvas.drawRect(x - gap * 2, y - gap, width + gap * 4, lineHeight + gap * 2, panel, BlendStyle::Normal);
        drawText(canvas, font, text, x, y, theme.text, BlendStyle::Masked, 0, scale);
    }

    void drawBar(Canvas2D& canvas, const std::vector<Prompt>& prompts)
    {
        UObject* top = extension::topModal();
        // Centered under the screen's own content, clear of the inventory's belt; at the very
        // bottom when the screen leaves no room below it. The keypad draws its border past the
        // edge of its window, and the computer screens' buttons hang just below theirs, so both
        // get more room.
        float clearance = script::isA(top, "HUDKeypadWindow") || script::isA(top, "NetworkTerminal") ? 3.0f : 1.0f;
        drawRow(canvas, prompts, hud::viewer(), ui::isMenuWindow(top), [&canvas, top, clearance](float panelHeight, float gap) {
            float panelY = extension::contentBottom(top) + gap * clearance;
            return panelY + panelHeight > canvas.height() ? canvas.height() - panelHeight : panelY;
        });
    }

    void drawInteraction(Canvas2D& canvas, UObject* player)
    {
        UObject* target = script::object(player, "FrobTarget");
        if (!target || target->destroyed || vm::Call(player, Name("RestrictInput")).resultBool()) {
            return;
        }
        // The trace also selects hidden-in-plain-sight movers and script-operated doors.
        if (!vm::Call(player, Name("IsHighlighted")).obj(target).resultBool() || (script::isA(target, "DeusExMover") && !script::flag(target, "bFrobbable"))) {
            return;
        }
        // Clear of the crosshair and of the game's own box around the target.
        drawRow(canvas, { { gamepad::Action::Interact, interactionLabel(target, player) } }, player, false, [&canvas](float, float) {
            return canvas.height() * 0.66f;
        });
    }

}
