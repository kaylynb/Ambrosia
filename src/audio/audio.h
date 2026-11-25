#pragma once

#include "core/math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace dx {
    class UObject;
    class USound;
    class UMusic;
    class World;

    namespace audio {
        struct ItModule;

        struct DecodedSound {
            uint32_t sampleRate = 0;
            uint32_t channels = 0;
            bool looped = false;
            size_t loopStart = 0;
            size_t loopEnd = 0;
            bool compressed = false;
            // Interleaved 16-bit samples, the only decoded copy that is kept.
            std::vector<int16_t> fixed;

            size_t frames() const;
            float duration() const;
        };

        struct Gains {
            float left = 0;
            float right = 0;
        };

        struct Volumes {
            float sound = 1;
            float music = 1;
            float speech = 1;
        };

        struct MusicStatus {
            const UMusic* music = nullptr;
            int32_t section = -1;
            int32_t order = -1;
            int32_t row = -1;
            float fade = 0;
            bool pending = false;
        };

        struct VoiceStatus {
            int32_t id = 0;
            const USound* sound = nullptr;
            const UObject* actor = nullptr;
            int32_t slot = 0;
            float gainLeft = 0;
            float gainRight = 0;
            float priority = 0;
            bool ambient = false;
            int32_t channel = -1;
            int32_t volume = 0;
            int32_t pan = 0;
            float obstruction = 0;
            double step = 0;
            // For the debug overlay: whether the sound is positioned and, if so, spatialised by
            // the HRTF mode now; where it is in the listener's view axes (x ahead, y right, z up)
            // and how far; the near-source blend (0 at the listener, 1 from a tenth of the radius
            // out); and the overlay's own mute and solo.
            bool attenuated = false;
            bool hrtf = false;
            Vector offset;
            float distance = 0;
            float spread = 1;
            bool muted = false;
            bool soloed = false;
            // Where the sound is, and in the raytracing mode, whether its direct path has been
            // simulated and what it gave: the fraction not occluded and the transmission of
            // what is, at 400 Hz, 2.5 kHz and 15 kHz.
            Vector location;
            // The sample the voice is at in its sound, and its sound's length in samples.
            size_t position = 0;
            size_t frames = 0;
            bool simulated = false;
            float occlusion = 1;
            std::array<float, 3> transmission { 1, 1, 1 };
            // The baked path the raytracing mode found for it, if any: how long and from which
            // way (world axes) it arrives, and per band the pathed sound the mixer adds.
            bool pathFound = false;
            float pathLength = 0;
            Vector pathDirection;
            std::array<float, 3> pathed {};
        };

        // Milliseconds of one core the mixer took per second of sound it mixed, measured over the
        // last whole second in each mode, or negative where that mode has not run for a second.
        struct MixCost {
            float stereo = -1;
            float hrtf = -1;
        };

        struct Placement {
            int32_t pan = 16383;
            float attenuation = 1;
            // Not Galaxy's: what the HRTF mode spatialises from. The source in the listener's view
            // axes (x ahead, y right, z up), its distance, and how far it is from Galaxy's centred
            // pan inside a tenth of the radius, 0 at the listener to 1 at a tenth and beyond.
            Vector offset;
            float distance = 0;
            float spread = 1;
        };

        struct ReverbSettings {
            float volume = 0;
            float cutoff = 0;
            std::array<float, 6> delay {};
            std::array<float, 6> gain {};

            bool operator==(const ReverbSettings&) const = default;
        };

        class Reverb {
        public:
            void configure(const ReverbSettings& settings, uint32_t rate);
            void process(const float* send, float* stereo, size_t frames);
            bool active() const;

        private:
            bool enabled = false;
            size_t position = 0;
            std::array<size_t, 6> delays {};
            std::array<float, 6> direct {};
            std::array<float, 6> feedback {};
            std::array<float, 6> through {};
            std::array<float, 6> lowpass {};
            std::array<float, 2> last {};
            float smoothing = 0;
            float keep = 0;
            float volume = 0;
            std::vector<float> lines;
        };

        constexpr int32_t SlotNone = 0;
        constexpr int32_t SlotAmbient = 4;
        constexpr int32_t SlotTalk = 5;

        // Opens the sound device, or with nullDevice one that plays nowhere at the same pace.
        bool initialize(bool nullDevice = false);
        bool initializeOffline(uint32_t sampleRate);
        void shutdown();
        // world is the world in play, or null when there is none.
        void update(World* world, const Vector& listenerLocation, const Rotator& listenerRotation, float deltaSeconds);
        void render(float* stereo, size_t frames);
        // Unpositioned sounds (!attenuate) never reach the reverb; dry keeps a positioned one out
        // of it too, as for the infolink, which plays from the player.
        int32_t playSound(USound* sound, UObject* actor, int32_t slot, const Vector& location, float volume, float radius, float pitch, bool attenuate, bool noOverride = false, int32_t channelId = 0, bool dry = false);
        void actorDestroyed(UObject* actor);
        void stopSound(int32_t id);
        // Stops by the script's sound id, as Actor.StopSound does.
        void stopSoundId(int32_t id);
        bool isPlaying(int32_t id);
        float duration(USound* sound);
        void playMusic(UMusic* music, uint8_t section, uint8_t transition);
        void setVolumes(float sound, float music, float speech);
        // Re-reads EffectsChannels and UseReverb, which the sound menu changes in game, and
        // Ambrosia's AmbrosiaHRTF, switching the HRTF mode on or off at once.
        void applyMenuSettings();
        // The volumes from the audio device's section, then applyMenuSettings: at start, and
        // whenever a setting in that section (or Engine.Engine, which names it) changes.
        void applySettings();
        Volumes volumes();
        // Whether the Ambrosia HRTF mode (AmbrosiaHRTF in the audio section) is on and Steam Audio
        // loaded, so positioned sounds are spatialised by it instead of panned.
        bool hrtfActive();
        // Whether the Ambrosia raytraced mode (AmbrosiaRaytracedAudio) is on, with its simulation. Its
        // controls and status are in audio/raytraced.h.
        bool raytracedActive();
        MusicStatus musicStatus();
        std::vector<VoiceStatus> voices();
        // Debug overlay only, kept on the voice and gone when it ends: a muted voice plays on
        // silently, and while any voice is soloed only soloed voices are heard.
        void setVoiceMuted(int32_t id, bool muted);
        void setVoiceSoloed(int32_t id, bool soloed);
        MixCost mixCost();
        // Investigation: the stereo mixer's cost with voices synthetic voices and the level's
        // music, mixed offline on the calling thread; a line of figures (audio.cc).
        std::string benchmarkMix(int voices, double seconds, size_t block);

        std::shared_ptr<const DecodedSound> decode(USound* sound);

        // What the decoded-sound cache holds: sounds, their packed bytes and decoded sample bytes.
        struct CacheStats {
            size_t sounds = 0;
            uint64_t sourceBytes = 0;
            uint64_t decodedBytes = 0;
            size_t modules = 0;
            uint64_t limitBytes = 0;
        };

        // The most decoded sample bytes the cache keeps; the least recently used sounds are dropped
        // past it and decoded again when next played. 0 keeps everything.
        void setSoundCacheLimit(uint64_t bytes);

        CacheStats cacheStats();

        // For releasing a level: the sounds and music still in use, and a notice that objects
        // are about to be deleted so voices and caches let go of them.
        void heldObjects(std::vector<UObject*>& objects);
        void objectsReleased(const std::unordered_set<const UObject*>& objects);
        std::shared_ptr<const DecodedSound> decodeBytes(const std::vector<uint8_t>& bytes);
        std::shared_ptr<const ItModule> loadModule(UMusic* music);
        Placement place(const Vector& listenerLocation, const Rotator& listenerRotation, const Vector& source, float radius);
        int32_t masterVolume(uint8_t sound, uint8_t speech);
        int32_t voiceVolume(float attenuation, float obstruction, uint8_t sound, uint8_t speech, bool speechSlot, float volume);
        Gains channelGains(int32_t volume, int32_t pan, int32_t master, int32_t send);
        float dopplerShift(const Vector& velocity, const Vector& source, const Vector& listener, float speed);
        uint32_t voiceStep(uint32_t sampleRate, float pitch, float doppler, uint32_t outputRate);
        ReverbSettings zoneReverb(UObject* zone);
        ReverbSettings reverbSettings();
        float ambientRadius(uint8_t soundRadius);
        char speechPhoneme(const DecodedSound& pcm, size_t position, size_t end = SIZE_MAX);
        void setLightRandom(float (*random)(int32_t index));
    }
}
