#include "debugui/world_draw.h"

#include "engine/world.h"

#include <algorithm>
#include <cmath>

namespace dx::debugui::draw {
    namespace {
        // How near the view a line is cut, in level units.
        constexpr float Near = 4.0f;

        ImU32 shadow(float alpha)
        {
            return IM_COL32(0, 0, 0, static_cast<int>(220.0f * alpha));
        }

        float alphaOf(ImU32 colour)
        {
            return static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
        }
    }

    ImU32 faded(ImU32 colour, float alpha)
    {
        float a = alphaOf(colour) * std::clamp(alpha, 0.0f, 1.0f);
        return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
    }

    void shadowedText(ImDrawList* list, ImVec2 at, ImU32 colour, const std::string& text)
    {
        list->AddText(ImVec2(at.x + 1, at.y + 1), shadow(alphaOf(colour)), text.c_str());
        list->AddText(at, colour, text.c_str());
    }

    float pixelsPerUnit(const Frame& frame, const Vector& at)
    {
        Coords axes = rotationAxes(frame.viewRotation);
        float ahead = (at - frame.viewLocation).dot(axes.xAxis);
        if (ahead <= 1.0f) {
            return 0;
        }
        float scale = ImGui::GetIO().DisplaySize.x * 0.5f / std::tan(frame.fov * Pi / 360.0f);
        return scale / ahead;
    }

    bool seen(const Frame& frame, const Vector& at)
    {
        return !frame.world || frame.world->fastLineCheck(at, frame.viewLocation);
    }

    bool line(const Frame& frame, const Vector& from, const Vector& to, ImU32 colour, float thickness)
    {
        Coords axes = rotationAxes(frame.viewRotation);
        Vector a = from;
        Vector b = to;
        float aheadA = (a - frame.viewLocation).dot(axes.xAxis);
        float aheadB = (b - frame.viewLocation).dot(axes.xAxis);
        if (aheadA <= Near && aheadB <= Near) {
            return false;
        }
        if (aheadA <= Near) {
            a = a + (b - a) * ((Near - aheadA) / (aheadB - aheadA));
        } else if (aheadB <= Near) {
            b = b + (a - b) * ((Near - aheadB) / (aheadA - aheadB));
        }
        float x0 = 0;
        float y0 = 0;
        float x1 = 0;
        float y1 = 0;
        if (!toScreen(frame, a, x0, y0) || !toScreen(frame, b, x1, y1)) {
            return false;
        }
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), shadow(alphaOf(colour)), thickness + 3.0f);
        list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), colour, thickness);
        return true;
    }

    bool marker(const Frame& frame, const Vector& at, ImU32 colour, bool filled, ImVec2& screen, float& radius)
    {
        float x = 0;
        float y = 0;
        if (!toScreen(frame, at, x, y)) {
            return false;
        }
        radius = std::clamp(24.0f * pixelsPerUnit(frame, at), 7.0f, 18.0f);
        screen = ImVec2(x, y);
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        ImU32 edge = shadow(alphaOf(colour));
        float arm = radius * 2.2f;
        list->AddLine(ImVec2(x - arm, y), ImVec2(x + arm, y), edge, 4.0f);
        list->AddLine(ImVec2(x, y - arm), ImVec2(x, y + arm), edge, 4.0f);
        list->AddLine(ImVec2(x - arm, y), ImVec2(x + arm, y), colour, 2.0f);
        list->AddLine(ImVec2(x, y - arm), ImVec2(x, y + arm), colour, 2.0f);
        if (filled) {
            list->AddCircleFilled(screen, radius, colour);
            list->AddCircle(screen, radius, edge, 0, 2.0f);
        } else {
            list->AddCircle(screen, radius, edge, 0, 5.0f);
            list->AddCircle(screen, radius, colour, 0, 3.0f);
        }
        return true;
    }

    void markerLabel(ImVec2 screen, float radius, int row, ImU32 colour, const std::string& text)
    {
        ImVec2 at(screen.x + radius + 6, screen.y - radius - 16 - static_cast<float>(row) * 16.0f);
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        list->AddLine(screen, ImVec2(at.x, at.y + 14), colour, 1.0f);
        shadowedText(list, at, colour, text);
    }

    void cylinder(const Frame& frame, const Vector& centre, float radius, float halfHeight, ImU32 colour, float thickness)
    {
        constexpr int Sides = 16;
        Vector top { 0, 0, halfHeight };
        for (int i = 0; i < Sides; i++) {
            float a0 = static_cast<float>(i) * 2.0f * Pi / Sides;
            float a1 = static_cast<float>(i + 1) * 2.0f * Pi / Sides;
            Vector p0 = centre + Vector { radius * std::cos(a0), radius * std::sin(a0), 0 };
            Vector p1 = centre + Vector { radius * std::cos(a1), radius * std::sin(a1), 0 };
            line(frame, p0 + top, p1 + top, colour, thickness);
            line(frame, p0 - top, p1 - top, colour, thickness);
            if (i % (Sides / 4) == 0) {
                line(frame, p0 - top, p0 + top, colour, thickness);
            }
        }
    }
}
