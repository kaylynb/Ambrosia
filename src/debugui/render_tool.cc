#include "debugui/render_tool.h"

#include "debugui/actor_marks.h"
#include "debugui/imgui_setup.h"
#include "debugui/world_draw.h"
#include "core/math.h"
#include "core/settings.h"
#include "engine/actor.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "render/render_debug.h"

#include <imgui.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        // "DebugOverlay light-markers on" marks every flicker and strobe light (actor_marks.h).
        const char* const lightSelectors[] = { "* LightType=LT_Flicker", "* LightType=LT_Strobe" };
        bool lightMarkers = false;

        void setLightMarkers(bool on)
        {
            lightMarkers = on;
            for (const char* selector : lightSelectors) {
                if (on) {
                    addMark(nullptr, selector);
                } else {
                    removeMark(selector);
                }
            }
        }

        // "DebugOverlay mirror-markers on" outlines every level surface and mover face with the
        // Mirror flag (PF_Mirrored), with a label per surface: what a mirror view is drawn
        // through, including polished floors and mirrored windows.
        bool mirrorMarkers = false;
        constexpr uint32_t PF_Invisible = 0x00000001;
        constexpr uint32_t PF_Mirrored = 0x08000000;
        const ImU32 mirrorColour = IM_COL32(51, 230, 255, 255);

        // One mirror surface's pieces, in world space, just off the surface so the outline is not
        // hidden by it.
        struct MirrorOutline {
            std::string label;
            Vector normal;
            std::vector<std::vector<Vector>> pieces;
        };

        // Draws only the surface's outer edges: an edge two of its pieces share is a BSP cut
        // across the mirror, not its border. Nothing is filled, so the reflection stays clear,
        // and the label hangs off the outline's top corner rather than its middle.
        void drawOutline(const Frame& frame, const MirrorOutline& outline, int row)
        {
            auto key = [](const Vector& point) {
                return std::to_string(static_cast<int64_t>(std::lround(point.x * 4))) + "," + std::to_string(static_cast<int64_t>(std::lround(point.y * 4))) + "," + std::to_string(static_cast<int64_t>(std::lround(point.z * 4)));
            };
            std::unordered_map<std::string, int> uses;
            for (const std::vector<Vector>& piece : outline.pieces) {
                for (size_t i = 0; i < piece.size(); i++) {
                    std::string a = key(piece[i]);
                    std::string b = key(piece[(i + 1) % piece.size()]);
                    uses[a < b ? a + "|" + b : b + "|" + a]++;
                }
            }
            Vector centre;
            size_t count = 0;
            for (const std::vector<Vector>& piece : outline.pieces) {
                for (const Vector& point : piece) {
                    centre += point;
                    count++;
                }
            }
            if (count == 0) {
                return;
            }
            centre = centre / static_cast<float>(count);
            bool facing = (frame.viewLocation - centre).dot(outline.normal) > 0;
            bool visible = facing && draw::seen(frame, centre + outline.normal * 2.0f);
            ImU32 colour = visible ? mirrorColour : draw::faded(mirrorColour, 0.35f);
            bool anchored = false;
            ImVec2 anchor;
            for (const std::vector<Vector>& piece : outline.pieces) {
                for (size_t i = 0; i < piece.size(); i++) {
                    const Vector& a = piece[i];
                    const Vector& b = piece[(i + 1) % piece.size()];
                    std::string ka = key(a);
                    std::string kb = key(b);
                    if (uses[ka < kb ? ka + "|" + kb : kb + "|" + ka] > 1) {
                        continue;
                    }
                    draw::line(frame, a, b, colour, visible ? 2.0f : 1.0f);
                    float x = 0;
                    float y = 0;
                    if (toScreen(frame, a, x, y) && (!anchored || y < anchor.y)) {
                        anchor = ImVec2(x, y);
                        anchored = true;
                    }
                }
            }
            if (anchored) {
                draw::markerLabel(anchor, 0.0f, row % 4, colour, outline.label);
            }
        }

        void drawMirrorMarkers(const Frame& frame)
        {
            if (!mirrorMarkers || !frame.world || !frame.world->level || !frame.world->level->model) {
                return;
            }
            UModel* model = frame.world->level->model;
            std::unordered_map<int32_t, MirrorOutline> surfaces;
            for (const BspNode& node : model->nodes) {
                if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= model->surfs.size()) {
                    continue;
                }
                const BspSurf& surf = model->surfs[static_cast<size_t>(node.surf)];
                if (!(surf.polyFlags & PF_Mirrored) || (surf.polyFlags & PF_Invisible)) {
                    continue;
                }
                Vector normal = model->vectors[static_cast<size_t>(surf.vNormal)].normal();
                MirrorOutline& outline = surfaces[node.surf];
                if (outline.label.empty()) {
                    const char* kind = normal.z > 0.7f ? "floor" : (normal.z < -0.7f ? "ceiling" : "wall");
                    outline.label = std::string("mirror ") + kind + ", " + (surf.texture ? surf.texture->name.str() : std::string("no texture")) + ", surf " + std::to_string(node.surf);
                    outline.normal = normal;
                }
                std::vector<Vector>& piece = outline.pieces.emplace_back();
                for (uint8_t i = 0; i < node.numVertices; i++) {
                    piece.push_back(model->points[static_cast<size_t>(model->verts[static_cast<size_t>(node.vertPool) + i].vertex)] + normal * 1.0f);
                }
            }
            int row = 0;
            for (const auto& [surfIndex, outline] : surfaces) {
                drawOutline(frame, outline, row++);
            }
            // Mirror faces on movers, where they are now.
            for (UObject* actor : frame.world->actors) {
                if (!actor || actor->destroyed || !actor->data || !actor->isA(Name("Mover"))) {
                    continue;
                }
                UModel* brush = cast<UModel>(actor::Brush(actor));
                if (!brush || !brush->polys) {
                    continue;
                }
                Coords axes = rotationAxes(actor::Rotation(actor));
                Vector scale = *reinterpret_cast<Vector*>(brush::MainScale(actor));
                if (scale.x == 0 && scale.y == 0 && scale.z == 0) {
                    scale = { 1, 1, 1 };
                }
                Vector location = actor::Location(actor);
                Vector prePivot = actor::PrePivot(actor);
                for (size_t index = 0; index < brush->polys->polys.size(); index++) {
                    const Poly& poly = brush->polys->polys[index];
                    if (poly.vertices.size() < 3 || !(poly.polyFlags & PF_Mirrored) || (poly.polyFlags & PF_Invisible)) {
                        continue;
                    }
                    MirrorOutline outline;
                    outline.normal = untransformVector(axes, poly.normal).normal();
                    outline.label = std::string("mirror on mover ") + actor->name.str() + ", " + (poly.texture ? poly.texture->name.str() : std::string("no texture")) + ", face " + std::to_string(index);
                    std::vector<Vector>& piece = outline.pieces.emplace_back();
                    for (const Vector& point : poly.vertices) {
                        piece.push_back(location + untransformVector(axes, (point - prePivot) * scale) + outline.normal * 1.0f);
                    }
                    drawOutline(frame, outline, row++);
                }
            }
        }

        struct Stage {
            const char* name;
            const char* label;
            bool RenderDebug::* flag;
            const char* help;
        };

        const Stage stages[] = {
            { "mirrors", "Mirrors", &RenderDebug::mirrors, "Off, a mirror surface draws as its own texture." },
            { "sky", "Sky zone", &RenderDebug::sky, "Off, fake backdrop surfaces draw as their own texture." },
            { "lightmap-animation", "Lightmap animation", &RenderDebug::lightmapAnimation, "Off, animated lightmaps keep the frame they had." },
            { "procedural", "Procedural texture updates", &RenderDebug::proceduralTextures, "Off, fire, water and wet textures keep the frame they had." },
            { "meshes", "Actor meshes", &RenderDebug::actorMeshes, "Off, mesh actors are not drawn; sprites, movers and decals still are." },
            { "translucent", "Translucent pass", &RenderDebug::translucent, "Off, translucent and modulated actors and surfaces are not drawn." },
            { "occlusion", "Occlusion queries", &RenderDebug::occlusionQueries, "Off, actors hidden behind others are drawn too, with no queries." },
            { "coronas", "Coronas", &RenderDebug::coronas, "" },
            { "post", "Post chain", &RenderDebug::postChain, "Off, all of its parts are off." },
            { "gamma", "Brightness (gamma) pass", &RenderDebug::gamma, "Off, the brightness setting has no effect." },
            { "hud", "HUD script drawing", &RenderDebug::hudScripts, "PreRender, RenderOverlays, PostRenderFlash and PostRender; the console still draws." },
        };

        // The post chain's parts are the video settings themselves (the Graphics and Display screens), so
        // a change here is saved and shows in the menu. Switching one on from the console
        // brings back the level it had, or a middle one.
        struct SettingSwitch {
            const char* name;
            const char* label;
            const settings::Int* level;
            const settings::Bool* toggle;
            std::vector<const char*> levels;
            int onLevel;
        };

        const SettingSwitch settingSwitches[] = {
            { "msaa", "Multisampling", &settings::multisampling, nullptr, { "Off", "2x", "4x", "8x" }, 2 },
            { "ao", "Ambient occlusion", &settings::ambientOcclusion, nullptr, { "Off", "Low", "Medium", "High" }, 2 },
            { "fxaa", "FXAA", nullptr, &settings::fxaa, {}, 1 },
        };

        void setSettingSwitch(const SettingSwitch& entry, bool on)
        {
            if (entry.toggle) {
                settings::set(*entry.toggle, on);
                return;
            }
            static std::unordered_map<const settings::Int*, int> lastOn;
            int current = settings::get(*entry.level);
            if (current != 0) {
                lastOn[entry.level] = current;
            }
            if (!on) {
                settings::set(*entry.level, 0);
            } else if (current == 0) {
                auto found = lastOn.find(entry.level);
                settings::set(*entry.level, found != lastOn.end() ? found->second : entry.onLevel);
            }
        }

        void settingControls()
        {
            for (const SettingSwitch& entry : settingSwitches) {
                ImGui::BeginDisabled(!renderDebug.postChain);
                if (entry.toggle) {
                    bool value = settings::get(*entry.toggle);
                    if (ImGui::Checkbox(entry.label, &value)) {
                        setSettingSwitch(entry, value);
                    }
                } else {
                    int value = settings::get(*entry.level);
                    ImGui::SetNextItemWidth(scaled(120));
                    if (ImGui::Combo(entry.label, &value, entry.levels.data(), static_cast<int>(entry.levels.size()))) {
                        settings::set(*entry.level, value);
                    }
                }
                ImGui::EndDisabled();
                std::string help = std::string("The video setting itself, saved as the menu saves it.\nDebugOverlay render-") + entry.name + " on|off";
                ImGui::SetItemTooltip("%s", help.c_str());
            }
        }

        void counter(const char* label, uint32_t value)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%u", value);
        }

        void drawWindow()
        {
            if (ImGui::Button("Everything on")) {
                renderDebug = RenderDebug {};
            }
            ImGui::SameLine();
            if (ImGui::Button("Everything off")) {
                for (const Stage& stage : stages) {
                    renderDebug.*stage.flag = false;
                }
            }
            helpMarker("Leaves render stages out, for finding what a frame costs. Every switch starts on and none is saved. Leaving a stage out can change actors' LastRenderTime, which some scripts read. Each switch is also a console command: DebugOverlay render-<name> on|off.");
            ImGui::Separator();
            for (const Stage& stage : stages) {
                ImGui::Checkbox(stage.label, &(renderDebug.*stage.flag));
                std::string help = std::string(stage.help) + (stage.help[0] ? "\n" : "") + "DebugOverlay render-" + stage.name;
                ImGui::SetItemTooltip("%s", help.c_str());
                if (stage.flag == &RenderDebug::postChain) {
                    ImGui::Indent();
                    settingControls();
                    ImGui::Unindent();
                }
            }
            bool markers = lightMarkers;
            if (ImGui::Checkbox("Flicker and strobe light markers", &markers)) {
                setLightMarkers(markers);
            }
            itemHelp("Marks flickering and strobing lights in the Marks tool, filled while lit.\nDebugOverlay light-markers");
            ImGui::Checkbox("Mirror surface markers", &mirrorMarkers);
            itemHelp("Outlines every surface and mover face with the Mirror flag.\nDebugOverlay mirror-markers");
            ImGui::Separator();
            ImGui::TextUnformatted("Last frame");
            helpMarker("Summed over the main view, mirrors and the sky. Draw calls count the scene, the HUD canvas and full-screen passes; a multi-draw counts once.");
            if (ImGui::BeginTable("counters", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                const RenderCounters& last = lastRenderCounters;
                counter("Visible surfaces", last.visibleSurfaces);
                counter("Visible actors", last.visibleActors);
                counter("Draw calls", last.drawCalls);
                counter("Triangles", last.triangles);
                counter("Texture uploads", last.textureUploads);
                counter("Procedural texture updates", last.proceduralUpdates);
                counter("Lightmap updates", last.lightmapUpdates);
                ImGui::EndTable();
            }
        }
    }

    void addRenderTool(Overlay& overlay)
    {
        Tool tool;
        tool.menu = "Render";
        tool.title = "Render";
        tool.name = "render";
        tool.width = 760;
        tool.height = 640;
        tool.draw = [](const Frame&) {
            drawWindow();
        };
        overlay.addTool(std::move(tool));
        for (const Stage& stage : stages) {
            bool RenderDebug::* flag = stage.flag;
            overlay.addSwitch(std::string("render-") + stage.name, [flag](bool on) {
                renderDebug.*flag = on;
            });
        }
        for (const SettingSwitch& entry : settingSwitches) {
            const SettingSwitch* chosen = &entry;
            overlay.addSwitch(std::string("render-") + entry.name, [chosen](bool on) {
                setSettingSwitch(*chosen, on);
            });
        }
        overlay.addSwitch("light-markers", setLightMarkers);
        overlay.addSwitch("mirror-markers", [](bool on) {
            mirrorMarkers = on;
        });
        overlay.addDrawer(drawMirrorMarkers);
        overlay.addCommand("render-reset", [](const std::string&) {
            renderDebug = RenderDebug {};
            return std::string("every render stage on");
        });
    }
}
