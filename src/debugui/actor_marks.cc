#include "debugui/actor_marks.h"

#include "core/objects.h"
#include "core/text.h"
#include "debugui/imgui_setup.h"
#include "debugui/world_draw.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        struct Selector {
            std::string text;
            std::string pattern;
            std::vector<std::pair<std::string, std::string>> where;
        };

        struct Mark {
            Selector selector;
            ImU32 colour = 0;
        };

        // Yellow, magenta, cyan, green, orange, blue, then round again.
        const ImU32 palette[] = {
            IM_COL32(255, 217, 26, 255),
            IM_COL32(255, 51, 230, 255),
            IM_COL32(51, 230, 255, 255),
            IM_COL32(90, 255, 90, 255),
            IM_COL32(255, 140, 26, 255),
            IM_COL32(110, 140, 255, 255),
        };

        std::vector<Mark>& marks()
        {
            static std::vector<Mark> list;
            return list;
        }

        bool parse(const std::string& text, Selector& selector)
        {
            std::istringstream words(text);
            std::string word;
            if (!(words >> selector.pattern)) {
                return false;
            }
            while (words >> word) {
                size_t equals = word.find('=');
                if (equals == std::string::npos || equals == 0) {
                    return false;
                }
                selector.where.emplace_back(word.substr(0, equals), word.substr(equals + 1));
            }
            selector.text = text;
            return true;
        }

        bool propertyMatches(UObject* actor, const std::string& name, const std::string& want)
        {
            UProperty* property = actor->cls->findProperty(Name(name));
            if (!property) {
                return false;
            }
            const uint8_t* memory = actor->data + property->offset;
            if (iequals(property->toText(memory), want)) {
                return true;
            }
            if (property->type == PropType::Byte) {
                // An enum also by its number.
                return want == std::to_string(*memory);
            }
            if (property->type == PropType::Object || property->type == PropType::Class) {
                const UObject* object = *reinterpret_cast<UObject* const*>(memory);
                return object && iequals(object->name.str(), want);
            }
            return false;
        }

        bool matches(UObject* actor, const Selector& selector)
        {
            if (!actor || actor->destroyed || !actor->data || !actor->cls) {
                return false;
            }
            if (selector.pattern != "*" && !iequals(actor->name.str(), selector.pattern) && !actor->isA(Name(selector.pattern))) {
                return false;
            }
            for (const auto& [name, want] : selector.where) {
                if (!propertyMatches(actor, name, want)) {
                    return false;
                }
            }
            return true;
        }

        std::string enumText(UObject* actor, const char* name)
        {
            UProperty* property = actor->cls->findProperty(Name(name));
            return property ? property->toText(actor->data + property->offset) : std::string();
        }

        void drawActor(const Frame& frame, UObject* actor, ImU32 colour, int row)
        {
            Vector location = actor::Location(actor);
            bool visible = draw::seen(frame, location);
            ImU32 shown = visible ? colour : draw::faded(colour, 0.45f);
            float radius = actor::CollisionRadius(actor);
            float height = actor::CollisionHeight(actor);
            bool collides = actor::bCollideActors(actor) != 0;
            if (radius > 0 && height > 0) {
                draw::cylinder(frame, location, radius, height, collides ? shown : draw::faded(shown, 0.5f), collides ? 2.0f : 1.0f);
            }
            std::string label = actor->name.str() + " (" + actor->cls->name.str() + ")";
            bool filled = true;
            uint8_t lightType = actor::LightType(actor);
            if (lightType != 0) {
                float level = 0;
                float time = 0;
                if (UObject* info = actor::Level(actor); info && info->data) {
                    time = levelinfo::TimeSeconds(info);
                }
                lightColor(actor, time, &level);
                bool off = actor::LightBrightness(actor) == 0;
                filled = !off && level > 0;
                label += ", " + enumText(actor, "LightType") + (off ? " off (brightness 0)" : (filled ? " lit" : " dark"));
            }
            // A Tag is the class name unless the level set one, for events to trigger it by.
            if (Name tag = actor::Tag(actor); tag != Name() && tag != actor->cls->name) {
                label += ", tag " + tag.str();
            }
            if (UObject* sound = actor::AmbientSound(actor)) {
                label += ", sound " + sound->name.str();
            }
            char hitbox[64];
            if (radius > 0 && height > 0) {
                std::snprintf(hitbox, sizeof(hitbox), ", hitbox %.0fx%.0f%s", static_cast<double>(radius), static_cast<double>(height) * 2.0, collides ? "" : " (not colliding)");
                label += hitbox;
            }
            ImVec2 screen;
            float size = 0;
            if (draw::marker(frame, location, shown, filled, screen, size)) {
                draw::markerLabel(screen, size, row, shown, label);
            }
        }

        void drawMarks(const Frame& frame)
        {
            if (!frame.world || marks().empty()) {
                return;
            }
            int row = 0;
            for (UObject* actor : frame.world->actors) {
                for (const Mark& mark : marks()) {
                    if (matches(actor, mark.selector)) {
                        drawActor(frame, actor, mark.colour, row++ % 4);
                        break;
                    }
                }
            }
        }

        std::string listMarks(World* world)
        {
            if (marks().empty()) {
                return "no marks";
            }
            std::string text;
            for (const Mark& mark : marks()) {
                text += (text.empty() ? "" : "; ") + mark.selector.text + " (" + std::to_string(findActors(world, mark.selector.text).size()) + ")";
            }
            return text;
        }

        struct MarksTool {
            char input[256] = "";
            std::string reply;
        };

        void drawTool(MarksTool& tool, const Frame& frame)
        {
            bool add = ImGui::InputText("##selector", tool.input, sizeof(tool.input), ImGuiInputTextFlags_EnterReturnsTrue);
            itemHelp("Marks actors by name, by class, or * for any, with Property=value filters, e.g. \"* LightType=LT_Flicker\". The cylinder is the actor's collision (its hitbox), thin when it does not collide.");
            ImGui::SameLine();
            add = ImGui::Button("Mark") || add;
            if (add && tool.input[0]) {
                tool.reply = addMark(frame.world, tool.input);
                tool.input[0] = 0;
            }
            if (!tool.reply.empty()) {
                ImGui::TextDisabled("%s", tool.reply.c_str());
            }
            ImGui::Separator();
            std::string removed;
            for (const Mark& mark : marks()) {
                ImGui::PushID(&mark);
                ImGui::ColorButton("##colour", ImGui::ColorConvertU32ToFloat4(mark.colour), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                ImGui::SameLine();
                ImGui::Text("%s (%zu)", mark.selector.text.c_str(), findActors(frame.world, mark.selector.text).size());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    removed = mark.selector.text;
                }
                ImGui::PopID();
            }
            if (!removed.empty()) {
                tool.reply = removeMark(removed);
            }
            if (!marks().empty() && ImGui::Button("Remove all")) {
                tool.reply = removeMark("all");
            }
        }
    }

    std::string addMark(World* world, const std::string& selector)
    {
        Mark mark;
        if (!parse(selector, mark.selector)) {
            return "mark takes <name, class or *> [<Property>=<value> ...], not \"" + selector + "\"";
        }
        for (const Mark& existing : marks()) {
            if (iequals(existing.selector.text, selector)) {
                return "already marked: " + selector;
            }
        }
        mark.colour = palette[marks().size() % std::size(palette)];
        marks().push_back(mark);
        return "marked " + std::to_string(findActors(world, selector).size()) + " actors for " + selector;
    }

    std::string removeMark(const std::string& selector)
    {
        if (iequals(selector, "all")) {
            size_t count = marks().size();
            marks().clear();
            return "removed " + std::to_string(count) + " marks";
        }
        auto found = std::find_if(marks().begin(), marks().end(), [&](const Mark& mark) {
            return iequals(mark.selector.text, selector);
        });
        if (found == marks().end()) {
            return "no mark " + selector;
        }
        marks().erase(found);
        return "removed mark " + selector;
    }

    std::vector<UObject*> findActors(World* world, const std::string& selector)
    {
        std::vector<UObject*> found;
        Selector parsed;
        if (!world || !parse(selector, parsed)) {
            return found;
        }
        for (UObject* actor : world->actors) {
            if (matches(actor, parsed)) {
                found.push_back(actor);
            }
        }
        return found;
    }

    void addActorMarks(Overlay& overlay, std::function<World*()> currentWorld)
    {
        auto tool = std::make_shared<MarksTool>();
        Tool entry;
        entry.menu = "Game";
        entry.title = "Marks";
        entry.name = "marks";
        entry.width = 640;
        entry.height = 360;
        entry.draw = [tool](const Frame& frame) {
            drawTool(*tool, frame);
        };
        overlay.addTool(std::move(entry));
        overlay.addDrawer(drawMarks);
        overlay.addCommand("mark", [currentWorld](const std::string& value) {
            return addMark(currentWorld(), value);
        });
        overlay.addCommand("unmark", [](const std::string& value) {
            return removeMark(value);
        });
        overlay.addCommand("marks", [currentWorld](const std::string&) {
            return listMarks(currentWorld());
        });
    }
}
