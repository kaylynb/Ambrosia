#include "debugui/audio_section.h"
#include "debugui/imgui_setup.h"

#include "audio/audio.h"
#include "core/config.h"
#include "core/objects.h"
#include "core/settings.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "core/text.h"
#include "debugui/world_draw.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dx::debugui {
    namespace {
        constexpr double Degrees = 180.0 / 3.14159265358979323846;

        // Saved, and applied at once by the audio library's change notice.
        void setIniSwitch(const settings::Bool& setting, bool on)
        {
            settings::set(setting, on);
        }

        const char* slotName(int32_t slot)
        {
            static const char* const names[] = { "None", "Misc", "Pain", "Interact", "Ambient", "Talk", "Interface" };
            return slot >= 0 && slot < 7 ? names[slot] : "?";
        }

        void switches()
        {
            bool raytraced = settings::get(settings::raytracedAudio);
            bool hrtf = settings::get(settings::hrtf);
            int mode = raytraced ? 2 : (hrtf ? 1 : 0);
            ImGui::TextUnformatted("Mode:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Galaxy stereo", mode == 0) && mode != 0) {
                setIniSwitch(settings::raytracedAudio, false);
                setIniSwitch(settings::hrtf, false);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("HRTF", mode == 1) && mode != 1) {
                setIniSwitch(settings::raytracedAudio, false);
                setIniSwitch(settings::hrtf, true);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Raytraced", mode == 2) && mode != 2) {
                setIniSwitch(settings::raytracedAudio, true);
            }
            ImGui::SameLine();
            const ImVec4 good(0.5f, 1.0f, 0.5f, 1.0f);
            const ImVec4 warn(1.0f, 0.6f, 0.3f, 1.0f);
            if (mode == 0) {
                ImGui::TextDisabled("Galaxy's stereo pan");
            } else if (!audio::hrtfActive()) {
                ImGui::TextColored(warn, "unavailable, stereo kept");
            } else if (mode == 1) {
                ImGui::TextColored(good, "Steam Audio binaural");
            } else if (audio::raytracedActive()) {
                ImGui::TextColored(good, "Steam Audio binaural and simulation");
            } else {
                ImGui::TextColored(warn, "simulation unavailable, HRTF kept");
            }
            helpMarker("AmbrosiaHRTF and AmbrosiaRaytracedAudio in the audio section. These and Reverb are saved to the ini, as from the console.");
            bool reverb = settings::get(settings::useReverb);
            if (ImGui::Checkbox("Reverb (UseReverb)", &reverb)) {
                setIniSwitch(settings::useReverb, reverb);
            }
        }

        void zoneReverb()
        {
            audio::ReverbSettings settings = audio::reverbSettings();
            if (settings.volume == 0) {
                ImGui::TextDisabled("Zone reverb: none here (or reverb off)");
                return;
            }
            ImGui::Text("Zone reverb: volume %.3f, cutoff %.0f Hz", static_cast<double>(settings.volume), static_cast<double>(settings.cutoff));
            if (ImGui::BeginTable("reverb", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("delay s");
                for (float delay : settings.delay) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", static_cast<double>(delay));
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("gain");
                for (float gain : settings.gain) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", static_cast<double>(gain));
                }
                ImGui::EndTable();
            }
        }

        void mixCost()
        {
            audio::MixCost cost = audio::mixCost();
            auto show = [](const char* name, float milliseconds) {
                if (milliseconds < 0) {
                    ImGui::Text("Mixing, %s: not measured yet (needs a second of sound in this mode)", name);
                } else {
                    ImGui::Text("Mixing, %s: %.2f ms per second of sound (%.2f%% of a core)", name, static_cast<double>(milliseconds), static_cast<double>(milliseconds) / 10.0);
                }
            };
            show("stereo", cost.stereo);
            show("HRTF", cost.hrtf);
        }

        void listener(const Frame& frame)
        {
            double viewYaw = static_cast<double>(frame.viewRotation.yaw & 65535) * 360.0 / 65536.0;
            ImGui::Text("Listener (view): %.0f %.0f %.0f, yaw %d (%.0f deg), pitch %d", static_cast<double>(frame.viewLocation.x), static_cast<double>(frame.viewLocation.y), static_cast<double>(frame.viewLocation.z), frame.viewRotation.yaw & 65535, viewYaw, frame.viewRotation.pitch);
            if (!frame.player || frame.player->destroyed || !frame.player->data) {
                return;
            }
            Vector location = actor::Location(frame.player);
            int32_t yaw = actor::Rotation(frame.player).yaw & 65535;
            char command[128];
            std::snprintf(command, sizeof(command), "teleport %.0f %.0f %.0f %d", static_cast<double>(location.x), static_cast<double>(location.y), static_cast<double>(location.z), yaw);
            ImGui::Text("Player: %s", command);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(command);
            }
        }

        // What the section draws over the level: the ray to one voice and a marker on each
        // positioned voice. Drawn with ImGui over the finished frame, so through walls by nature.
        struct RayState {
            // The voice the ray goes to, or 0.
            int32_t selected = 0;
            bool markers = false;
            // Off, a marker shows only while the level's geometry does not hide it from the view.
            bool markersThroughWalls = true;
        };

        // How hidden a voice is, 0 clear to 1 occluded, or negative where this mode does not know:
        // the raytraced mode's occlusion once simulated, otherwise Galaxy's line check, which
        // brings obstruction up to 0.5 over half a second and leaves out speech.
        float hiddenness(const audio::VoiceStatus& voice, bool raytraced)
        {
            if (voice.simulated) {
                return std::clamp(1.0f - voice.occlusion, 0.0f, 1.0f);
            }
            if (raytraced || voice.slot == 5) {
                return -1.0f;
            }
            return std::clamp(voice.obstruction / 0.5f, 0.0f, 1.0f);
        }

        // Green when clear through red when occluded; grey when not known.
        ImU32 hiddenColour(float hidden, float alpha)
        {
            if (hidden < 0) {
                return ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f, 0.75f, 0.75f, alpha));
            }
            return ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f + 0.8f * hidden, 1.0f - 0.8f * hidden, 0.25f, alpha));
        }

        std::string soundName(const audio::VoiceStatus& voice)
        {
            return voice.sound ? voice.sound->name.str() : std::string("None");
        }

        // A thick line from just below the middle of the view to the voice, through walls and
        // with a dark edge, coloured by how hidden the voice is. It starts a little ahead of the
        // eye: a line from the eye itself would show end on, as a single point. Behind the view,
        // the line runs to where it leaves the screen.
        void drawRay(const Frame& frame, const audio::VoiceStatus& voice, bool raytraced)
        {
            Coords axes = rotationAxes(frame.viewRotation);
            Vector start = frame.viewLocation + axes.xAxis * 30.0f - axes.zAxis * 12.0f;
            ImU32 colour = hiddenColour(hiddenness(voice, raytraced), 1.0f);
            if (!draw::line(frame, start, voice.location, colour, 4.0f)) {
                return;
            }
            float x = 0;
            float y = 0;
            if (draw::pixelsPerUnit(frame, voice.location) > 0 && toScreen(frame, voice.location, x, y)) {
                ImDrawList* list = ImGui::GetBackgroundDrawList();
                list->AddCircleFilled(ImVec2(x, y), 7.0f, IM_COL32(0, 0, 0, 200));
                list->AddCircleFilled(ImVec2(x, y), 5.0f, colour);
                char label[160];
                std::snprintf(label, sizeof(label), "ray: %s (%d), %.0f units", soundName(voice).c_str(), voice.id, static_cast<double>(voice.distance));
                draw::shadowedText(list, ImVec2(x + 10, y + 6), IM_COL32(255, 255, 255, 255), label);
            }
        }

        // A marker where the voice is: a diamond for an ambient loop, a circle for a one-shot,
        // green clear through red occluded (grey where not known), hollow when muted, ringed in
        // white when soloed or chosen for the ray; a fixed size in the world, kept between a few
        // pixels far off and a small disc up close; labelled with the sound's name.
        void drawMarker(const Frame& frame, const audio::VoiceStatus& voice, bool raytraced, bool highlighted, bool throughWalls)
        {
            float x = 0;
            float y = 0;
            if (!toScreen(frame, voice.location, x, y)) {
                return;
            }
            if (!throughWalls && !draw::seen(frame, voice.location)) {
                return;
            }
            float radius = std::clamp(16.0f * draw::pixelsPerUnit(frame, voice.location), 5.0f, 13.0f);
            ImDrawList* list = ImGui::GetBackgroundDrawList();
            ImU32 colour = hiddenColour(hiddenness(voice, raytraced), 0.9f);
            ImU32 edge = IM_COL32(0, 0, 0, 220);
            ImVec2 centre(x, y);
            if (highlighted) {
                if (voice.ambient) {
                    float r = radius + 5;
                    list->AddQuad(ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y), IM_COL32(255, 255, 255, 255), 3.0f);
                } else {
                    list->AddCircle(centre, radius + 5, IM_COL32(255, 255, 255, 255), 0, 3.0f);
                }
            }
            if (voice.ambient) {
                ImVec2 top(x, y - radius);
                ImVec2 right(x + radius, y);
                ImVec2 bottom(x, y + radius);
                ImVec2 left(x - radius, y);
                if (!voice.muted) {
                    list->AddQuadFilled(top, right, bottom, left, colour);
                }
                list->AddQuad(top, right, bottom, left, voice.muted ? colour : edge, 2.0f);
            } else {
                if (!voice.muted) {
                    list->AddCircleFilled(centre, radius, colour);
                }
                list->AddCircle(centre, radius, voice.muted ? colour : edge, 0, 2.0f);
            }
            float textHeight = ImGui::GetTextLineHeight();
            draw::shadowedText(list, ImVec2(x + radius + 4, y - textHeight * 0.5f), highlighted ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 230, 230), soundName(voice));
        }

        // Each frame the overlay shows; nothing is asked of the audio while both are off.
        void drawInLevel(RayState& ray, const Frame& frame)
        {
            if (!ray.markers && ray.selected == 0) {
                return;
            }
            std::vector<audio::VoiceStatus> voices = audio::voices();
            bool raytraced = audio::raytracedActive();
            const audio::VoiceStatus* rayVoice = nullptr;
            for (const audio::VoiceStatus& voice : voices) {
                if (voice.id == ray.selected && voice.attenuated) {
                    rayVoice = &voice;
                }
            }
            if (!rayVoice) {
                // The sound ended (or never was placed).
                ray.selected = 0;
            }
            if (ray.markers) {
                for (const audio::VoiceStatus& voice : voices) {
                    if (voice.attenuated) {
                        drawMarker(frame, voice, raytraced, voice.soloed || voice.id == ray.selected, ray.markersThroughWalls);
                    }
                }
            }
            if (rayVoice) {
                drawRay(frame, *rayVoice, raytraced);
            }
        }

        void voiceTable(RayState& ray)
        {
            // In the order the voices started, so a row stays put while others come and go; the
            // mixer's own order follows its channel slots, which new sounds take over.
            std::vector<audio::VoiceStatus> voices = audio::voices();
            std::stable_sort(voices.begin(), voices.end(), [](const audio::VoiceStatus& a, const audio::VoiceStatus& b) {
                return a.id < b.id;
            });
            ImGui::Checkbox("Source markers", &ray.markers);
            ImGui::SameLine();
            ImGui::Checkbox("Markers through walls", &ray.markersThroughWalls);
            helpMarker("Diamond: ambient loop. Circle: one-shot. Green clear to red occluded, grey unknown. Hollow: muted. White ring: soloed or the ray's voice.");
            ImGui::Text("%zu voices playing", voices.size());
            helpMarker("Direction: azimuth right of ahead and elevation in degrees, or Galaxy's pan from -1 left to +1 right.\n"
                       "Blend: 0 at the listener, 1 from a tenth of the radius out.\n"
                       "Occlusion (raytraced mode): the fraction of the source not hidden, 1 clear. Transmission: what passes through the hidden part at 400 Hz, 2.5 kHz and 15 kHz.\n"
                       "Ray: a thick line through walls to the chosen positioned voice, green clear to red occluded (grey where nothing measures it). DebugOverlay ray <sound name or id> chooses it, ray none clears it.\n"
                       "Id / sample: a restarted sound shows a new id with its sample back at the start.\n"
                       "Solo and mute last only while the sound plays and are not saved.");
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
            if (!ImGui::BeginTable("voices", 15, flags, ImVec2(0, scaled(320)))) {
                return;
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* name : { "Solo", "Mute", "Ray", "Sound", "Actor", "Id / sample", "Slot", "Level", "Direction", "Distance", "Obstruct", "Occlusion", "Transmission", "Blend", "Ambient" }) {
                ImGui::TableSetupColumn(name);
            }
            ImGui::TableHeadersRow();
            for (const audio::VoiceStatus& voice : voices) {
                ImGui::PushID(voice.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // These act on the press rather than ImGui's release, as a short sound can end, and
                // its row go, between the two.
                bool soloed = voice.soloed;
                ImGui::Checkbox("##solo", &soloed);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    audio::setVoiceSoloed(voice.id, !voice.soloed);
                }
                ImGui::TableNextColumn();
                bool muted = voice.muted;
                ImGui::Checkbox("##mute", &muted);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    audio::setVoiceMuted(voice.id, !voice.muted);
                }
                ImGui::TableNextColumn();
                if (voice.attenuated) {
                    ImGui::RadioButton("##ray", ray.selected == voice.id);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        ray.selected = ray.selected == voice.id ? 0 : voice.id;
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(voice.sound ? voice.sound->name.c_str() : "None");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(voice.actor ? voice.actor->name.c_str() : "None");
                ImGui::TableNextColumn();
                // A voice cut off and started again shows a new id and its sample back at the start.
                ImGui::Text("%d  %zu/%zu", voice.id, voice.position, voice.frames);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slotName(voice.slot));
                ImGui::TableNextColumn();
                ImGui::Text("%5.1f%%", static_cast<double>(voice.volume) * 100.0 / 32767.0);
                ImGui::TableNextColumn();
                if (!voice.attenuated) {
                    ImGui::TextDisabled("not placed");
                } else if (voice.hrtf) {
                    double forward = static_cast<double>(voice.offset.x);
                    double right = static_cast<double>(voice.offset.y);
                    double up = static_cast<double>(voice.offset.z);
                    double azimuth = std::atan2(right, forward) * Degrees;
                    double elevation = std::atan2(up, std::sqrt(forward * forward + right * right)) * Degrees;
                    ImGui::Text("HRTF az %+4.0f el %+3.0f", azimuth, elevation);
                } else {
                    // Galaxy's pan, 0 hard left to 32767 hard right.
                    ImGui::Text("pan %+5.2f", (static_cast<double>(voice.pan) - 16383.0) / 16384.0);
                }
                ImGui::TableNextColumn();
                if (voice.attenuated) {
                    ImGui::Text("%.0f", static_cast<double>(voice.distance));
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", static_cast<double>(voice.obstruction));
                ImGui::TableNextColumn();
                if (voice.simulated) {
                    ImGui::Text("%.2f", static_cast<double>(voice.occlusion));
                }
                ImGui::TableNextColumn();
                if (voice.simulated) {
                    ImGui::Text("%.2f %.2f %.2f", static_cast<double>(voice.transmission[0]), static_cast<double>(voice.transmission[1]), static_cast<double>(voice.transmission[2]));
                }
                ImGui::TableNextColumn();
                if (voice.attenuated) {
                    ImGui::Text("%.2f", static_cast<double>(voice.spread));
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(voice.ambient ? "yes" : "");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // "DebugOverlay ray <sound name or voice id>" picks the positioned voice to draw the ray
        // to, the loudest one if several play that sound; "none" or "off" clears it.
        std::string selectRay(RayState& ray, const std::string& value)
        {
            if (value == "none" || value == "off" || value == "0") {
                ray.selected = 0;
                return "ray: none";
            }
            const audio::VoiceStatus* best = nullptr;
            std::vector<audio::VoiceStatus> voices = audio::voices();
            for (const audio::VoiceStatus& voice : voices) {
                bool named = voice.sound && lowered(voice.sound->name.str()) == value;
                if (voice.attenuated && (named || std::to_string(voice.id) == value) && (!best || voice.volume > best->volume)) {
                    best = &voice;
                }
            }
            if (!best) {
                return "ray: no positioned voice playing with that sound name or id";
            }
            ray.selected = best->id;
            return "ray: voice " + std::to_string(best->id) + " " + (best->sound ? best->sound->name.str() : std::string());
        }
    }

    void addAudioSection(Overlay& overlay)
    {
        auto ray = std::make_shared<RayState>();
        overlay.addCommand("ray", [ray](const std::string& value) {
            return selectRay(*ray, value);
        });
        Tool tool;
        tool.menu = "Audio";
        tool.title = "Audio";
        tool.name = "audio";
        tool.width = 1000;
        tool.height = 640;
        tool.openAtFirst = true;
        tool.draw = [ray](const Frame& frame) {
            switches();
            mixCost();
            listener(frame);
            zoneReverb();
            ImGui::Separator();
            voiceTable(*ray);
        };
        overlay.addTool(std::move(tool));
        overlay.addDrawer([ray](const Frame& frame) {
            drawInLevel(*ray, frame);
        });
        overlay.addSwitch("markers", [ray](bool on) {
            ray->markers = on;
        });
        overlay.addSwitch("markers-through-walls", [ray](bool on) {
            ray->markersThroughWalls = on;
        });
    }
}
