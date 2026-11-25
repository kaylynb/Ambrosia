#pragma once

#include "audio/acoustic_baker.h"
#include "audio/acoustics.h"
#include "audio/steam_audio.h"
#include "core/math.h"
#include "core/profile.h"
#include "core/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The mixer's side of the Ambrosia raytraced sound mode: what audio.cc keeps for it, and the few
// calls it makes into it. Only audio.cc and raytraced_mode.cc include this; everything else
// uses audio/raytraced.h.
namespace dx {
    class ULevel;
    class World;

    namespace audio {
        struct VoiceStatus;

        // The raytraced mode's state for one voice: its latest direct path, clear until
        // simulated; its latest baked path round what is in the way; and the band amplitudes of
        // the pathed sound the mixer last added for it.
        struct RaytracedVoice {
            acoustics::DirectPath direct;
            bool simulated = false;
            acoustics::PathResult path;
            std::array<float, 3> pathed {};
        };

        // The pathed sound for the voice on one HRTF channel, through effects of its own: whether
        // it was heard last block, its levels for its tail, and the last way and amplitudes,
        // which a fading pathed sound keeps.
        struct RaytracedChannel {
            bool pathOn = false;
            bool pathMixed = false;
            bool pathTail = false;
            float pathDry = 0;
            float pathWet = 0;
            steam::Direction pathDirection;
            float pathBlend = 1;
            std::array<float, 3> pathAmplitude {};
        };

        // What the pathed sound takes from the voice the HRTF mode mixes: its distance and
        // radius, its way and spread, and its dry and reverb levels as fractions of full scale.
        struct PathedVoice {
            float distance = 0;
            float radius = 0;
            steam::Direction direction;
            float spread = 1;
            float dry = 0;
            float wet = 0;
        };

        // The raytraced mode's reverb level by default (settings::reverbLevel).
        constexpr float DefaultReverbLevel = static_cast<float>(settings::reverbLevel.setting.number);

        // The raytraced mode, held by the mixer's state. The game thread owns simulation and
        // hands it the level it is in; the mixer reads raytracing under the mixer's lock. Calls
        // marked "mixer" are made with the mixer locked.
        struct RaytracedMode {
            // Asked for by AmbrosiaRaytracedAudio, which also asks for the HRTF, and on while
            // simulation is made.
            bool raytracedWanted = false;
            bool raytracedUnavailable = false;
            std::unique_ptr<acoustics::Simulation> simulation;
            // The simulation while the mode is on, else null: the mixer's and update's test of it.
            acoustics::Simulation* raytracing = nullptr;
            const ULevel* acousticLevel = nullptr;
            // Set when acousticLevel's world announced it unloading (engine/level_events.h), so
            // the next update lets its scene go even if another level is later made at the same
            // address.
            bool acousticLevelGone = false;
            double exportMilliseconds = 0;
            std::vector<acoustics::MoverPose> moverPoses;
            // The direct paths the mixer last took from the simulation, and their version.
            std::unordered_map<int32_t, acoustics::DirectPath> directPaths;
            uint64_t directVersion = 0;
            // The reverb: its level (AmbrosiaReverbLevel), the decay times the mixer last took, and
            // the debug overlay's switch back to the zone's reverb, and its rays.
            float reverbLevel = DefaultReverbLevel;
            std::array<float, 3> reverbTimes {};
            bool authoredReverb = false;
            acoustics::ReflectionSettings reflectionSettings;
            // The material table as tuned from the debug overlay, for this run only.
            std::vector<acoustics::Material> materials = acoustics::defaultMaterials();
            // Baked acoustics: the level's data found or baked in the background (with
            // AmbrosiaBakeAcoustics), which of it is used (AmbrosiaBakedReverb, AmbrosiaPathing), the
            // paths the mixer last took, and the voice whose path the overlay draws.
            std::unique_ptr<acoustics::Baker> baker;
            bool backgroundBake = true;
            acoustics::BakedUse bakedUse;
            std::string acousticMap;
            std::unordered_map<int32_t, acoustics::PathResult> pathResults;
            uint64_t pathVersion = 0;
            int32_t tracedVoice = 0;
            // The listener's view axes for the block being mixed, and the pathed sound's buffers.
            Coords axes;
            bool directNew = false;
            bool pathsNew = false;
            std::vector<float> pathMono;
            std::vector<float> pathLeft;
            std::vector<float> pathRight;

