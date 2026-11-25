#include "audio/audio.h"

#include "audio/extended.h"
#include "audio/module.h"
#include "audio/output.h"
#include "audio/raytraced_mode.h"
#include "audio/steam_audio.h"
#include "core/config.h"
#include "core/settings.h"
#include "core/gc.h"
#include "core/log.h"
#include "core/objects.h"
#include "core/profile.h"
#include "engine/light_effects.h"
#include "engine/actor.h"
#include "engine/level_events.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "core/text.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dx::audio {
    namespace {
        using Ext = Extended;

        constexpr uint8_t TransitionFade = 3;
        constexpr uint8_t TransitionFastFade = 4;
        constexpr uint8_t TransitionSlowFade = 5;
        constexpr uint8_t SectionSilence = 255;
        constexpr int32_t MaxPan = 32767;
        constexpr int32_t CenterPan = 16383;
        constexpr int32_t ReverbSend = 127;
        constexpr int32_t DrySend = 128;
        constexpr size_t ReverbFrames = 16384;
        constexpr uint8_t LT_None = 0;
        constexpr uint8_t LT_Pulse = 2;
        constexpr uint8_t LT_Blink = 3;
        constexpr uint8_t LT_Flicker = 4;
        constexpr uint8_t LT_Strobe = 5;
        constexpr uint8_t LT_SubtlePulse = 7;
        constexpr uint8_t LT_TexturePaletteOnce = 8;
        constexpr uint8_t LT_TexturePaletteLoop = 9;

        float (*lightRandomSource)(int32_t) = nullptr;

        struct Voice {
            int32_t id = 0;
            // The id Galaxy keeps for the channel: the script's sound id
            // (actor index * 16 + slot * 2 + bNoOverride), or a fresh negative
            // multiple of 16 for SLOT_None, which nothing can stop by id.
            int32_t channelId = 0;
            std::shared_ptr<const DecodedSound> pcm;
            const USound* sound = nullptr;
            size_t position = 0;
            uint32_t fraction = 0;
            uint32_t step = 65536;
            float gainLeft = 0;
            float gainRight = 0;
            int32_t dryLeft = 0;
            int32_t dryRight = 0;
            int32_t wetLeft = 0;
            int32_t wetRight = 0;
            float priority = 0;
            bool loop = false;
            bool stopping = false;
            bool done = false;
            bool ambient = false;
            bool pending = true;
            UObject* actor = nullptr;
            int32_t slot = 0;
            int32_t channel = -1;
            Vector location;
            float volume = 1;
            float radius = 1600;
            float pitch = 1;
            float doppler = 1;
            float obstruction = 0;
            int32_t level = 0;
            int32_t pan = CenterPan;
            bool attenuate = true;
            // Kept out of the reverb: the interface, the infolink and other unpositioned sounds.
            // Galaxy sends every sound to it.
            bool dry = false;
            float elapsed = 0;
            float nextPhonemeTime = 0;
            // The HRTF mode's placement and levels: Galaxy's dry and reverb levels without the pan.
            steam::Direction hrtfDirection;
            float hrtfSpread = 1;
            int32_t hrtfDry = 0;
            int32_t hrtfWet = 0;
            // Shown by the debug overlay: the placement in view axes and its distance.
            Vector offset;
            float distance = 0;
            // The debug overlay's mute and solo.
            bool debugMute = false;
            bool debugSolo = false;
            RaytracedVoice raytraced;
        };

        // The HRTF mode's state for one channel's binaural effect: the voice it last spatialised,
        // and the levels its tail keeps once that voice has ended.
        struct HrtfChannel {
            int32_t owner = 0;
            bool mixed = false;
            bool tail = false;
            float dry = 0;
            float wet = 0;
            RaytracedChannel raytraced;
        };

        constexpr size_t MaxChannels = 64;

        void fft(std::array<float, 1024>& re, std::array<float, 1024>& im)
        {
            std::array<float, 1024> input = re;
            for (int i = 0; i < 1024; i++) {
                int reversed = 0;
                int value = i;
                for (int bit = 0; bit < 10; bit++) {
                    reversed = (reversed << 1) | (value & 1);
                    value >>= 1;
                }
                re[static_cast<size_t>(reversed)] = input[static_cast<size_t>(i)];
                im[static_cast<size_t>(reversed)] = 0;
            }
            int blockEnd = 1;
            for (int blockSize = 2; blockSize <= 1024; blockSize <<= 1) {
                float delta = 6.2831854820251465f / static_cast<float>(blockSize);
                float sm2 = std::sin(-2 * delta);
                float sm1 = std::sin(-delta);
                float cm2 = std::cos(-2 * delta);
                float cm1 = std::cos(-delta);
                float w = 2 * cm1;
                for (int i = 0; i < 1024; i += blockSize) {
                    float ar2 = cm2;
                    float ar1 = cm1;
                    float ai2 = sm2;
                    float ai1 = sm1;
                    for (int j = i, n = 0; n < blockEnd; j++, n++) {
                        float ar0 = w * ar1 - ar2;
                        ar2 = ar1;
                        ar1 = ar0;
                        float ai0 = w * ai1 - ai2;
                        ai2 = ai1;
                        ai1 = ai0;
                        size_t a = static_cast<size_t>(j);
                        size_t b = static_cast<size_t>(j + blockEnd);
                        float tr = ar0 * re[b] - ai0 * im[b];
                        float ti = ar0 * im[b] + ai0 * re[b];
                        re[b] = re[a] - tr;
                        im[b] = im[a] - ti;
                        re[a] += tr;
                        im[a] += ti;
                    }
                }
                blockEnd = blockSize;
            }
        }

        struct State {
            bool active = false;
            bool offline = false;
            bool updated = false;

            uint32_t rate = 44100;
            size_t channels = 16;
            float ambientFactor = 0.7f;
            float dopplerSpeed = 6500;
            bool useReverb = true;
            bool digitalMusic = true;
            DX_LOCKABLE(mutex, "Audio state");
            std::vector<Voice> voices;
            int32_t nextId = 1;
            int32_t noneSlotCount = 0;
            Vector listener;
            Rotator listenerRotation;
            // The listener's rotation as the mixer reads it, copied from listenerRotation while
            // update holds the lock; update writes the others before taking it.
            Rotator mixRotation;
            Vector viewer;
            Vector ear;
            float soundVolume = 1;
            float musicVolume = 1;
            float speechVolume = 1;
            std::unique_ptr<ModulePlayer> music;
            std::unique_ptr<ModulePlayer> pendingMusic;
            UMusic* musicObject = nullptr;
            int32_t musicSection = -1;
            bool musicPlaying = false;
            // The order the heard music is at, or 255 with none, written by the
            // mixer and read on the game thread (Galaxy's current order byte).
            std::atomic<int32_t> playingOrder { 255 };
            // The player whose music request is fading out the old music.
            UObject* requestPlayer = nullptr;
            float musicFade = 1;
            float fadeSpeed = 0;
            bool requested = false;
            UMusic* requestObject = nullptr;
            int32_t requestSection = -1;
            std::vector<float> scratch;
            std::vector<float> send;
            Reverb reverb;
            ReverbSettings reverbSettings;
            // The Ambrosia HRTF mode: asked for by AmbrosiaHRTF, and on while binaural is loaded. The
            // mix is then made in whole Steam Audio blocks, handed out from block.
            bool hrtfWanted = false;
            bool hrtfUnavailable = false;
            // The Ambrosia raytraced mode, on top of the HRTF mode (raytraced_mode.cc).
            RaytracedMode raytraced;
            std::unique_ptr<steam::Binaural> binaural;
            std::vector<HrtfChannel> hrtfChannels;
            std::vector<float> block;
            size_t blockRead = steam::BlockFrames;
            std::vector<float> mono;
            std::vector<float> earLeft;
            std::vector<float> earRight;
            // Set once the debug overlay mutes or solos a voice; until then the mixer never looks.
            bool debugVoices = false;
            // The mixing time over the frames mixed so far in the current mode, and the last
            // whole second's figure for each mode.
            double costSeconds = 0;
            size_t costFrames = 0;
            bool costHrtf = false;
            MixCost cost;
            // Investigation (benchmarkMix): how often the device asks for sound, for how many
            // frames, and how long it waits for the lock first.
            std::atomic<uint64_t> deviceCalls { 0 };
            std::atomic<uint64_t> deviceFrames { 0 };
            std::atomic<int64_t> deviceWaitNs { 0 };

            struct CachedSound {
                std::shared_ptr<const DecodedSound> pcm;
                uint64_t lastUse = 0;
            };

            std::unordered_map<const USound*, CachedSound> sounds;
            uint64_t soundUse = 0;
            uint64_t soundBytes = 0;
            uint64_t soundLimit = 64ull * 1024 * 1024;
            std::unordered_map<const UMusic*, std::shared_ptr<const ItModule>> modules;
        };

        State& state()
        {
            static State instance;
            return instance;
        }

        std::array<int32_t, 2> channelLevels(int32_t volume, int32_t pan, int32_t master, int32_t send)
        {
            int32_t level = (0x7e02 * (master * master * 2)) >> 15;
            for (int i = 0; i < 4; i++) {
                level = (level * 0x7fff) >> 15;
            }
            level = (level * volume) >> 15;
            const std::array<int16_t, 32768>& table = galaxyPanTable();
            int32_t left = (((table[static_cast<size_t>(MaxPan - pan)] * level) >> 15) * send) >> 7;
            int32_t right = (((table[static_cast<size_t>(pan)] * level) >> 15) * send) >> 7;
            return { left, right };
        }

        uint8_t volumeByte(float volume)
        {
            return static_cast<uint8_t>(std::lround(std::clamp(volume, 0.0f, 1.0f) * 255.0f));
        }

        Ext squaredDistance(const Vector& a, const Vector& b)
        {
            Ext dx = static_cast<Ext>(a.x) - static_cast<Ext>(b.x);
            Ext dy = static_cast<Ext>(a.y) - static_cast<Ext>(b.y);
            Ext dz = static_cast<Ext>(a.z) - static_cast<Ext>(b.z);
            return dx * dx + dy * dy + dz * dz;
        }

        Ext categoryScale(uint8_t sound, uint8_t speech, bool speechSlot)
        {
            if (sound == speech) {
                return static_cast<Ext>(sound) * static_cast<Ext>(1.0f / 255.0f);
            }
            if (speechSlot && sound > speech) {
                return static_cast<Ext>(speech) / static_cast<Ext>(sound);
            }
            if (!speechSlot && sound < speech) {
                return static_cast<Ext>(sound) / static_cast<Ext>(speech);
            }
            return Ext(1);
        }

        float sinTab(int32_t angle)
        {
            return static_cast<float>(std::sin(static_cast<double>((angle >> 2) & 16383) * 6.283185307179586 / 16384.0));
        }

        float cosTab(int32_t angle)
        {
            return sinTab(angle + 16384);
        }

        Coords viewAxes(const Rotator& rotation)
        {
            float sp = sinTab(rotation.pitch);
            float cp = cosTab(rotation.pitch);
            float sy = sinTab(rotation.yaw);
            float cy = cosTab(rotation.yaw);
            float sr = sinTab(rotation.roll);
            float cr = cosTab(rotation.roll);
            Coords c;
            c.xAxis = { cp * cy, cp * sy, sp };
            c.yAxis = { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
            c.zAxis = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
            return c;
        }

        float ambientBrightness(UObject* actor, float brightness, float time)
        {
            uint8_t type = actor::LightType(actor);
            if (type == LT_None) {
                return brightness;
            }
            float lit = static_cast<float>(static_cast<Ext>(actor::LightBrightness(actor)) * static_cast<Ext>(brightness) * static_cast<Ext>(1.0 / 255.0));
            double cycle = static_cast<double>(time) * 35.0 * 65536.0;
            int32_t phase = actor::LightPhase(actor) << 8;
            if (type == LT_Pulse || type == LT_SubtlePulse) {
                int32_t angle = static_cast<int32_t>(cycle / std::max<int32_t>(actor::LightPeriod(actor), 1) + phase);
                double depth = type == LT_Pulse ? 0.39 : 0.09;
                double base = type == LT_Pulse ? 0.6 : 0.9;
                lit = static_cast<float>(static_cast<double>(lit) * (base + depth * static_cast<double>(sinTab(angle))));
            } else if (type == LT_Blink) {
                if (static_cast<int32_t>(cycle / (actor::LightPeriod(actor) + 1) + phase) & 1) {
                    lit = 0;
                }
            } else if (type == LT_Flicker) {
                float random = lightRandomSource ? lightRandomSource(static_cast<int32_t>(actor->handle & 0xff)) : 1.0f;
                lit = random < 0.5f ? 0.0f : lit * random;
            } else if (type == LT_Strobe) {
                if (strobeDark(actor, time)) {
                    lit = 0;
                }
            } else if (type == LT_TexturePaletteOnce || type == LT_TexturePaletteLoop) {
                int32_t index = 0;
                if (type == LT_TexturePaletteOnce) {
                    float defaultLife = actor->cls && actor->cls->defaultObject && actor->cls->defaultObject->data ? actor::LifeSpan(actor->cls->defaultObject) : 0.0f;
                    double fraction = 1.0 - static_cast<double>(actor::LifeSpan(actor)) / static_cast<double>(defaultLife);
                    fraction = std::isnan(fraction) ? 0.0 : std::clamp(fraction, 0.0, 1.0);
                    index = std::clamp(static_cast<int32_t>(std::floor(static_cast<float>(fraction * 255.0))), 0, 255);
                } else {
                    double loop = static_cast<double>(time) * 35.0 / std::max<int32_t>(actor::LightPeriod(actor), 1) + actor::LightPhase(actor);
                    index = (static_cast<int32_t>(loop * 256.0) & 255) % 255;
                }
                UObject* skinObject = actor::Skin(actor);
                if (skinObject) {
                    objects::ensureLoaded(skinObject);
                }
                UTexture* skin = cast<UTexture>(skinObject);
                UPalette* palette = skin ? skin->palette() : nullptr;
                if (palette && palette->colors.size() >= 256) {
                    uint32_t entry = palette->colors[static_cast<size_t>(index)];
                    double r = static_cast<double>(entry & 0xff);
                    double g = static_cast<double>((entry >> 8) & 0xff);
                    double b = static_cast<double>((entry >> 16) & 0xff);
                    lit = static_cast<float>(static_cast<double>(lit) * ((2.0 * r + 3.0 * g + b) * (1.0 / 1536.0)) * 2.8);
                }
            }
            return std::clamp(lit, 0.0f, 1.0f);
        }

        uint32_t readU32(const uint8_t* bytes)
        {
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        }

        void readLoop(const std::vector<uint8_t>& bytes, DecodedSound& pcm)
        {
            if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
                return;
            }
            size_t offset = 12;
            while (offset + 8 <= bytes.size()) {
                size_t size = readU32(bytes.data() + offset + 4);
                if (std::memcmp(bytes.data() + offset, "smpl", 4) == 0) {
                    if (size < 60 || offset + 8 + size > bytes.size()) {
                        return;
                    }
                    const uint8_t* chunk = bytes.data() + offset + 8;
                    if (readU32(chunk + 28) == 0) {
                        return;
                    }
                    size_t start = readU32(chunk + 44);
                    size_t end = static_cast<size_t>(readU32(chunk + 48)) + 1;
                    end = std::min(end, pcm.frames());
                    if (end > start) {
                        pcm.looped = true;
                        pcm.loopStart = start;
                        pcm.loopEnd = end;
                    }
                    return;
                }
                offset += 8 + size + (size & 1);
            }
        }

        float priorityOf(const State& s, const Voice& voice)
        {
            if (!voice.attenuate) {
                return voice.volume;
            }
            Ext distance = static_cast<Ext>(std::sqrt(static_cast<double>(squaredDistance(voice.location, s.viewer))));
            return static_cast<float>((Ext(1) - distance / static_cast<Ext>(voice.radius)) * static_cast<Ext>(voice.volume));
        }

        void refreshGains(State& s, Voice& voice)
        {
            Placement placement;
            if (voice.attenuate) {
                placement = place(s.listener, s.listenerRotation, voice.location, voice.radius);
            }
            uint8_t sound = volumeByte(s.soundVolume);
            uint8_t speech = volumeByte(s.speechVolume);
            voice.level = voiceVolume(placement.attenuation, voice.obstruction, sound, speech, voice.slot == SlotTalk, voice.volume);
            voice.pan = placement.pan;
            int32_t master = masterVolume(sound, speech);
            std::array<int32_t, 2> dry = channelLevels(voice.level, voice.pan, master, DrySend);
            std::array<int32_t, 2> wet = voice.dry ? std::array<int32_t, 2> {} : channelLevels(voice.level, voice.pan, master, ReverbSend);
            voice.dryLeft = dry[0];
            voice.dryRight = dry[1];
            voice.wetLeft = wet[0];
            voice.wetRight = wet[1];
            // A pan hard right gives the right channel the whole level, so it is the level without
            // the pan law.
            voice.hrtfDry = channelLevels(voice.level, MaxPan, master, DrySend)[1];
            voice.hrtfWet = voice.dry ? 0 : channelLevels(voice.level, MaxPan, master, ReverbSend)[1];
            voice.hrtfSpread = placement.spread;
            voice.offset = placement.offset;
            voice.distance = placement.distance;
            if (placement.distance > 0) {
                // Unreal's view axes are x ahead, y right and z up; Steam Audio's are +x right,
                // +y up and -z ahead.
                float distance = placement.distance;
                voice.hrtfDirection = { placement.offset.y / distance, placement.offset.z / distance, -placement.offset.x / distance };
            } else {
                voice.hrtfDirection = {};
            }
            Gains gains = channelGains(voice.level, voice.pan, master, DrySend);
            voice.gainLeft = gains.left;
            voice.gainRight = gains.right;
            voice.priority = priorityOf(s, voice);
            if (voice.pcm) {
                voice.step = voiceStep(voice.pcm->sampleRate, voice.pitch, voice.doppler, s.rate);
            }
        }

        bool alive(const Voice& voice)
        {
            return !voice.done && !voice.stopping;
        }

        constexpr float LevelUnit = 1.0f / 1073741824.0f;
        constexpr float SampleScale = 1.0f / 32768.0f;

        // Steps the voice through frames output frames, passing each frame's interpolated left and
        // right samples (equal for mono) to sink, until it ends.
        template <typename Sink>
        void resample(Voice& voice, size_t frames, Sink&& sink)
        {
            const DecodedSound& pcm = *voice.pcm;
            size_t total = pcm.frames();
            if (total == 0 || voice.stopping) {
                voice.done = true;
                return;
            }
            bool looping = voice.loop || pcm.looped;
            size_t loopStart = pcm.looped ? pcm.loopStart : 0;
            size_t loopEnd = pcm.looped ? pcm.loopEnd : total;
            const int16_t* data = pcm.fixed.data();
            uint32_t channels = pcm.channels;
            uint32_t step = voice.step & ~1u;
            for (size_t i = 0; i < frames; i++) {
                if (looping && voice.position >= loopEnd) {
                    voice.position = loopStart + (voice.position - loopStart) % (loopEnd - loopStart);
                }
                if (voice.position >= total) {
                    voice.done = true;
                    return;
                }
                size_t index = voice.position;
                size_t next = index + 1 < total ? index + 1 : index;
                int32_t later = static_cast<int32_t>(voice.fraction >> 1);
                int32_t earlier = 32767 - later;
                size_t right = channels >= 2 ? 1 : 0;
                float left = static_cast<float>(data[index * channels] * earlier + data[next * channels] * later);
                float other = static_cast<float>(data[index * channels + right] * earlier + data[next * channels + right] * later);
                sink(i, left, other);
                uint32_t fraction = voice.fraction + (step & 0xffff);
                voice.position += (step >> 16) + (fraction >> 16);
                voice.fraction = fraction & 0xffff;
            }
        }

        // Mixes a voice by Galaxy's stereo path, to the reverb send too unless it is dry.
        void mixVoice(Voice& voice, float* out, float* send, size_t frames, bool dry = false)
        {
            float dryLeft = static_cast<float>(voice.dryLeft) * LevelUnit;
            float dryRight = static_cast<float>(voice.dryRight) * LevelUnit;
            float wetLeft = static_cast<float>(voice.wetLeft) * LevelUnit;
            float wetRight = static_cast<float>(voice.wetRight) * LevelUnit;
            const float scale = SampleScale;
            if (dry) {
                resample(voice, frames, [&](size_t i, float left, float other) {
                    out[i * 2] += left * dryLeft * scale;
                    out[i * 2 + 1] += other * dryRight * scale;
                });
                return;
            }
            resample(voice, frames, [&](size_t i, float left, float other) {
                out[i * 2] += left * dryLeft * scale;
                out[i * 2 + 1] += other * dryRight * scale;
                send[i * 2] += left * wetLeft * scale;
                send[i * 2 + 1] += other * wetRight * scale;
            });
        }

        void mixEars(const State& s, const HrtfChannel& channel, float* out, float* send)
        {
            for (size_t i = 0; i < steam::BlockFrames; i++) {
                out[i * 2] += s.earLeft[i] * channel.dry;
                out[i * 2 + 1] += s.earRight[i] * channel.dry;
                send[i * 2] += s.earLeft[i] * channel.wet;
                send[i * 2 + 1] += s.earRight[i] * channel.wet;
            }
        }

        // The HRTF mode's mix of a positioned voice over one block: the voice in mono through its
        // channel's binaural effect, at Galaxy's dry and reverb levels without the pan.
        void mixBinaural(State& s, Voice& voice, float* out, float* send)
        {
            std::fill(s.mono.begin(), s.mono.end(), 0.0f);
            float* mono = s.mono.data();
            resample(voice, steam::BlockFrames, [&](size_t i, float left, float other) {
                mono[i] = (left + other) * 0.5f * SampleScale;
            });
            // A stopped voice plays nothing more; its effect's tail rings out in mixTails.
            if (voice.stopping || voice.channel < 0 || static_cast<size_t>(voice.channel) >= s.hrtfChannels.size()) {
                return;
            }
            size_t index = static_cast<size_t>(voice.channel);
            HrtfChannel& channel = s.hrtfChannels[index];
            bool talk = voice.slot == SlotTalk;
            if (channel.owner != voice.id) {
                s.binaural->reset(index);
                s.raytraced.resetChannel(index, channel.raytraced, talk);
                channel.owner = voice.id;
            }
            s.raytraced.filterDirect(index, mono, voice.raytraced, talk);
            float loudness = steam::Binaural::loudness(voice.hrtfSpread);
            channel.dry = static_cast<float>(voice.hrtfDry) * LevelUnit * loudness;
            channel.wet = static_cast<float>(voice.hrtfWet) * LevelUnit * loudness;
            channel.mixed = true;
            channel.tail = true;
            s.binaural->apply(index, mono, s.earLeft.data(), s.earRight.data(), voice.hrtfDirection, voice.hrtfSpread);
            mixEars(s, channel, out, send);
            PathedVoice pathed { voice.distance, voice.radius, voice.hrtfDirection, voice.hrtfSpread, static_cast<float>(voice.hrtfDry) * LevelUnit, static_cast<float>(voice.hrtfWet) * LevelUnit };
            s.raytraced.mixPath(index, voice.raytraced, channel.raytraced, pathed, talk, out, send);
        }

        // The debug overlay's mute: the voice moves on as if heard. Its binaural effect, if it
        // has one, rings out and starts afresh when the voice is heard again.
        void silenceVoice(State& s, Voice& voice, size_t frames)
        {
            resample(voice, frames, [](size_t, float, float) {});
            if (s.binaural && voice.attenuate && voice.channel >= 0 && static_cast<size_t>(voice.channel) < s.hrtfChannels.size()) {
                s.hrtfChannels[static_cast<size_t>(voice.channel)].owner = 0;
            }
        }

        // Lets each binaural effect whose voice has ended ring out at that voice's last levels.
        void mixTails(State& s, float* out, float* send)
        {
            for (size_t index = 0; index < s.hrtfChannels.size(); index++) {
                HrtfChannel& channel = s.hrtfChannels[index];
                if (channel.tail && !channel.mixed) {
                    channel.tail = s.binaural->tail(index, s.earLeft.data(), s.earRight.data());
                    mixEars(s, channel, out, send);
                }
                channel.mixed = false;
                s.raytraced.mixTail(index, channel.raytraced, out, send);
            }
        }

        void startRequestedMusic(State& s)
        {
            if (s.requestObject != s.musicObject) {
                s.music = std::move(s.pendingMusic);
            }
            s.pendingMusic.reset();
            s.musicObject = s.requestObject;
            s.musicSection = s.requestSection;
            s.musicFade = 1;
            s.fadeSpeed = 0;
            s.requested = false;
            s.musicPlaying = s.music && s.musicSection != SectionSilence;
            if (s.musicPlaying) {
                s.music->restart(s.musicSection);
            }
        }

        // How far past silence a music change's fade runs before the next music starts, in the
        // units of the original's Latency setting (milliseconds of output buffer), whose default
        // it is. Ambrosia's output has no such setting, so the key is not read.
        constexpr float MusicStartDelay = 40;

        void mixMusic(State& s, float* out, size_t frames)
        {
            if (!s.music || !s.musicPlaying) {
                s.playingOrder = 255;
                return;
            }
            if (s.scratch.size() < frames * 2) {
                s.scratch.resize(frames * 2);
            }
            s.music->render(s.scratch.data(), frames);
            for (size_t i = 0; i < frames; i++) {
                if (s.requested) {
                    s.musicFade -= s.fadeSpeed / static_cast<float>(s.rate);
                }
                int32_t master = static_cast<int32_t>(static_cast<Ext>(std::max(s.musicFade, 0.0f)) * static_cast<Ext>(s.musicVolume) * Ext(127));
                float gain = static_cast<float>(master * master) / 16129.0f;
                out[i * 2] += s.scratch[i * 2] * gain;
                out[i * 2 + 1] += s.scratch[i * 2 + 1] * gain;
            }
            if (s.requested && s.musicFade < -s.fadeSpeed * 0.002f * MusicStartDelay) {
                startRequestedMusic(s);
            }
            s.playingOrder = s.music && s.musicPlaying ? s.music->order() : 255;
        }

        void mixAll(State& s, float* out, size_t frames)
        {
            std::fill(out, out + frames * 2, 0.0f);
            if (s.send.size() < frames * 2) {
                s.send.resize(frames * 2);
            }
            std::fill(s.send.begin(), s.send.begin() + static_cast<std::ptrdiff_t>(frames * 2), 0.0f);
            // The raytraced mode's newest paths, if the simulation has published any since the
            // last block.
            if (s.raytraced.on() && s.raytraced.beginBlock(viewAxes(s.mixRotation))) {
                for (Voice& voice : s.voices) {
                    s.raytraced.takeResults(voice.id, voice.raytraced);
                }
            }
            bool soloing = s.debugVoices && std::any_of(s.voices.begin(), s.voices.end(), [](const Voice& voice) {
                return alive(voice) && voice.debugSolo;
            });
            for (Voice& voice : s.voices) {
                if (!voice.done && (!voice.pending || voice.stopping)) {
                    // With the HRTF mode on, frames is always a whole block (mixOutput).
                    if (s.debugVoices && (voice.debugMute || (soloing && !voice.debugSolo))) {
                        silenceVoice(s, voice, frames);
                    } else if (s.binaural && voice.attenuate) {
                        mixBinaural(s, voice, out, s.send.data());
                    } else {
                        mixVoice(voice, out, s.send.data(), frames, voice.dry);
                    }
                }
            }
            if (s.binaural) {
                mixTails(s, out, s.send.data());
            }
            // The raytraced mode's reverb, simulated around the listener, takes the place of the
            // zone's unless the debug overlay asks for the zone's to compare.
            if (!s.raytraced.mixReverb(s.send.data(), out, s.binaural != nullptr, s.useReverb)) {
                s.reverb.process(s.send.data(), out, frames);
            }
            s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(), [](const Voice& voice) {
                return voice.done;
            }),
                s.voices.end());
            mixMusic(s, out, frames);
            for (size_t i = 0; i < frames * 2; i++) {
                out[i] = std::clamp(out[i], -1.0f, 1.0f);
            }
        }

        // Mixes frames of output. The stereo mixer works at any count; the HRTF mode mixes whole
        // Steam Audio blocks and hands them out, so what it plays is up to a block late and the
        // same however the frames are asked for.
        void mixFrames(State& s, float* out, size_t frames)
        {
            if (!s.binaural) {
                mixAll(s, out, frames);
                return;
            }
            while (frames > 0) {
                if (s.blockRead == steam::BlockFrames) {
                    DX_ZONE_NAMED("HRTF block");
                    mixAll(s, s.block.data(), steam::BlockFrames);
                    s.blockRead = 0;
                }
                size_t count = std::min(frames, steam::BlockFrames - s.blockRead);
                std::copy_n(s.block.data() + s.blockRead * 2, count * 2, out);
                out += count * 2;
                frames -= count;
                s.blockRead += count;
            }
        }

        // Mixes as mixFrames does, timing it for the debug overlay's figure per mode.
        void mixOutput(State& s, float* out, size_t frames)
        {
            auto start = std::chrono::steady_clock::now();
            mixFrames(s, out, frames);
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            bool hrtf = s.binaural != nullptr;
            if (hrtf != s.costHrtf) {
                s.costHrtf = hrtf;
                s.costSeconds = 0;
                s.costFrames = 0;
            }
            s.costSeconds += seconds;
            s.costFrames += frames;
            if (s.costFrames >= s.rate) {
                float cost = static_cast<float>(s.costSeconds * 1000.0 * static_cast<double>(s.rate) / static_cast<double>(s.costFrames));
                (hrtf ? s.cost.hrtf : s.cost.stereo) = cost;
                s.costSeconds = 0;
                s.costFrames = 0;
            }
        }

        // Makes the binaural effects for the HRTF mode, or null when Steam Audio cannot be used.
        // That is only reported, and only tried, once: asking again keeps the stereo pan quietly.
        std::unique_ptr<steam::Binaural> makeBinaural(State& s)
        {
            if (s.hrtfUnavailable) {
                return nullptr;
            }
            std::unique_ptr<steam::Binaural> binaural = steam::Binaural::create(s.rate, MaxChannels);
            if (!binaural) {
                s.hrtfUnavailable = true;
                log::warn("DevAudio", "AmbrosiaHRTF: Steam Audio is not available, so sounds are panned in stereo");
            }
            return binaural;
        }

        // Switches the mixer to the HRTF mode with binaural, or back to the stereo mixer with
        // null. Either way it starts as from a fresh start: no effect state or tails, and nothing
        // left in the block buffer, whose unplayed frames (up to a block) are dropped. Voices
        // carry on from where the mixer has got to. Called with the mixer locked.
        void installBinaural(State& s, std::unique_ptr<steam::Binaural>& binaural)
        {
            std::swap(s.binaural, binaural);
            s.blockRead = steam::BlockFrames;
            s.hrtfChannels.clear();
            if (s.binaural) {
                s.hrtfChannels.assign(MaxChannels, {});
                s.block.assign(steam::BlockFrames * 2, 0.0f);
                s.mono.assign(steam::BlockFrames, 0.0f);
                s.earLeft.assign(steam::BlockFrames, 0.0f);
                s.earRight.assign(steam::BlockFrames, 0.0f);
                s.raytraced.allocateBlocks();
            }
        }

        // Switches the raytraced mode on or off; it is only on over the HRTF mode. Either way the
        // mix starts as from a fresh start in the mode switched to: the block buffer, the
        // binaural, direct and reverb effects' state and the zone reverb's lines are cleared, and
        // the voices' paths are forgotten. Voices carry on. Called with the mixer locked.
        void switchRaytracing(State& s, bool on)
        {
            if (!s.raytraced.switchTo(on && s.binaural)) {
                return;
            }
            s.blockRead = steam::BlockFrames;
            if (s.binaural) {
                s.hrtfChannels.assign(MaxChannels, {});
            }
            s.reverb.configure(s.reverbSettings, s.rate);
            for (Voice& voice : s.voices) {
                voice.raytraced = {};
            }
        }

        void startHrtf(State& s)
        {
            s.raytraced.clearActive();
            std::unique_ptr<steam::Binaural> binaural = s.hrtfWanted ? makeBinaural(s) : nullptr;
            installBinaural(s, binaural);
            if (s.raytraced.wanted() && s.binaural && !s.raytraced.hasSimulation()) {
                if (std::unique_ptr<acoustics::Simulation> simulation = s.raytraced.makeSimulation(s.rate, MaxChannels)) {
                    s.raytraced.install(std::move(simulation));
                }
            }
            switchRaytracing(s, s.raytraced.wanted());
        }

        // The output device's callback (audio/output.h).
        void deviceMix(void* user, float* out, size_t frames)
        {
            DX_THREAD_NAME("Audio device");
            DX_ZONE_NAMED("Audio mix callback");
            State& s = *static_cast<State*>(user);
            s.deviceCalls.fetch_add(1, std::memory_order_relaxed);
            s.deviceFrames.fetch_add(frames, std::memory_order_relaxed);
            int64_t waitStart = profile::nanoseconds();
            DX_LOCK_GUARD(lock, s.mutex, "Mixer waits for the audio state");
            s.deviceWaitNs.fetch_add(profile::nanoseconds() - waitStart, std::memory_order_relaxed);
            mixOutput(s, out, frames);
        }

        Voice makeVoice(State& s, std::shared_ptr<const DecodedSound> pcm, USound* sound, UObject* actor, int32_t slot, const Vector& location, float volume, float radius, float pitch, bool attenuate, bool dry = false)
        {
            Voice voice;
            voice.pcm = std::move(pcm);
            voice.sound = sound;
            voice.actor = actor;
            voice.slot = slot;
            voice.location = location;
            voice.volume = std::max(volume, 0.0f);
            voice.radius = radius > 0 ? radius : 1600.0f;
            voice.pitch = pitch;
            voice.attenuate = attenuate;
            voice.dry = dry || !attenuate;
            refreshGains(s, voice);
            return voice;
        }

        Voice* channelVoice(State& s, int32_t channel)
        {
            for (Voice& voice : s.voices) {
                if (alive(voice) && voice.channel == channel) {
                    return &voice;
                }
            }
            return nullptr;
        }

        int32_t addVoice(State& s, Voice voice, bool noOverride)
        {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            int32_t index = -1;
            float best = voice.priority;
            for (int32_t channel = 0; channel < static_cast<int32_t>(s.channels); channel++) {
                Voice* other = channelVoice(s, channel);
                if (other && voice.actor && voice.slot != SlotNone && other->actor == voice.actor && other->slot == voice.slot) {
                    if (noOverride) {
                        return 0;
                    }
                    index = channel;
                    break;
                }
                float priority = other ? other->priority : 0.0f;
                if (priority <= best) {
                    index = channel;
                    best = priority;
                }
            }
            if (index < 0) {
                return 0;
            }
            Voice* replaced = channelVoice(s, index);
            if (replaced) {
                replaced->stopping = true;
                replaced->channel = -1;
            }
            voice.channel = index;
            voice.id = s.nextId++;
            if (voice.slot == SlotNone) {
                voice.channelId = --s.noneSlotCount * 16;
            }
            int32_t id = voice.id;
            s.voices.push_back(std::move(voice));
            return id;
        }

        void stopVoice(State& s, int32_t id)
        {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            for (Voice& voice : s.voices) {
                if (voice.id == id) {
                    voice.stopping = true;
                }
            }
        }

        bool voiceAlive(State& s, int32_t id)
        {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            for (const Voice& voice : s.voices) {
                if (voice.id == id && alive(voice)) {
                    return true;
                }
            }
            return false;
        }

        bool ambientPlaying(State& s, UObject* actor)
        {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            for (const Voice& voice : s.voices) {
                if (alive(voice) && voice.ambient && voice.actor == actor) {
                    return true;
                }
            }
            return false;
        }

        void updateAmbient(State& s, World* world, bool realtime)
        {
            if (realtime) {
                for (UObject* actor : world->actors) {
                    if (!actor || actor->destroyed || !actor->data) {
                        continue;
                    }
                    USound* sound = cast<USound>(actor::AmbientSound(actor));
                    if (!sound) {
                        continue;
                    }
                    float radius = ambientRadius(actor::SoundRadius(actor));
                    if (squaredDistance(actor::Location(actor), s.viewer) > static_cast<Ext>(radius) * static_cast<Ext>(radius) || ambientPlaying(s, actor)) {
                        continue;
                    }
                    objects::ensureLoaded(sound);
                    std::shared_ptr<const DecodedSound> pcm = decode(sound);
                    if (!pcm) {
                        continue;
                    }
                    float volume = static_cast<float>(static_cast<Ext>(actor::SoundVolume(actor)) * static_cast<Ext>(s.ambientFactor) * static_cast<Ext>(1.0 / 255.0));
                    float pitch = static_cast<float>(actor::SoundPitch(actor)) / 64.0f;
                    Voice voice = makeVoice(s, pcm, sound, actor, SlotAmbient, actor::Location(actor), volume, radius, pitch, true);
                    voice.loop = true;
                    voice.ambient = true;
                    addVoice(s, std::move(voice), false);
                }
            }
            float time = world->levelInfo && world->levelInfo->data ? levelinfo::TimeSeconds(world->levelInfo) : world->timeSeconds;
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            for (Voice& voice : s.voices) {
                if (!alive(voice) || !voice.ambient) {
                    continue;
                }
                UObject* actor = voice.actor;
                if (!actor || actor->destroyed || !actor->data || !realtime) {
                    voice.stopping = true;
                    continue;
                }
                float radius = ambientRadius(actor::SoundRadius(actor));
                if (squaredDistance(actor::Location(actor), s.viewer) > static_cast<Ext>(radius) * static_cast<Ext>(radius) || actor::AmbientSound(actor) != voice.sound) {
                    voice.stopping = true;
                    continue;
                }
                float brightness = static_cast<float>(Ext(2) * (static_cast<Ext>(actor::SoundVolume(actor)) * static_cast<Ext>(s.ambientFactor) * static_cast<Ext>(1.0 / 255.0)));
                voice.volume = ambientBrightness(actor, brightness, time);
                voice.radius = radius;
                voice.pitch = static_cast<float>(actor::SoundPitch(actor)) / 64.0f;
            }
        }

        // How fast the old music fades for each transition:
        // none for MTRAN_Instant and MTRAN_Segue.
        float fadeSpeed(uint8_t transition)
        {
            return transition == TransitionFade ? 1.0f : (transition == TransitionFastFade ? 3.0f : (transition == TransitionSlowFade ? 0.2f : 0.0f));
        }

        void applyMusicRequest(World* world)
        {
            UObject* player = world ? world->player() : nullptr;
            if (!player || player->destroyed || !player->data || !isPlayerPawn(player)) {
                return;
            }
            uint8_t& transition = playerpawn::Transition(player);
            if (transition == 0) {
                // UGalaxyAudioSubsystem::Update: with no
                // change asked for, the player's SongSection follows the order
                // the music has reached, so DeusExPlayer's savedSection resumes
                // ambient music where it was.
                playerpawn::SongSection(player) = static_cast<uint8_t>(state().playingOrder.load());
                return;
            }
            // Galaxy keeps the request in the player while the old music fades:
            // Transition stays set (so SongSection is not written back), each
            // update fades at the rate the current Transition asks for, and the
            // music started at the end is the player's Song and SongSection at
            // that moment. Transition goes back to MTRAN_None once it starts.
            State& s = state();
            UMusic* music = cast<UMusic>(playerpawn::Song(player));
            uint8_t section = playerpawn::SongSection(player);
            bool fading = false;
            {
                std::lock_guard<dx::profile::Mutex> lock(s.mutex);
                if (s.requestPlayer == player && !s.requested) {
                    // The fade ended and the music started in the mixer.
                    s.requestPlayer = nullptr;
                    transition = 0;
                    return;
                }
                fading = s.requested && s.requestObject == music;
                if (fading) {
                    s.requestSection = section;
                    s.fadeSpeed = fadeSpeed(transition);
                    if (s.fadeSpeed == 0) {
                        startRequestedMusic(s);
                    }
                }
            }
            if (!fading) {
                playMusic(music, section, transition);
            }
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            s.requestPlayer = s.requested ? player : nullptr;
            if (!s.requested) {
                transition = 0;
            }
        }

        // The audio settings, all applied while the game runs: read at start and again on each
        // change notice for them (settingsWatch below).
        void readSettings(State& s)
        {
            s.channels = static_cast<size_t>(settings::get(settings::effectsChannels));
            s.useReverb = settings::get(settings::useReverb);
            s.ambientFactor = settings::get(settings::ambientFactor);
            s.dopplerSpeed = settings::get(settings::dopplerSpeed);
            // Applies to the next music played.
            s.digitalMusic = settings::get(settings::useDigitalMusic);
            // A smaller cache gives up sounds as the next one is decoded.
            s.soundLimit = static_cast<uint64_t>(settings::get(settings::soundCacheMegabytes)) * 1024 * 1024;
            // Spatialise positioned sounds with Steam Audio's HRTF instead of Galaxy's stereo pan.
            s.hrtfWanted = settings::get(settings::hrtf);
            // The raytraced mode's settings; asking for it asks for the HRTF too.
            s.raytraced.readSettings();
            s.hrtfWanted = s.hrtfWanted || s.raytraced.wanted();
        }
    }

    size_t DecodedSound::frames() const
    {
        return channels == 0 ? 0 : fixed.size() / channels;
    }

    float DecodedSound::duration() const
    {
        return sampleRate == 0 ? 0.0f : static_cast<float>(static_cast<double>(frames()) / static_cast<double>(sampleRate));
    }

    char speechPhoneme(const DecodedSound& pcm, size_t position, size_t end)
    {
        std::array<float, 1024> re {};
        std::array<float, 1024> im {};
        size_t frames = pcm.frames();
        for (size_t i = 0; i < 1024; i++) {
            float clamped = position + i < std::min(frames, end) ? static_cast<float>(pcm.fixed[(position + i) * pcm.channels]) : 0.0f;
            re[i] = clamped * (1.0f / 32768.0f) * (static_cast<float>(i) / 1024.0f);
        }
        fft(re, im);
        float peak = -99999.0f;
        size_t bin = 0;
        for (size_t k = 0; k < 512; k++) {
            float value = std::fabs(re[k]);
            if (peak < value) {
                peak = value;
                bin = k;
            }
        }
        if (peak <= 5.0f) {
            return 'X';
        }
        float frequency = static_cast<float>(bin) * static_cast<float>(pcm.sampleRate) * (1.0f / 1024.0f);
        if (frequency > 2000) {
            return 'T';
        }
        if (frequency > 1500) {
            return 'F';
        }
        if (frequency > 600) {
            return 'A';
        }
        if (frequency > 400) {
            return 'O';
        }
        if (frequency > 250) {
            return 'U';
        }
        return 'E';
    }

    float ambientRadius(uint8_t soundRadius)
    {
        return 25.0f * (static_cast<float>(soundRadius) + 1.0f);
    }

    std::shared_ptr<const DecodedSound> decodeBytes(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty()) {
            return nullptr;
        }
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder decoder;
        if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS) {
            return nullptr;
        }
        // Uninitialised however this returns.
        std::unique_ptr<ma_decoder, decltype(&ma_decoder_uninit)> owner(&decoder, &ma_decoder_uninit);
        auto result = std::make_shared<DecodedSound>();
        result->sampleRate = decoder.outputSampleRate;
        result->channels = decoder.outputChannels;
        if (result->channels == 0 || result->sampleRate == 0) {
            return nullptr;
        }
        // Only the 16-bit samples are kept: the mixer and the lip-sync analysis both work from
        // them, so each chunk is converted as it is read.
        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0) {
            result->fixed.reserve(static_cast<size_t>(length) * result->channels);
        }
        std::vector<float> chunk(4096 * static_cast<size_t>(result->channels));
        while (true) {
            ma_uint64 read = 0;
            ma_result status = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read);
            size_t count = static_cast<size_t>(read) * result->channels;
            for (size_t i = 0; i < count; i++) {
                result->fixed.push_back(static_cast<int16_t>(std::clamp(std::nearbyint(chunk[i] * 32768.0f), -32768.0f, 32767.0f)));
            }
            if (status != MA_SUCCESS || read == 0) {
                break;
            }
        }
        // The cache counts what the samples hold, so no growth slack is kept.
        result->fixed.shrink_to_fit();
        readLoop(bytes, *result);
        result->compressed = bytes.size() < 4 || std::memcmp(bytes.data(), "RIFF", 4) != 0;
        return result;
    }

    namespace {
        uint64_t decodedBytes(const DecodedSound& pcm)
        {
            return pcm.fixed.capacity() * sizeof(int16_t);
        }

        // Drops the least recently used sounds until the cache fits its limit. A voice still
        // playing a dropped sound keeps its own reference, so nothing playing is cut off.
        void trimSounds(State& s, const USound* keep)
        {
            while (s.soundLimit != 0 && s.soundBytes > s.soundLimit) {
                auto oldest = s.sounds.end();
                for (auto it = s.sounds.begin(); it != s.sounds.end(); ++it) {
                    if (it->first != keep && it->second.pcm && (oldest == s.sounds.end() || it->second.lastUse < oldest->second.lastUse)) {
                        oldest = it;
                    }
                }
                if (oldest == s.sounds.end()) {
                    return;
                }
                s.soundBytes -= decodedBytes(*oldest->second.pcm);
                s.sounds.erase(oldest);
            }
        }
    }

    void setSoundCacheLimit(uint64_t bytes)
    {
        State& s = state();
        s.soundLimit = bytes;
        trimSounds(s, nullptr);
    }

    std::shared_ptr<const DecodedSound> decode(USound* sound)
    {
        if (!sound) {
            return nullptr;
        }
        State& s = state();
        auto it = s.sounds.find(sound);
        if (it != s.sounds.end()) {
            it->second.lastUse = ++s.soundUse;
            return it->second.pcm;
        }
        std::shared_ptr<const DecodedSound> result = decodeBytes(sound->bytes);
        s.sounds.emplace(sound, State::CachedSound { result, ++s.soundUse });
        if (result) {
            sound->duration = result->duration();
            s.soundBytes += decodedBytes(*result);
            trimSounds(s, sound);
        }
        return result;
    }

    std::shared_ptr<const ItModule> loadModule(UMusic* music)
    {
        if (!music) {
            return nullptr;
        }
        State& s = state();
        auto it = s.modules.find(music);
        if (it != s.modules.end()) {
            return it->second;
        }
        auto module = std::make_shared<ItModule>();
        std::shared_ptr<const ItModule> result;
        if (module->load(music->bytes.data(), music->bytes.size())) {
            result = module;
        }
        s.modules.emplace(music, result);
        return result;
    }

    Placement place(const Vector& listenerLocation, const Rotator& listenerRotation, const Vector& source, float radius)
    {
        Coords axes = viewAxes(listenerRotation);
        Ext dx = static_cast<Ext>(source.x) - static_cast<Ext>(listenerLocation.x);
        Ext dy = static_cast<Ext>(source.y) - static_cast<Ext>(listenerLocation.y);
        Ext dz = static_cast<Ext>(source.z) - static_cast<Ext>(listenerLocation.z);
        auto project = [&](const Vector& axis) {
            return static_cast<float>(dx * static_cast<Ext>(axis.x) + dy * static_cast<Ext>(axis.y) + dz * static_cast<Ext>(axis.z));
        };
        float right = project(axes.yAxis);
        float up = project(axes.zAxis);
        float forward = project(axes.xAxis);
        Ext angle = static_cast<Ext>(static_cast<float>(std::atan2(static_cast<double>(right), static_cast<double>(std::fabs(forward)))));
        // a correctly rounded 1/10 is the same value as the 0.1L literal
        float center = static_cast<float>(Ext(1) / Ext(10) * static_cast<Ext>(radius));
        Ext sum = static_cast<Ext>(up) * static_cast<Ext>(up) + static_cast<Ext>(right) * static_cast<Ext>(right) + static_cast<Ext>(forward) * static_cast<Ext>(forward);
        float sizeSquared = static_cast<float>(sum);
        Ext size = static_cast<Ext>(std::sqrt(static_cast<double>(sum)));
        Placement placement;
        if (static_cast<Ext>(center) * static_cast<Ext>(center) > static_cast<Ext>(sizeSquared)) {
            angle = size / static_cast<Ext>(center) * angle;
            placement.spread = static_cast<float>(size / static_cast<Ext>(center));
        }
        placement.offset = { forward, right, up };
        placement.distance = static_cast<float>(size);
        Ext pan = angle * static_cast<Ext>(28671.125f) * static_cast<Ext>(0.3183098861837907) + Ext(16383);
        placement.pan = std::clamp(static_cast<int32_t>(pan), 0, MaxPan);
        placement.attenuation = static_cast<float>(std::clamp(Ext(1) - size / static_cast<Ext>(radius), Ext(0), Ext(1)));
        return placement;
    }

    int32_t masterVolume(uint8_t sound, uint8_t speech)
    {
        float soundLevel = static_cast<float>(static_cast<Ext>(sound) * static_cast<Ext>(1.0 / 255.0));
        Ext speechLevel = static_cast<Ext>(speech) * static_cast<Ext>(1.0 / 255.0);
        Ext loudest = static_cast<Ext>(soundLevel) < speechLevel ? speechLevel : static_cast<Ext>(soundLevel);
        return static_cast<int32_t>(loudest * Ext(127));
    }

    int32_t voiceVolume(float attenuation, float obstruction, uint8_t sound, uint8_t speech, bool speechSlot, float volume)
    {
        Ext clear = std::max(Ext(1) - static_cast<Ext>(obstruction + obstruction), static_cast<Ext>(0.33));
        Ext level = clear * static_cast<Ext>(attenuation) * categoryScale(sound, speech, speechSlot) * static_cast<Ext>(volume) * Ext(32767);
        int32_t value = std::clamp(static_cast<int32_t>(level), 0, 32767);
        return std::max(value, 128);
    }

    Gains channelGains(int32_t volume, int32_t pan, int32_t master, int32_t send)
    {
        std::array<int32_t, 2> levels = channelLevels(volume, pan, master, send);
        int32_t left = levels[0];
        int32_t right = levels[1];
        const float scale = 32767.0f / 1073741824.0f;
        Gains gains;
        gains.left = static_cast<float>(left) * scale;
        gains.right = static_cast<float>(right) * scale;
        return gains;
    }

    float dopplerShift(const Vector& velocity, const Vector& source, const Vector& listener, float speed)
    {
        Vector direction = (source - listener).normal();
        Ext along = static_cast<Ext>(direction.z) * static_cast<Ext>(velocity.z) + static_cast<Ext>(direction.y) * static_cast<Ext>(velocity.y) + static_cast<Ext>(direction.x) * static_cast<Ext>(velocity.x);
        return static_cast<float>(std::clamp(Ext(1) - along / static_cast<Ext>(speed), Ext(0.5), Ext(2)));
    }

    uint32_t voiceStep(uint32_t sampleRate, float pitch, float doppler, uint32_t outputRate)
    {
        uint32_t frequency = static_cast<uint32_t>(static_cast<int32_t>(static_cast<Ext>(sampleRate) * static_cast<Ext>(pitch) * static_cast<Ext>(doppler)));
        uint64_t wide = frequency;
        uint64_t rate = outputRate;
        return static_cast<uint32_t>(((wide / rate) << 16) + (((wide % rate) << 16) / rate));
    }

    ReverbSettings zoneReverb(UObject* zone)
    {
        ReverbSettings settings;
        if (!zone || !zone->data || !zoneinfo::bReverbZone(zone)) {
            return settings;
        }
        settings.volume = static_cast<float>(static_cast<Ext>(zoneinfo::MasterGain(zone)) * static_cast<Ext>(1.0 / 255.0));
        settings.cutoff = static_cast<float>(std::clamp(zoneinfo::CutoffHz(zone), 0, 44100));
        for (int i = 0; i < 6; i++) {
            Ext delay = static_cast<Ext>(zoneinfo::Delay(zone, i)) * static_cast<Ext>(0.002f);
            Ext gain = static_cast<Ext>(zoneinfo::Gain(zone, i)) * static_cast<Ext>(1.0f / 255.0f);
            settings.delay[static_cast<size_t>(i)] = static_cast<float>(std::clamp(delay, static_cast<Ext>(0.001f), static_cast<Ext>(0.34f)));
            settings.gain[static_cast<size_t>(i)] = static_cast<float>(std::clamp(gain, static_cast<Ext>(0.001f), static_cast<Ext>(0.999f)));
        }
        return settings;
    }

    void Reverb::configure(const ReverbSettings& settings, uint32_t rate)
    {
        enabled = settings.volume != 0;
        if (!enabled) {
            lines.clear();
            return;
        }
        Ext bend = Ext(1) - cos(static_cast<Ext>(settings.cutoff) * static_cast<Ext>(6.282f) / static_cast<Ext>(rate));
        smoothing = static_cast<float>(sqrt((bend + Ext(2)) * bend) - bend);
        keep = static_cast<float>(Ext(1) - static_cast<Ext>(smoothing));
        volume = settings.volume;
        for (size_t i = 0; i < 6; i++) {
            float delay = settings.delay[i] == 0.0f ? 0.001f : settings.delay[i];
            Ext amount = Ext(1) - static_cast<Ext>(settings.gain[i]);
            if (amount == Ext(1)) {
                amount = static_cast<Ext>(0.999f);
            }
            delays[i] = static_cast<size_t>(static_cast<Ext>(rate) * static_cast<Ext>(delay));
            direct[i] = static_cast<float>(-amount);
            feedback[i] = static_cast<float>(amount);
            through[i] = static_cast<float>(Ext(1) - amount * amount);
        }
        lines.assign(3 * ReverbFrames * 2, 0.0f);
        lowpass.fill(0.0f);
        last.fill(0.0f);
        position = 0;
    }

    void Reverb::process(const float* send, float* stereo, size_t frames)
    {
        if (!enabled) {
            return;
        }
        const size_t mask = ReverbFrames - 1;
        for (size_t n = 0; n < frames; n++) {
            std::array<float, 2> wave = { (send[n * 2] + last[1]) * 0.5f, (send[n * 2 + 1] + last[0]) * 0.5f };
            for (size_t stage = 0; stage < 3; stage++) {
                float* line = lines.data() + stage * ReverbFrames * 2;
                for (size_t channel = 0; channel < 2; channel++) {
                    size_t tap = stage * 2 + channel;
                    size_t read = (position + ReverbFrames - delays[tap]) & mask;
                    float delayed = line[read * 2 + channel];
                    float out = wave[channel] * direct[tap];
                    lowpass[tap] = delayed * smoothing + lowpass[tap] * keep;
                    line[position * 2 + channel] = wave[channel] + lowpass[tap] * feedback[tap];
                    wave[channel] = out + delayed * through[tap];
                }
            }
            last = wave;
            stereo[n * 2] += wave[0] * volume;
            stereo[n * 2 + 1] += wave[1] * volume;
            position = (position + 1) & mask;
        }
    }

    bool Reverb::active() const
    {
        return enabled;
    }

    bool initialize(bool nullDevice)
    {
        State& s = state();
        if (s.active) {
            return true;
        }
        readSettings(s);
        if (!output::open(s.rate, deviceMix, &s, nullDevice)) {
            return false;
        }
        {
            // The device may already be pulling the mix.
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            startHrtf(s);
        }
        if (!output::start()) {
            s.raytraced.dropSimulation();
            s.binaural.reset();
            output::close();
            return false;
        }
        s.active = true;
        s.offline = false;
        s.updated = false;
        return true;
    }

    bool initializeOffline(uint32_t sampleRate)
    {
        State& s = state();
        if (s.active) {
            shutdown();
        }
        readSettings(s);
        s.sounds.clear();
        s.soundBytes = 0;
        s.modules.clear();
        s.rate = sampleRate;
        startHrtf(s);
        s.active = true;
        s.offline = true;
        s.updated = false;
        return true;
    }

    void shutdown()
    {
        State& s = state();
        if (!s.active) {
            return;
        }
        if (!s.offline) {
            output::close();
        }
        s.active = false;
        s.offline = false;
        s.updated = false;
        s.raytraced.shutdown();
        s.binaural.reset();
        s.hrtfChannels.clear();
        s.blockRead = steam::BlockFrames;
        s.debugVoices = false;
        s.cost = {};
        s.costSeconds = 0;
        s.costFrames = 0;
        s.voices.clear();
        s.reverb.configure({}, s.rate);
        s.reverbSettings = {};
        s.music.reset();
        s.pendingMusic.reset();
        s.musicObject = nullptr;
        s.musicSection = -1;
        s.musicPlaying = false;
        s.musicFade = 1;
        s.fadeSpeed = 0;
        s.requested = false;
        s.requestObject = nullptr;
        s.requestSection = -1;
        s.listener = {};
        s.listenerRotation = {};
        s.mixRotation = {};
        s.viewer = {};
        s.ear = {};
        s.sounds.clear();
        s.soundBytes = 0;
        s.modules.clear();
    }

    void render(float* stereo, size_t frames)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        mixOutput(s, stereo, frames);
    }

    void update(World* world, const Vector& listenerLocation, const Rotator& listenerRotation, float deltaSeconds)
    {
        DX_ZONE_NAMED("Audio update");
        State& s = state();
        if (!s.active) {
            return;
        }
        float delta = std::clamp(deltaSeconds, 0.0f, 1.0f);
        s.updated = true;
        s.listener = listenerLocation;
        s.listenerRotation = listenerRotation;
        s.viewer = listenerLocation;
        s.ear = listenerLocation;
        {
            DX_ZONE_NAMED("Audio music request");
            applyMusicRequest(world);
            s.raytraced.followLevel(world);
        }
        UObject* zone = nullptr;
        std::unordered_set<UObject*> present;
        if (world) {
            UObject* player = world->player();
            if (player && !player->destroyed && player->data) {
                UObject* view = isPlayerPawn(player) ? playerpawn::ViewTarget(player) : nullptr;
                if (!view || view->destroyed || !view->data) {
                    view = player;
                }
                s.viewer = actor::Location(view);
                s.ear = actor::Location(player);
                if (isPawn(player)) {
                    s.ear.z = pawn::EyeHeight(player) + s.ear.z;
                }
                zone = region(view).zone;
            }
            bool realtime = !world->levelInfo || !world->levelInfo->data || levelinfo::Pauser(world->levelInfo).empty();
            DX_ZONE_NAMED("Audio ambient sounds");
            updateAmbient(s, world, realtime);
        } else {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            for (Voice& voice : s.voices) {
                if (voice.ambient) {
                    voice.stopping = true;
                }
            }
        }
        ReverbSettings settings;
        {
            DX_ZONE_NAMED("Audio zone reverb");
            settings = s.useReverb ? zoneReverb(zone) : ReverbSettings {};
        }

        // Galaxy's line check from the ear to each sound on a live actor is made with the mixer
        // unlocked, so the mixer never waits on the world's collision. Which voices and where
        // they are is read under the lock, and the results go to the same voices in this
        // update, as when the checks were made under it. In between only the mixer can run, and
        // all it can do to a voice is end it or drop it once ended: one that has is passed over
        // below, as it would have been had the mixer run just before this update. Nothing else
        // starts or stops a voice meanwhile, as that is done on this thread.
        struct LineCheck {
            int32_t id = 0;
            Vector location;
            bool clear = false;
        };

        if (world) {
            // Only the voices' actors are looked for in the world's actor list, rather than making
            // a set of every actor, after the ambient sounds above have started theirs; until the
            // gather below only the mixer runs, and it only ends voices.
            DX_ZONE_NAMED("Audio voice actor lookup");
            std::vector<UObject*> wanted;
            {
                DX_LOCK_GUARD(copy, s.mutex, "Audio update copies voice actors");
                for (const Voice& voice : s.voices) {
                    if (voice.actor) {
                        wanted.push_back(voice.actor);
                    }
                }
            }
            std::sort(wanted.begin(), wanted.end());
            wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
            if (!wanted.empty()) {
                for (UObject* actor : world->actors) {
                    if (std::binary_search(wanted.begin(), wanted.end(), actor)) {
                        present.insert(actor);
                    }
                }
            }
        }
        std::vector<LineCheck> checks;
        {
            DX_LOCK_GUARD(gather, s.mutex, "Audio update waits for the audio state");
            DX_ZONE_NAMED("Audio update gathers voices (locked)");
            if (!s.raytraced.on()) {
                for (const Voice& voice : s.voices) {
                    bool live = voice.actor && present.count(voice.actor) && !voice.actor->destroyed && voice.actor->data;
                    if (alive(voice) && live && voice.slot != SlotTalk) {
                        checks.push_back({ voice.id, actor::Location(voice.actor) });
                    }
                }
            }
        }
        {
            DX_ZONE_NAMED("Sound line checks");
            for (LineCheck& check : checks) {
                check.clear = world->fastLineCheck(s.ear, check.location);
            }
        }
        size_t nextCheck = 0;
        DX_LOCK_GUARD(lock, s.mutex, "Audio update waits for the audio state");
        s.mixRotation = s.listenerRotation;
        DX_ZONE_NAMED("Audio update applies to voices (locked)");
        for (Voice& voice : s.voices) {
            if (!alive(voice)) {
                continue;
            }
            bool live = voice.actor && present.count(voice.actor) && !voice.actor->destroyed && voice.actor->data;
            if (live) {
                voice.location = actor::Location(voice.actor);
            }
            voice.elapsed += delta;
            if (s.raytraced.on()) {
                // The raytraced mode's occlusion and transmission replace Galaxy's line check
                // and its floor of a third.
                voice.obstruction = 0;
            } else {
                bool obstructed = false;
                if (live && voice.slot != SlotTalk) {
                    // The voices keep their order, so each one's check comes after the last one
                    // found. A voice with none (there should be none) is checked here.
                    auto check = std::find_if(checks.begin() + static_cast<std::ptrdiff_t>(nextCheck), checks.end(), [&](const LineCheck& made) {
                        return made.id == voice.id;
                    });
                    if (check != checks.end()) {
                        obstructed = !check->clear;
                        nextCheck = static_cast<size_t>(check - checks.begin()) + 1;
                    } else {
                        obstructed = !world->fastLineCheck(s.ear, voice.location);
                    }
                }
                voice.obstruction = obstructed ? std::min(voice.obstruction + delta, 0.5f) : std::max(voice.obstruction - delta, 0.0f);
            }
            voice.doppler = live && voice.ambient ? dopplerShift(actor::Velocity(voice.actor), voice.location, s.viewer, s.dopplerSpeed) : 1.0f;
            refreshGains(s, voice);
            voice.pending = false;
            static const Name namePawn("Pawn");
            if (live && voice.slot == SlotTalk && voice.pcm && voice.elapsed >= voice.nextPhonemeTime && voice.actor->isA(namePawn) && pawn::bIsSpeaking(voice.actor)) {
                size_t start = voice.position;
                size_t end = voice.position - voice.position % 32767 + 32767;
                if (!voice.pcm->compressed) {
                    Ext rate = static_cast<Ext>(voice.pcm->sampleRate);
                    Ext played = static_cast<Ext>(static_cast<int64_t>(rate * static_cast<Ext>(voice.elapsed)));
                    start = static_cast<size_t>(played + rate * static_cast<Ext>(0.1));
                    end = voice.pcm->frames();
                }
                pawn::nextPhoneme(voice.actor) = std::string(1, speechPhoneme(*voice.pcm, start, end));
                voice.nextPhonemeTime += 0.1f;
            }
        }
        if (s.raytraced.on()) {
            // The positioned voices the raytraced mode simulates, heard from the ear, where
            // Galaxy's line check starts.
            std::vector<acoustics::SourceInput> sources;
            for (const Voice& voice : s.voices) {
                if (alive(voice) && voice.attenuate && voice.slot != SlotTalk) {
                    sources.push_back({ voice.id, voice.location, voice.priority });
                }
            }
            s.raytraced.hear(s.ear, s.listenerRotation, std::move(sources));
        }
        if (settings != s.reverbSettings) {
            s.reverb.configure(settings, s.rate);
            s.reverbSettings = settings;
        }
    }

    // Investigation: mixes seconds of sound offline, on the calling thread, through the stereo
    // mixer (mixAll) with a fresh state of its own: voices looping a 22 kHz mono sound of noise,
    // spread across the pan, at the device's rate, in blocks of block frames, with the zone reverb
    // the game has now. Then the level's music on its own, if any is playing. Reports the time
    // per second of sound mixed. The live mixer is not touched.
    std::string benchmarkMix(int voiceCount, double seconds, size_t block)
    {
        State& live = state();
        uint32_t rate = live.active ? live.rate : 48000;
        ReverbSettings reverb;
        UMusic* musicObject = nullptr;
        size_t liveVoices = 0;
        {
            std::lock_guard<dx::profile::Mutex> lock(live.mutex);
            reverb = live.reverbSettings;
            musicObject = live.musicObject;
            liveVoices = live.voices.size();
        }
        auto pcm = std::make_shared<DecodedSound>();
        pcm->sampleRate = 22050;
        pcm->channels = 1;
        pcm->looped = true;
        pcm->loopStart = 0;
        pcm->loopEnd = 22050 * 2;
        pcm->fixed.resize(pcm->loopEnd);
        uint32_t noise = 12345;
        for (int16_t& sample : pcm->fixed) {
            noise = noise * 1103515245u + 12345u;
            sample = static_cast<int16_t>(static_cast<int32_t>(noise >> 16) - 32768);
        }
        auto bench = std::make_unique<State>();
        bench->rate = rate;
        bench->reverb.configure(reverb, rate);
        bench->reverbSettings = reverb;
        for (int i = 0; i < voiceCount; i++) {
            Voice voice;
            voice.pcm = pcm;
            voice.loop = true;
            voice.pending = false;
            voice.position = static_cast<size_t>(i) * 977u % pcm->loopEnd;
            voice.dryLeft = 1 << 28;
            voice.dryRight = 1 << 27;
            voice.wetLeft = 1 << 26;
            voice.wetRight = 1 << 26;
            voice.step = voiceStep(pcm->sampleRate, 0.8f + 0.4f * static_cast<float>(i) / static_cast<float>(std::max(voiceCount, 1)), 1.0f, rate);
            bench->voices.push_back(std::move(voice));
        }
        size_t total = static_cast<size_t>(seconds * static_cast<double>(rate));
        std::vector<float> out(block * 2);
        size_t calls = 0;
        auto start = std::chrono::steady_clock::now();
        for (size_t done = 0; done < total; done += block) {
            mixAll(*bench, out.data(), block);
            calls++;
        }
        double voiceSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        double musicSeconds = -1;
        if (musicObject) {
            std::shared_ptr<const ItModule> module = loadModule(musicObject);
            if (module) {
                ModulePlayer player(module, rate);
                player.restart(0);
                start = std::chrono::steady_clock::now();
                for (size_t done = 0; done < total; done += block) {
                    player.render(out.data(), block);
                }
                musicSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            }
        }
        double mixed = static_cast<double>(calls * block) / static_cast<double>(rate);
        uint64_t deviceCalls = live.deviceCalls.load();
        uint64_t deviceFrames = live.deviceFrames.load();
        int64_t deviceWait = live.deviceWaitNs.load();
        MixCost cost = mixCost();
        char line[512];
        std::snprintf(line, sizeof(line), "mixbench voices=%d rate=%u block=%zu reverb=%d seconds=%.2f mix_ms_per_s=%.3f us_per_call=%.2f music=%s music_ms_per_s=%.3f | live: device_calls=%llu frames_per_call=%.1f lock_wait_us_per_call=%.2f live_stereo_ms_per_s=%.3f live_voices=%zu", voiceCount, rate, block, bench->reverb.active() ? 1 : 0, mixed, voiceSeconds * 1000.0 / mixed, voiceSeconds * 1e6 / static_cast<double>(calls), musicObject ? musicObject->name.c_str() : "None", musicSeconds < 0 ? -1.0 : musicSeconds * 1000.0 / mixed, static_cast<unsigned long long>(deviceCalls), deviceCalls ? static_cast<double>(deviceFrames) / static_cast<double>(deviceCalls) : 0.0, deviceCalls ? static_cast<double>(deviceWait) / 1e3 / static_cast<double>(deviceCalls) : 0.0, static_cast<double>(cost.stereo), liveVoices);
        return line;
    }

    int32_t playSound(USound* sound, UObject* actor, int32_t slot, const Vector& location, float volume, float radius, float pitch, bool attenuate, bool noOverride, int32_t channelId, bool dry)
    {
        State& s = state();
        if (!s.active || !sound) {
            return 0;
        }
        std::shared_ptr<const DecodedSound> pcm = decode(sound);
        if (!pcm) {
            return 0;
        }
        Voice voice = makeVoice(s, pcm, sound, actor, slot, location, volume, radius, pitch, attenuate, dry);
        voice.channelId = channelId;
        voice.pending = s.updated || voice.actor != nullptr || voice.attenuate;
        return addVoice(s, std::move(voice), noOverride);
    }

    // UGalaxyAudioSubsystem::StopSoundId: stops the
    // first channel whose id matches, ignoring the bNoOverride bit. So it stops
    // whatever that actor's slot is playing now, even a later sound.
    void stopSoundId(int32_t id)
    {
        State& s = state();
        if (!s.active) {
            return;
        }
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (int32_t channel = 0; channel < static_cast<int32_t>(s.channels); channel++) {
            Voice* voice = channelVoice(s, channel);
            if (voice && ((voice->channelId ^ id) & ~1) == 0) {
                voice->stopping = true;
                voice->channel = -1;
                return;
            }
        }
    }

    void heldObjects(std::vector<UObject*>& objects)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (const Voice& voice : s.voices) {
            if (voice.sound) {
                objects.push_back(const_cast<USound*>(voice.sound));
            }
        }
        objects.push_back(s.musicObject);
        objects.push_back(s.requestObject);
    }

    void objectsReleased(const std::unordered_set<const UObject*>& objects)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (Voice& voice : s.voices) {
            if (voice.actor && objects.count(voice.actor)) {
                if (voice.ambient) {
                    voice.stopping = true;
                }
                voice.actor = nullptr;
            }
        }
        if (s.requestPlayer && objects.count(s.requestPlayer)) {
            s.requestPlayer = nullptr;
        }
        for (auto it = s.sounds.begin(); it != s.sounds.end();) {
            if (objects.count(it->first)) {
                if (it->second.pcm) {
                    s.soundBytes -= decodedBytes(*it->second.pcm);
                }
                it = s.sounds.erase(it);
            } else {
                ++it;
            }
        }
        std::erase_if(s.modules, [&](const auto& entry) { return objects.count(entry.first) != 0; });
    }

    namespace {
        // The sounds and music the mixer is playing are kept. The actors it follows and the
        // decoded copies it caches are dropped when those objects go.
        //
        // That is every object pointer in the audio library: a voice's sound and actor, the
        // music playing and the one requested, the player that requested it, and the sound and
        // module caches keyed on their objects. The mixer thread only reads those fields under
        // the lock, and its module players and decoded samples are copies that point at nothing.
        // The caches are filled on the game thread, which is also where collections run.
        struct MixerReferences final : gc::Holder {
            MixerReferences()
            {
                gc::libraryReported("audio");
            }

            void addReferences(ReferenceVisitor& references) override
            {
                std::vector<UObject*> held;
                heldObjects(held);
                for (UObject* object : held) {
                    references(object);
                }
            }

            void objectsReleased(const gc::ReleasedSet& released) override
            {
                audio::objectsReleased(released);
            }
        };

        MixerReferences mixerReferences;

        // The raytraced mode's scene is of a level; it hears when that level is going.
        levelevents::Subscription levelWatch = levelevents::subscribe(levelevents::Stage::Audio, {
                                                                                                     .loaded = nullptr,
                                                                                                     .unloading = [](World& world) { raytracedMode().levelUnloading(world.level); },
                                                                                                 });
    }

    void actorDestroyed(UObject* actor)
    {
        State& s = state();
        if (!s.active || !actor) {
            return;
        }
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (Voice& voice : s.voices) {
            if (voice.actor != actor) {
                continue;
            }
            if (voice.ambient) {
                voice.stopping = true;
            }
            voice.actor = nullptr;
        }
    }

    void stopSound(int32_t id)
    {
        State& s = state();
        if (!s.active || id == 0) {
            return;
        }
        stopVoice(s, id);
    }

    bool isPlaying(int32_t id)
    {
        State& s = state();
        if (!s.active || id == 0) {
            return false;
        }
        return voiceAlive(s, id);
    }

    float duration(USound* sound)
    {
        std::shared_ptr<const DecodedSound> pcm = decode(sound);
        return pcm ? pcm->duration() : 0.0f;
    }

    CacheStats cacheStats()
    {
        CacheStats stats;
        for (const auto& [sound, cached] : state().sounds) {
            stats.sounds++;
            if (cached.pcm) {
                stats.sourceBytes += sound->bytes.size();
                stats.decodedBytes += decodedBytes(*cached.pcm);
            }
        }
        stats.limitBytes = state().soundLimit;
        stats.modules = state().modules.size();
        return stats;
    }

    void playMusic(UMusic* music, uint8_t section, uint8_t transition)
    {
        State& s = state();
        if (!s.active) {
            return;
        }
        std::unique_ptr<ModulePlayer> player;
        float speed = fadeSpeed(transition);
        std::unique_lock<dx::profile::Mutex> lock(s.mutex);
        if (music && music != s.musicObject && s.digitalMusic) {
            lock.unlock();
            objects::ensureLoaded(music);
            std::shared_ptr<const ItModule> module = loadModule(music);
            if (module) {
                player = std::make_unique<ModulePlayer>(module, s.rate);
            }
            lock.lock();
        }
        s.requestObject = music;
        s.requestSection = section;
        s.pendingMusic = std::move(player);
        s.requested = true;
        if (speed == 0 || !s.music || !s.musicPlaying) {
            startRequestedMusic(s);
            return;
        }
        s.fadeSpeed = speed;
    }

    void applyMenuSettings()
    {
        State& s = state();
        bool switchHrtf = false;
        {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            readSettings(s);
            // Sounds on channels that no longer exist stop, as they would never have started. The
            // next update reconfigures the reverb if UseReverb changed.
            for (Voice& voice : s.voices) {
                if (voice.channel >= static_cast<int32_t>(s.channels)) {
                    voice.stopping = true;
                    voice.channel = -1;
                }
            }
            switchHrtf = s.active && s.hrtfWanted != (s.binaural != nullptr);
        }
        if (!s.active) {
            return;
        }
        // Steam Audio is loaded and its effects made, or freed, outside the lock so the mixer
        // keeps playing meanwhile. Only this thread switches the modes. The raytraced mode goes
        // off before the HRTF it rests on, and on after it.
        if (s.raytraced.on() && (!s.raytraced.wanted() || !s.hrtfWanted)) {
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            switchRaytracing(s, false);
        }
        if (switchHrtf) {
            std::unique_ptr<steam::Binaural> binaural = s.hrtfWanted ? makeBinaural(s) : nullptr;
            if (!s.hrtfWanted || binaural) {
                std::lock_guard<dx::profile::Mutex> lock(s.mutex);
                installBinaural(s, binaural);
            }
        }
        if (!s.raytraced.wanted() || !s.binaural || s.raytraced.on()) {
            return;
        }
        if (!s.raytraced.hasSimulation()) {
            std::unique_ptr<acoustics::Simulation> simulation = s.raytraced.makeSimulation(s.rate, MaxChannels);
            if (!simulation) {
                return;
            }
            std::lock_guard<dx::profile::Mutex> lock(s.mutex);
            s.raytraced.install(std::move(simulation));
        }
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        switchRaytracing(s, true);
    }

    void applySettings()
    {
        setVolumes(settings::get(settings::soundVolume) / 255.0f, settings::get(settings::musicVolume) / 255.0f, settings::get(settings::speechVolume) / 255.0f);
        applyMenuSettings();
    }

    namespace {
        // Every audio setting is in the audio device's section of the system ini, which
        // Engine.Engine's AudioDevice names.
        Subscription settingsWatch = config::onSettingChanged([](const config::SettingChange& change) {
            if (&change.ini != &config::system()) {
                return;
            }
            const settings::Setting& device = settings::audioDevice.setting;
            bool named = iequals(change.section, settings::section(device.section)) && iequals(change.key, device.key);
            if (named || iequals(change.section, settings::section(settings::audio))) {
                applySettings();
            }
        });
    }

    void setVolumes(float sound, float music, float speech)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        s.soundVolume = std::clamp(sound, 0.0f, 1.0f);
        s.musicVolume = std::clamp(music, 0.0f, 1.0f);
        s.speechVolume = std::clamp(speech, 0.0f, 1.0f);
    }

    Volumes volumes()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        Volumes result;
        result.sound = s.soundVolume;
        result.music = s.musicVolume;
        result.speech = s.speechVolume;
        return result;
    }

    RaytracedMode& raytracedMode()
    {
        return state().raytraced;
    }

    dx::profile::Mutex& mixerMutex()
    {
        return state().mutex;
    }

    bool hrtfActive()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        return s.binaural != nullptr;
    }

    MusicStatus musicStatus()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        MusicStatus status;
        status.music = s.musicObject;
        status.section = s.musicSection;
        if (s.music && s.musicPlaying) {
            status.order = s.music->order();
            status.row = s.music->row();
        }
        status.fade = s.music && s.musicPlaying ? s.musicFade : 0.0f;
        status.pending = s.requested;
        return status;
    }

    std::vector<VoiceStatus> voices()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        std::vector<VoiceStatus> result;
        for (const Voice& voice : s.voices) {
            if (!alive(voice)) {
                continue;
            }
            VoiceStatus status;
            status.id = voice.id;
            status.sound = voice.sound;
            status.actor = voice.actor;
            status.slot = voice.slot;
            status.gainLeft = voice.gainLeft;
            status.gainRight = voice.gainRight;
            status.priority = voice.priority;
            status.ambient = voice.ambient;
            status.channel = voice.channel;
            status.volume = voice.level;
            status.pan = voice.pan;
            status.obstruction = voice.obstruction;
            status.step = static_cast<double>(voice.step) / 65536.0;
            status.attenuated = voice.attenuate;
            status.hrtf = voice.attenuate && s.binaural != nullptr;
            status.offset = voice.offset;
            status.distance = voice.distance;
            status.spread = voice.hrtfSpread;
            status.muted = voice.debugMute;
            status.soloed = voice.debugSolo;
            status.location = voice.location;
            status.position = voice.position;
            status.frames = voice.pcm ? voice.pcm->frames() : 0;
            s.raytraced.report(voice.raytraced, voice.attenuate && voice.slot != SlotTalk, status);
            result.push_back(status);
        }
        return result;
    }

    void setVoiceMuted(int32_t id, bool muted)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (Voice& voice : s.voices) {
            if (voice.id == id) {
                voice.debugMute = muted;
                s.debugVoices = true;
            }
        }
    }

    void setVoiceSoloed(int32_t id, bool soloed)
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        for (Voice& voice : s.voices) {
            if (voice.id == id) {
                voice.debugSolo = soloed;
                s.debugVoices = true;
            }
        }
    }

    MixCost mixCost()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        return s.cost;
    }

    ReverbSettings reverbSettings()
    {
        State& s = state();
        std::lock_guard<dx::profile::Mutex> lock(s.mutex);
        return s.reverbSettings;
    }

    void setLightRandom(float (*random)(int32_t index))
    {
        lightRandomSource = random;
    }
}
