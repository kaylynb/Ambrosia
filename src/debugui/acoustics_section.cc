#include "debugui/acoustics_section.h"
#include "debugui/imgui_setup.h"

#include "audio/acoustic_scene.h"
#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/objects.h"
#include "core/settings.h"
#include "engine/resources.h"
#include "render/scene_renderer.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        // Renderer layers this section draws in.
        constexpr int MeshLayer = 0;
        constexpr int PickLayer = 1;
        constexpr int MoverLayer = 3;
        constexpr int ProbeLayer = 4;
        constexpr int PathLayer = 5;

        // One colour per material table entry, in table order, repeating if the table grows.
        const std::array<std::array<float, 3>, 18> palette = { {
            { 0.60f, 0.60f, 0.60f }, // generic
            { 0.35f, 0.60f, 1.00f }, // metal
            { 0.70f, 0.55f, 0.40f }, // stone
            { 0.80f, 0.80f, 0.70f }, // concrete
            { 0.85f, 0.50f, 0.15f }, // wood
            { 0.85f, 0.25f, 0.20f }, // brick
            { 0.30f, 1.00f, 1.00f }, // glass
            { 1.00f, 1.00f, 0.30f }, // fixture
            { 0.85f, 0.30f, 0.85f }, // textile
            { 1.00f, 0.95f, 0.85f }, // paper
            { 0.25f, 0.85f, 0.25f }, // foliage
            { 0.50f, 0.35f, 0.15f }, // earth
            { 0.95f, 0.75f, 0.85f }, // ceramic
            { 0.60f, 0.85f, 0.75f }, // tiles
            { 0.90f, 0.85f, 0.60f }, // plaster
            { 0.15f, 0.35f, 0.90f }, // water
            { 0.55f, 0.95f, 0.55f }, // grate
            { 0.75f, 0.60f, 1.00f }, // ceiling
        } };

        const std::array<float, 3>& colour(size_t material)
        {
            return palette[material % palette.size()];
        }

        // Baked acoustics: what is drawn for them, apart from the rest of the section.
        struct BakedView {
            bool showProbes = false;
            bool throughWalls = true;
            // The voice whose path is drawn, or 0; followLoudest picks the loudest voice with a
            // pathed sound each frame instead.
            int32_t traced = 0;
            bool followLoudest = false;
            std::shared_ptr<const audio::acoustics::BakedData> drawnProbes;
            bool drawnThrough = false;
            bool pathDrawn = false;
            SceneRenderer* renderer = nullptr;

            void clearProbes()
            {
                if (renderer && drawnProbes) {
                    renderer->setDebugMeshes(ProbeLayer, {});
                }
                drawnProbes.reset();
            }

            void clearPath()
            {
                if (renderer && pathDrawn) {
                    renderer->setDebugMeshes(PathLayer, {});
                }
                pathDrawn = false;
            }

            void clear()
            {
                clearProbes();
                clearPath();
                if (traced != 0) {
                    audio::setTracedVoice(0);
                }
            }
        };

        struct ViewState {
            bool showMesh = false;
            bool edges = true;
            bool throughWalls = false;
            bool pick = true;
            float opacity = 0.35f;
            // What was last handed to the renderer, to rebuild only on change.
            std::shared_ptr<const audio::acoustics::Geometry> drawn;
            bool drawnEdges = false;
            bool drawnThrough = false;
            float drawnOpacity = 0;
            std::shared_ptr<const audio::acoustics::Geometry> pickedGeometry;
            int32_t pickedSurface = -1;
            bool pickedWithMesh = false;
            float pickedOpacity = 0;
            std::vector<audio::acoustics::MoverPose> drawnPoses;
            bool moversDrawn = false;
            SceneRenderer* renderer = nullptr;
            std::string tableText;
            BakedView baked;

            void clearMesh()
            {
                if (renderer && drawn) {
                    renderer->setDebugMeshes(MeshLayer, {});
                }
                if (renderer && moversDrawn) {
                    renderer->setDebugMeshes(MoverLayer, {});
                }
                drawn.reset();
                moversDrawn = false;
                drawnPoses.clear();
            }

            void clearPick()
            {
                if (renderer && pickedSurface >= 0) {
                    renderer->setDebugMeshes(PickLayer, {});
                }
                pickedSurface = -1;
                pickedGeometry.reset();
            }

            void clear()
            {
                clearMesh();
                clearPick();
                baked.clear();
            }
        };

        void drawMesh(ViewState& view, const std::shared_ptr<const audio::acoustics::Geometry>& geometry)
        {
            if (geometry == view.drawn && view.edges == view.drawnEdges && view.throughWalls == view.drawnThrough && view.opacity == view.drawnOpacity) {
                return;
            }
            std::vector<SceneRenderer::DebugMesh> meshes(audio::acoustics::defaultMaterials().size());
            for (size_t i = 0; i < meshes.size(); i++) {
                meshes[i].r = colour(i)[0];
                meshes[i].g = colour(i)[1];
                meshes[i].b = colour(i)[2];
                meshes[i].a = view.opacity;
                meshes[i].throughWalls = view.throughWalls;
            }
            for (size_t t = 0; t < geometry->triangles.size(); t++) {
                size_t material = static_cast<size_t>(geometry->triangleMaterial[t]);
                if (material >= meshes.size()) {
                    continue;
                }
                SceneRenderer::DebugMesh& mesh = meshes[material];
                const std::array<int32_t, 3>& triangle = geometry->triangles[t];
                for (int32_t index : triangle) {
                    mesh.triangles.push_back(geometry->vertices[static_cast<size_t>(index)]);
                }
                if (view.edges) {
                    for (size_t e = 0; e < 3; e++) {
                        mesh.lines.push_back(geometry->vertices[static_cast<size_t>(triangle[e])]);
                        mesh.lines.push_back(geometry->vertices[static_cast<size_t>(triangle[(e + 1) % 3])]);
                    }
                }
            }
            view.renderer->setDebugMeshes(MeshLayer, std::move(meshes));
            view.drawn = geometry;
            view.drawnEdges = view.edges;
            view.drawnThrough = view.throughWalls;
            view.drawnOpacity = view.opacity;
        }

        // The movers where they are now, in their materials' colours with white edges.
        void drawMovers(ViewState& view, const std::shared_ptr<const audio::acoustics::Geometry>& geometry)
        {
            const std::vector<audio::acoustics::MoverPose>& poses = audio::acousticMoverPoses();
            if (view.moversDrawn && poses == view.drawnPoses) {
                return;
            }
            std::vector<SceneRenderer::DebugMesh> meshes(audio::acoustics::defaultMaterials().size() + 1);
            for (size_t i = 0; i + 1 < meshes.size(); i++) {
                meshes[i].r = colour(i)[0];
                meshes[i].g = colour(i)[1];
                meshes[i].b = colour(i)[2];
                meshes[i].a = view.opacity;
                meshes[i].throughWalls = view.throughWalls;
            }
            SceneRenderer::DebugMesh& edges = meshes.back();
            edges.throughWalls = view.throughWalls;
            for (const audio::acoustics::MoverShape& mover : geometry->movers) {
                const audio::acoustics::MoverPose* pose = nullptr;
                for (const audio::acoustics::MoverPose& candidate : poses) {
                    if (candidate.id == mover.id) {
                        pose = &candidate;
                    }
                }
                if (!pose) {
                    continue;
                }
                const audio::acoustics::Geometry& shape = *mover.shape;
                for (size_t t = 0; t < shape.triangles.size(); t++) {
                    size_t material = static_cast<size_t>(shape.triangleMaterial[t]);
                    for (size_t e = 0; e < 3; e++) {
                        Vector corner = audio::acoustics::placePoint(*pose, shape.vertices[static_cast<size_t>(shape.triangles[t][e])]);
                        Vector next = audio::acoustics::placePoint(*pose, shape.vertices[static_cast<size_t>(shape.triangles[t][(e + 1) % 3])]);
                        if (material + 1 < meshes.size()) {
                            meshes[material].triangles.push_back(corner);
                        }
                        edges.lines.push_back(corner);
                        edges.lines.push_back(next);
                    }
                }
            }
            view.renderer->setDebugMeshes(MoverLayer, std::move(meshes));
            view.drawnPoses = poses;
            view.moversDrawn = true;
        }

        // What the crosshair is on: the surface the view's centre ray meets first, shaded in the
        // level in its material's colour, with how its material was decided.
        void crosshair(ViewState& view, const Frame& frame, const std::shared_ptr<const audio::acoustics::Geometry>& geometry)
        {
            Coords axes = rotationAxes(frame.viewRotation);
            float distance = 0;
            int32_t triangle = audio::acoustics::pickTriangle(*geometry, frame.viewLocation, axes.xAxis, 65536.0f, distance);
            if (triangle < 0) {
                ImGui::TextDisabled("Crosshair: no surface of the scene ahead");
                view.clearPick();
                return;
            }
            int32_t surface = geometry->triangleSurface[static_cast<size_t>(triangle)];
            const audio::acoustics::SurfaceInfo& info = geometry->surfaces[static_cast<size_t>(surface)];
            const audio::acoustics::Resolution& resolution = geometry->resolutions[static_cast<size_t>(surface)];
            const std::string& material = audio::acousticMaterials()[static_cast<size_t>(resolution.material)].name;
            ImGui::Text("Crosshair, %.0f units (%.1f m) away: %s.%s, flags %08x", static_cast<double>(distance), static_cast<double>(distance / audio::acoustics::UnitsPerMetre), info.group.c_str(), info.texture.c_str(), info.flags);
            // The material's swatch and name in the colour the scene view draws it in.
            const std::array<float, 3>& shade = colour(static_cast<size_t>(resolution.material));
            ImVec4 swatch(shade[0], shade[1], shade[2], 1.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted("->");
            ImGui::SameLine();
            float side = ImGui::GetTextLineHeight();
            ImGui::ColorButton("##crosshair", swatch, ImGuiColorEditFlags_NoTooltip, ImVec2(side, side));
            ImGui::SameLine();
            ImGui::TextColored(swatch, "%s (%s)", material.c_str(), resolution.rule);
            bool withMesh = view.showMesh;
            if (!view.renderer || (surface == view.pickedSurface && geometry == view.pickedGeometry && withMesh == view.pickedWithMesh && view.opacity == view.pickedOpacity)) {
                return;
            }
            // The surface in its material's colour. Over the scene view, where the mesh around it
            // has the same colour, it is filled more opaquely than the view and edged in white.
            SceneRenderer::DebugMesh fill;
            fill.r = shade[0];
            fill.g = shade[1];
            fill.b = shade[2];
            fill.a = withMesh ? std::min(1.0f, view.opacity + 0.4f) : 0.5f;
            fill.throughWalls = true;
            SceneRenderer::DebugMesh edges;
            edges.throughWalls = true;
            SceneRenderer::DebugMesh& edged = withMesh ? edges : fill;
            for (size_t t = 0; t < geometry->triangles.size(); t++) {
                if (geometry->triangleSurface[t] != surface) {
                    continue;
                }
                const std::array<int32_t, 3>& corners = geometry->triangles[t];
                for (size_t e = 0; e < 3; e++) {
                    fill.triangles.push_back(geometry->vertices[static_cast<size_t>(corners[e])]);
                    edged.lines.push_back(geometry->vertices[static_cast<size_t>(corners[e])]);
                    edged.lines.push_back(geometry->vertices[static_cast<size_t>(corners[(e + 1) % 3])]);
                }
            }
            std::vector<SceneRenderer::DebugMesh> meshes;
            meshes.push_back(std::move(fill));
            if (withMesh) {
                meshes.push_back(std::move(edges));
            }
            view.renderer->setDebugMeshes(PickLayer, std::move(meshes));
            view.pickedSurface = surface;
            view.pickedGeometry = geometry;
            view.pickedWithMesh = withMesh;
            view.pickedOpacity = view.opacity;
        }

        // The material table, editable: what is changed reaches the scene at once and lasts
        // until the game quits. "Copy as code" gives lines to paste over the table in
        // src/audio/acoustic_scene.cc.
        void materialTable(ViewState& view, const std::shared_ptr<const audio::acoustics::Geometry>& geometry)
        {
            std::vector<audio::acoustics::Material> table = audio::acousticMaterials();
            std::vector<size_t> perMaterial(table.size(), 0);
            if (geometry) {
                for (int32_t material : geometry->triangleMaterial) {
                    if (material >= 0 && static_cast<size_t>(material) < perMaterial.size()) {
                        perMaterial[static_cast<size_t>(material)]++;
                    }
                }
            }
            bool changed = false;
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("materials", 6, flags)) {
                ImGui::TableSetupColumn("");
                ImGui::TableSetupColumn("Material");
                ImGui::TableSetupColumn("Triangles");
                ImGui::TableSetupColumn("Absorption 400 Hz / 2.5 kHz / 15 kHz");
                ImGui::TableSetupColumn("Scattering");
                ImGui::TableSetupColumn("Transmission 400 Hz / 2.5 kHz / 15 kHz");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < table.size(); i++) {
                    audio::acoustics::Material& material = table[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::ColorButton("##swatch", ImVec4(colour(i)[0], colour(i)[1], colour(i)[2], 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(scaled(14), scaled(14)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(material.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", perMaterial[i]);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(scaled(210));
                    changed |= ImGui::DragFloat3("##absorption", material.absorption.data(), 0.005f, 0.0f, 1.0f, "%.3f");
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(scaled(70));
                    changed |= ImGui::DragFloat("##scattering", &material.scattering, 0.005f, 0.0f, 1.0f, "%.3f");
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(scaled(210));
                    changed |= ImGui::DragFloat3("##transmission", material.transmission.data(), 0.001f, 0.0f, 1.0f, "%.3f");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (changed) {
                audio::setAcousticMaterials(table);
            }
            if (ImGui::Button("Reset to the code's values")) {
                audio::setAcousticMaterials(audio::acoustics::defaultMaterials());
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy as code")) {
                view.tableText = audio::acoustics::materialTableText(audio::acousticMaterials());
                ImGui::SetClipboardText(view.tableText.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("drag to change, ctrl+click to type; lasts until the game quits");
            if (!view.tableText.empty()) {
                ImGui::InputTextMultiline("##table", view.tableText.data(), view.tableText.size() + 1, ImVec2(-1, 120), ImGuiInputTextFlags_ReadOnly);
            }
        }

        // The simulated reverb and the worker: the level over the reverb send, the authored zone
        // reverb instead to compare, the reflection simulation's rays and bounces, and what the
        // worker last measured.
        void reverb(const audio::AcousticStatus& status)
        {
            audio::ReverbControls controls = audio::reverbControls();
            bool changed = false;
            ImGui::SetNextItemWidth(scaled(200));
            changed |= ImGui::SliderFloat("Reverb level", &controls.level, 0.0f, 3.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("Save as AmbrosiaReverbLevel")) {
                settings::set(settings::reverbLevel, controls.level);
            }
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Authored zone reverb instead", &controls.authored);
            ImGui::SetNextItemWidth(scaled(200));
            changed |= ImGui::SliderInt("Rays", &controls.reflections.rays, 256, audio::acoustics::MaxRays);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(scaled(200));
            changed |= ImGui::SliderInt("Bounces", &controls.reflections.bounces, 1, 64);
            if (changed) {
                audio::setReverbControls(controls);
            }
            const audio::acoustics::Stats& stats = status.simulation;
            ImGui::Text("Decay (RT60) around the listener: %.2f s at 400 Hz, %.2f s at 2.5 kHz, %.2f s at 15 kHz%s", static_cast<double>(stats.reverbTimes[0]), static_cast<double>(stats.reverbTimes[1]), static_cast<double>(stats.reverbTimes[2]), controls.authored ? " (not heard: authored reverb chosen)" : "");
            ImGui::Text("Worker: %.1f passes a second; direct %.2f ms for %zu sources; reflections %.1f ms (%d rays, %d bounces) about ten times a second", stats.passesPerSecond, stats.directMilliseconds, stats.sources, stats.reflectionMilliseconds, stats.rays, stats.bounces);
            audio::ReverbSettings zone = audio::reverbSettings();
            if (zone.volume > 0) {
                ImGui::TextDisabled("This zone has authored reverb (volume %.2f), which the simulation replaces unless chosen above.", static_cast<double>(zone.volume));
            } else {
                ImGui::TextDisabled("No authored reverb in this zone; the original plays it dry.");
            }
        }

        std::string describeAge(double seconds)
        {
            char text[64];
            if (seconds < 0) {
                return "unknown";
            }
            if (seconds < 120) {
                std::snprintf(text, sizeof(text), "%.0f s ago", seconds);
            } else if (seconds < 7200) {
                std::snprintf(text, sizeof(text), "%.0f minutes ago", seconds / 60);
            } else if (seconds < 172800) {
                std::snprintf(text, sizeof(text), "%.0f hours ago", seconds / 3600);
            } else {
                std::snprintf(text, sizeof(text), "%.0f days ago", seconds / 86400);
            }
            return text;
        }

        // Each probe as a small cross where it stands, cyan.
        void drawProbes(BakedView& baked, const std::shared_ptr<const audio::acoustics::BakedData>& data)
        {
            if (!data) {
                baked.clearProbes();
                return;
            }
            if (data == baked.drawnProbes && baked.throughWalls == baked.drawnThrough) {
                return;
            }
            SceneRenderer::DebugMesh crosses;
            crosses.r = 0.2f;
            crosses.g = 1.0f;
            crosses.b = 1.0f;
            crosses.a = 0.9f;
            crosses.throughWalls = baked.throughWalls;
            constexpr float Arm = 6.0f;
            for (const audio::acoustics::Probe& probe : data->probes) {
                const Vector& p = probe.location;
                for (const Vector& arm : { Vector { Arm, 0, 0 }, Vector { 0, Arm, 0 }, Vector { 0, 0, Arm } }) {
                    crosses.lines.push_back(p - arm);
                    crosses.lines.push_back(p + arm);
                }
            }
            std::vector<SceneRenderer::DebugMesh> meshes;
            meshes.push_back(std::move(crosses));
            baked.renderer->setDebugMeshes(ProbeLayer, std::move(meshes));
            baked.drawnProbes = data;
            baked.drawnThrough = baked.throughWalls;
        }

        // The traced voice's path: the legs between probes Steam Audio checked (green when
        // clear, red where something in the scene now breaks one), and a yellow line from the
        // listener the way the pathed sound arrives.
        void drawPath(BakedView& baked, const Frame& frame, const audio::VoiceStatus* voice)
        {
            if (!voice || !baked.renderer) {
                baked.clearPath();
                return;
            }
            SceneRenderer::DebugMesh clear;
            clear.r = 0.2f;
            clear.g = 1.0f;
            clear.b = 0.3f;
            clear.throughWalls = true;
            SceneRenderer::DebugMesh broken = clear;
            broken.r = 1.0f;
            broken.g = 0.2f;
            broken.b = 0.2f;
            SceneRenderer::DebugMesh way = clear;
            way.r = 1.0f;
            way.g = 0.9f;
            way.b = 0.2f;
            for (const audio::acoustics::PathSegment& leg : audio::tracedPath()) {
                SceneRenderer::DebugMesh& mesh = leg.occluded ? broken : clear;
                mesh.lines.push_back(leg.from);
                mesh.lines.push_back(leg.to);
            }
            if (voice->pathFound) {
                Vector start = frame.viewLocation + rotationAxes(frame.viewRotation).xAxis * 24.0f - Vector { 0, 0, 8 };
                way.lines.push_back(start);
                way.lines.push_back(start + voice->pathDirection * 160.0f);
            }
            std::vector<SceneRenderer::DebugMesh> meshes;
            meshes.push_back(std::move(clear));
            meshes.push_back(std::move(broken));
            meshes.push_back(std::move(way));
            baked.renderer->setDebugMeshes(PathLayer, std::move(meshes));
            baked.pathDrawn = true;
        }

        float decibels(float amplitude)
        {
            return amplitude > 1e-5f ? 20.0f * std::log10(amplitude) : -100.0f;
        }

        // The baked part of the raytraced mode: where the level's bake stands and its file, a
        // button to bake it again, which baked data is used, the probes over the level, and the
        // path a chosen voice is heard by.
        void bakedAcoustics(BakedView& baked, const Frame& frame, const audio::AcousticStatus& status)
        {
            baked.renderer = frame.renderer;
            audio::acoustics::BakeReport report = audio::bakeReport();
            const audio::acoustics::Stats& stats = status.simulation;
            ImGui::Text("Baked acoustics for %s: %s", report.map.empty() ? "(no map)" : report.map.c_str(), audio::acoustics::bakeStateName(report.state));
            if (report.state == audio::acoustics::BakeState::Baking) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s %.0f%%", audio::acoustics::bakeStageName(report.stage), static_cast<double>(report.progress * 100));
                ImGui::SameLine();
                ImGui::ProgressBar(report.progress, ImVec2(scaled(220), 0), label);
            }
            if (!report.reason.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Cache %s: %s", audio::acoustics::cacheStateName(report.cache), report.reason.c_str());
            }
            if (report.fileBytes > 0) {
                ImGui::Text("Cache file %s: %.2f MB, written %s", report.path.filename().string().c_str(), static_cast<double>(report.fileBytes) / 1048576.0, describeAge(report.fileAgeSeconds).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", report.path.string().c_str());
                }
            } else if (!report.path.empty()) {
                ImGui::TextDisabled("No cache file at %s", report.path.string().c_str());
            }
            if (report.data) {
                ImGui::Text("%zu probes; baked in %.1f s (reverb %.1f s, paths %.1f s); reverb %.1f KB, paths %.1f KB", report.data->probes.size(), (report.data->probeMilliseconds + report.data->reverbMilliseconds + report.data->pathMilliseconds) / 1000.0, report.data->reverbMilliseconds / 1000.0, report.data->pathMilliseconds / 1000.0, static_cast<double>(report.data->reverbBytes) / 1024.0, static_cast<double>(report.data->pathBytes) / 1024.0);
            }
            bool canBake = report.state != audio::acoustics::BakeState::Unavailable && report.state != audio::acoustics::BakeState::Idle;
            ImGui::BeginDisabled(!canBake || report.state == audio::acoustics::BakeState::Baking);
            if (ImGui::Button(report.data ? "Rebake" : "Bake")) {
                audio::rebakeAcoustics();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (!stats.bakeError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Not used: %s", stats.bakeError.c_str());
            } else {
                ImGui::TextDisabled(stats.baked ? "the simulation is using %zu probes" : "the simulation is running live", stats.probes);
            }

            audio::acoustics::BakedUse use = audio::bakedUse();
            bool changed = ImGui::Checkbox("Baked reverb (else live)", &use.reverb);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Pathing", &use.pathing);
            if (changed) {
                audio::setBakedUse(use);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("for this run; AmbrosiaBakedReverb, AmbrosiaPathing and AmbrosiaBakeAcoustics in the ini");
            ImGui::Text("Reverb now from %s. Pathing: %.2f ms a pass, a path for %zu of %zu sources.", stats.reverbBaked ? "the baked probes" : "the live simulation", stats.pathMilliseconds, stats.pathed, stats.sources);

            ImGui::Checkbox("Show probes", &baked.showProbes);
            ImGui::SameLine();
            ImGui::Checkbox("Probes through walls", &baked.throughWalls);
            if (baked.showProbes && baked.renderer) {
                drawProbes(baked, report.data);
            } else {
                baked.clearProbes();
            }

            // The voice whose path is drawn.
            std::vector<audio::VoiceStatus> voices = audio::voices();
            const audio::VoiceStatus* chosen = nullptr;
            if (baked.followLoudest) {
                float loudest = 0;
                for (const audio::VoiceStatus& voice : voices) {
                    float pathed = std::max({ voice.pathed[0], voice.pathed[1], voice.pathed[2] }) * static_cast<float>(voice.volume);
                    if (voice.pathFound && pathed > loudest) {
                        loudest = pathed;
                        chosen = &voice;
                    }
                }
            } else {
                for (const audio::VoiceStatus& voice : voices) {
                    if (voice.id == baked.traced) {
                        chosen = &voice;
                    }
                }
            }
            int32_t wanted = chosen ? chosen->id : 0;
            if (wanted != baked.traced) {
                baked.traced = wanted;
                audio::setTracedVoice(wanted);
            }
            char preview[128] = "none";
            if (chosen) {
                std::snprintf(preview, sizeof(preview), "%d %s", chosen->id, chosen->sound ? chosen->sound->name.str().c_str() : "");
            }
            ImGui::SetNextItemWidth(scaled(260));
            if (ImGui::BeginCombo("Path of voice", preview)) {
                if (ImGui::Selectable("none", !chosen)) {
                    baked.followLoudest = false;
                    baked.traced = 0;
                    audio::setTracedVoice(0);
                }
                for (const audio::VoiceStatus& voice : voices) {
                    if (!voice.simulated) {
                        continue;
                    }
                    char label[128];
                    std::snprintf(label, sizeof(label), "%d %s%s", voice.id, voice.sound ? voice.sound->name.str().c_str() : "", voice.pathFound ? "" : " (no path)");
                    if (ImGui::Selectable(label, chosen && voice.id == chosen->id)) {
                        baked.followLoudest = false;
                        baked.traced = voice.id;
                        audio::setTracedVoice(voice.id);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Follow the loudest pathed voice", &baked.followLoudest);
            if (chosen) {
                if (chosen->pathFound) {
                    ImGui::Text("Path %.0f units (%.1f m), straight line %.0f; arrives from %.2f %.2f %.2f; adds %.1f / %.1f / %.1f dB at 400 Hz / 2.5 kHz / 15 kHz; direct %.2f / %.2f / %.2f", static_cast<double>(chosen->pathLength), static_cast<double>(chosen->pathLength / audio::acoustics::UnitsPerMetre), static_cast<double>(chosen->distance), static_cast<double>(chosen->pathDirection.x), static_cast<double>(chosen->pathDirection.y), static_cast<double>(chosen->pathDirection.z), static_cast<double>(decibels(chosen->pathed[0])), static_cast<double>(decibels(chosen->pathed[1])), static_cast<double>(decibels(chosen->pathed[2])), static_cast<double>(chosen->occlusion + (1 - chosen->occlusion) * chosen->transmission[0]), static_cast<double>(chosen->occlusion + (1 - chosen->occlusion) * chosen->transmission[1]), static_cast<double>(chosen->occlusion + (1 - chosen->occlusion) * chosen->transmission[2]));
                } else {
                    ImGui::TextDisabled("No path reaches the listener from this voice.");
                }
            }
            drawPath(baked, frame, chosen);
        }

        void section(ViewState& view, const Frame& frame)
        {
            view.renderer = frame.renderer;
            audio::AcousticStatus status = audio::acousticStatus();
            if (!status.built) {
                view.clear();
                ImGui::TextDisabled("Not built: this build has no Steam Audio (AMBROSIA_STEAM_AUDIO is off)");
                return;
            }
            if (!status.active) {
                view.clear();
                ImGui::TextDisabled(status.unavailable ? "Raytraced mode unavailable (Steam Audio's simulation could not be set up)" : "Raytraced mode is off; choose Raytraced in the audio section");
                materialTable(view, nullptr);
                return;
            }
            std::shared_ptr<const audio::acoustics::Geometry> geometry = audio::acousticGeometry();
            const audio::acoustics::Stats& stats = status.simulation;
            ImGui::Text("Ray tracer: %s. Scene: %zu triangles with %zu movers, built in %.1f ms on the worker; exported in %.1f ms.", stats.embree ? "Embree" : "Steam Audio's built-in", stats.triangles, stats.movers, stats.buildMilliseconds, status.exportMilliseconds);
            if (!geometry) {
                ImGui::TextDisabled("No level yet.");
                view.clear();
                return;
            }
            const auto& counts = geometry->triangleCounts;
            ImGui::Text("Model surfaces %zu, drawn %zu. Triangles kept %zu; left out:", geometry->modelSurfaces, geometry->surfaces.size(), counts[0]);
            for (size_t reason = 1; reason < counts.size(); reason++) {
                ImGui::SameLine();
                ImGui::Text("%s %zu%s", audio::acoustics::exclusionName(static_cast<audio::acoustics::Exclusion>(reason)), counts[reason], reason + 1 < counts.size() ? "," : "");
            }
            ImGui::Checkbox("Show the scene over the level", &view.showMesh);
            ImGui::SameLine();
            ImGui::Checkbox("Edges", &view.edges);
            ImGui::SameLine();
            ImGui::Checkbox("Through walls", &view.throughWalls);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(scaled(120));
            ImGui::SliderFloat("Opacity", &view.opacity, 0.05f, 1.0f, "%.2f");
            ImGui::SameLine();
            ImGui::Checkbox("Crosshair", &view.pick);
            if (view.showMesh && view.renderer) {
                drawMesh(view, geometry);
                drawMovers(view, geometry);
            } else {
                view.clearMesh();
            }
            if (view.pick) {
                crosshair(view, frame, geometry);
            } else {
                view.clearPick();
            }
            ImGui::Separator();
            reverb(status);
            ImGui::Separator();
            bakedAcoustics(view.baked, frame, status);
            ImGui::Separator();
            materialTable(view, geometry);
        }
    }

    void addAcousticsSection(Overlay& overlay)
    {
        auto view = std::make_shared<ViewState>();
        Tool tool;
        tool.menu = "Audio";
        tool.title = "Acoustic scene";
        tool.name = "acoustics";
        tool.width = 1000;
        tool.height = 640;
        // Open at first, as its section was, so "DebugOverlay mesh on" shows the scene.
        tool.openAtFirst = true;
        tool.draw = [view](const Frame& frame) {
            section(*view, frame);
        };
        // Closed, the window takes away the scene and probes it drew, as hiding the overlay does.
        tool.closed = [view] {
            view->clear();
        };
        overlay.addTool(std::move(tool));
        overlay.onClose([view] {
            view->clear();
        });
        overlay.addSwitch("mesh", [view](bool on) {
            view->showMesh = on;
        });
        overlay.addSwitch("mesh-through-walls", [view](bool on) {
            view->throughWalls = on;
        });
        overlay.addSwitch("probes", [view](bool on) {
            view->baked.showProbes = on;
        });
        overlay.addSwitch("path", [view](bool on) {
            view->baked.followLoudest = on;
        });
    }
}
