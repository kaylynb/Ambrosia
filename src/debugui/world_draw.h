#pragma once

#include "core/math.h"
#include "debugui/overlay.h"

#include <imgui.h>

#include <string>

// Shapes drawn over the level for the developer overlay's tools: lines, markers, labels and
// collision cylinders placed in level coordinates and drawn into ImGui's background draw list.
// For use inside an Overlay drawer (Overlay::addDrawer), where Frame says where the view is.
namespace dx::debugui::draw {
    // A colour with its alpha scaled, as for fading a shape that is behind a wall.
    ImU32 faded(ImU32 colour, float alpha);

    // Text with a dark shadow, readable over bright and dark scenes alike.
    void shadowedText(ImDrawList* list, ImVec2 at, ImU32 colour, const std::string& text);

    // Pixels per level unit at a point in front of the view; 0 behind it.
    float pixelsPerUnit(const Frame& frame, const Vector& at);

    // Whether nothing in the level's geometry is between the view and a point.
    bool seen(const Frame& frame, const Vector& at);

    // A line between two points in the level, cut where it passes behind the view, with a dark
    // edge. False when the whole line is behind the view.
    bool line(const Frame& frame, const Vector& from, const Vector& to, ImU32 colour, float thickness);

    // A marker: a crosshair reaching past a disc, filled or hollow, sized for about 24 units in
    // the level but kept readable far off and up close. Returns where it was drawn, and false when
    // the point is behind the view.
    bool marker(const Frame& frame, const Vector& at, ImU32 colour, bool filled, ImVec2& screen, float& radius);

    // A label beside a marker drawn at screen with radius, with a thin leader line to it; row
    // lifts it by a line, so labels of markers side by side can be staggered.
    void markerLabel(ImVec2 screen, float radius, int row, ImU32 colour, const std::string& text);

    // An upright cylinder the way the engine's actor collision is: centred on the actor, with a
    // radius and a half height. Drawn as its top and bottom rings and four sides.
    void cylinder(const Frame& frame, const Vector& centre, float radius, float halfHeight, ImU32 colour, float thickness);
}