            bool on() const
            {
                return raytracing != nullptr;
            }

            bool wanted() const
            {
                return raytracedWanted;
            }

            bool hasSimulation() const
            {
                return simulation != nullptr;
            }

            // Reads the mode's settings from the audio section of the ini.
            void readSettings();
            // Makes the simulation, or null when it cannot be used, reported and tried once as for
            // the HRTF.
            std::unique_ptr<acoustics::Simulation> makeSimulation(uint32_t rate, size_t channels);
            // Takes in a newly made simulation, paused until switched on. The level is handed to it
            // when it is first switched on. Mixer.
            void install(std::unique_ptr<acoustics::Simulation> made);
            // Switches the mode on or off, false if it already was. The simulation stays, its
            // worker paused while off, so its scene is kept and switching back in the same level is
            // at once. Its direct and reverb effects start afresh and its paths are forgotten; the
            // mixer clears its own state and the voices'. Mixer.
            bool switchTo(bool on);
            // Forgets that the mode was on, as a fresh start of the mixer does.
            void clearActive();
            // Lets go of the simulation when the device will not start, and of everything at
            // shutdown.
            void dropSimulation();
            void shutdown();

            // Game thread, once per update before the voices: follows the level the world is in.
            void followLevel(World* world);
            // Game thread, when a world is about to let go of its level. Only noted: the scene
            // is let go of, or rebuilt, by the next followLevel, as when the level changes.
            void levelUnloading(const ULevel* level);
            // Game thread, once per update after the voices: where sound is heard from, and the
            // sources to simulate. Mixer.
            void hear(const Vector& ear, const Rotator& rotation, std::vector<acoustics::SourceInput> sources);

            // Mixer: sizes the pathed sound's buffers when the HRTF mode is installed.
            void allocateBlocks();
            // Mixer, at the start of each block: takes the newest direct and baked paths if the
            // simulation has published any since the last block, true if so; takeResults then
            // hands them to each voice.
            bool beginBlock(const Coords& listenerAxes);
            void takeResults(int32_t id, RaytracedVoice& voice) const;
            // Mixer, per positioned voice on an HRTF channel: a channel taken by a new voice, the
            // direct path's occlusion and transmission over the voice's mono before the HRTF, and
            // the pathed sound after it.
            void resetChannel(size_t index, RaytracedChannel& channel, bool talk);
            void filterDirect(size_t index, float* mono, const RaytracedVoice& voice, bool talk);
            void mixPath(size_t index, RaytracedVoice& voice, RaytracedChannel& channel, const PathedVoice& from, bool talk, float* out, float* send);
            // Mixer, per channel after the voices: the pathed sound's tail once its voice has ended.
            void mixTail(size_t index, RaytracedChannel& channel, float* out, float* send);
            // Mixer: the reverb simulated around the listener over the send, true if it took the
            // place of the zone's reverb.
            bool mixReverb(const float* send, float* out, bool binaural, bool useReverb);
            // The voice's raytraced fields for the debug overlay; positioned is whether the mode
            // simulates it at all.
            void report(const RaytracedVoice& voice, bool positioned, VoiceStatus& status) const;

        private:
            bool pathing(bool talk) const;
        };

        // The mixer's raytraced mode and the lock that guards what the mixer reads of it, in
        // audio.cc.
        RaytracedMode& raytracedMode();
        dx::profile::Mutex& mixerMutex();
    }
}
