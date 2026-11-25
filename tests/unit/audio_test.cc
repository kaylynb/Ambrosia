#include "audio/audio.h"
#include "test_paths.h"
#include "audio/module.h"
#include "audio/raytraced.h"
#include "core/config.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/console.h"
#include "engine/props_engine.h"
#include "core/package.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
    std::vector<uint8_t> resourceBytes(const std::string& path, const char* className, const char* objectName)
    {
        dx::Package package(path);
        int32_t ref = package.findExport(className, objectName);
        REQUIRE(ref != 0);
        dx::Reader reader = package.load(ref).native();
        reader.index();
        reader.u32();
        int32_t size = reader.index();
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        reader.bytes(bytes.data(), bytes.size());
        return bytes;
    }

    void loadSound(dx::USound& sound, const char* name)
    {
        sound.bytes = resourceBytes(dx::test::gameData() + "/System/DeusExSounds.u", "Sound", name);
    }

    std::vector<uint8_t> sampleWave(const std::vector<int16_t>& samples, uint32_t rate)
    {
        uint32_t frames = static_cast<uint32_t>(samples.size());
        std::vector<uint8_t> bytes;
        auto u32 = [&bytes](uint32_t v) {
            for (int i = 0; i < 4; i++) {
                bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
            }
        };
        auto u16 = [&bytes](uint16_t v) {
            bytes.push_back(static_cast<uint8_t>(v));
            bytes.push_back(static_cast<uint8_t>(v >> 8));
        };
        bytes.insert(bytes.end(), { 'R', 'I', 'F', 'F' });
        u32(36 + frames * 2);
        bytes.insert(bytes.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
        u32(16);
        u16(1);
        u16(1);
        u32(rate);
        u32(rate * 2);
        u16(2);
        u16(16);
        bytes.insert(bytes.end(), { 'd', 'a', 't', 'a' });
        u32(frames * 2);
        for (int16_t sample : samples) {
            u16(static_cast<uint16_t>(sample));
        }
        return bytes;
    }

    std::vector<uint8_t> loopWave(const std::vector<int16_t>& samples, uint32_t rate, uint32_t start, uint32_t last)
    {
        std::vector<uint8_t> bytes = sampleWave(samples, rate);
        std::vector<uint32_t> chunk(15, 0);
        chunk[7] = 1;
        chunk[11] = start;
        chunk[12] = last;
        bytes.insert(bytes.end(), { 's', 'm', 'p', 'l', 60, 0, 0, 0 });
        for (uint32_t value : chunk) {
            for (int i = 0; i < 4; i++) {
                bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
            }
        }
        uint32_t riff = static_cast<uint32_t>(bytes.size() - 8);
        for (int i = 0; i < 4; i++) {
            bytes[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(riff >> (8 * i));
        }
        return bytes;
    }

    std::vector<uint8_t> constantWave(int16_t value, uint32_t rate, uint32_t frames)
    {
        return sampleWave(std::vector<int16_t>(frames, value), rate);
    }

    void renderFrames(size_t frames)
    {
        std::vector<float> buffer(4096 * 2);
        while (frames > 0) {
            size_t chunk = std::min<size_t>(frames, 4096);
            dx::audio::render(buffer.data(), chunk);
            frames -= chunk;
        }
    }

    std::shared_ptr<const dx::audio::ItModule> trainingMusic()
    {
        std::vector<uint8_t> bytes = resourceBytes(dx::test::gameData() + "/Music/Training_Music.umx", "Music", "Training_Music");
        auto module = std::make_shared<dx::audio::ItModule>();
        REQUIRE(module->load(bytes.data(), bytes.size()));
        return module;
    }

    struct TrackerCell {
        int channel = 0;
        int note = -1;
        int instrument = 0;
        int volume = -1;
        int command = 0;
        int param = 0;
    };

    struct TrackerSample {
        std::vector<int16_t> data;
        uint8_t flags = 0x13;
        uint32_t loopBegin = 0;
        uint32_t loopEnd = 0;
        uint32_t sustainBegin = 0;
        uint32_t sustainEnd = 0;
        uint8_t globalVolume = 64;
        uint8_t volume = 64;
    };

    std::vector<uint8_t> trackerModule(const std::vector<std::vector<TrackerCell>>& rows, const TrackerSample& sample)
    {
        std::vector<uint8_t> bytes(0xC0, 0);
        auto put16 = [&bytes](size_t at, uint32_t v) {
            bytes[at] = static_cast<uint8_t>(v);
            bytes[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        auto put32 = [&bytes](size_t at, uint32_t v) {
            for (size_t i = 0; i < 4; i++) {
                bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
            }
        };
        std::memcpy(bytes.data(), "IMPM", 4);
        put16(0x20, 2);
        put16(0x22, 0);
        put16(0x24, 1);
        put16(0x26, 1);
        put16(0x28, 0x217);
        put16(0x2A, 0x200);
        put16(0x2C, 0x09);
        bytes[0x30] = 128;
        bytes[0x31] = 48;
        bytes[0x32] = 6;
        bytes[0x33] = 125;
        bytes[0x34] = 128;
        for (size_t i = 0; i < 64; i++) {
            bytes[0x40 + i] = 32;
            bytes[0x80 + i] = 64;
        }
        bytes.push_back(0);
        bytes.push_back(255);
        size_t sampleTable = bytes.size();
        bytes.resize(bytes.size() + 8, 0);
        size_t sampleHeader = bytes.size();
        bytes.resize(bytes.size() + 0x50, 0);
        std::memcpy(bytes.data() + sampleHeader, "IMPS", 4);
        bytes[sampleHeader + 0x11] = sample.globalVolume;
        bytes[sampleHeader + 0x12] = sample.flags;
        bytes[sampleHeader + 0x13] = sample.volume;
        bytes[sampleHeader + 0x2E] = 1;
        bytes[sampleHeader + 0x2F] = 32;
        put32(sampleHeader + 0x30, static_cast<uint32_t>(sample.data.size()));
        put32(sampleHeader + 0x34, sample.loopBegin);
        put32(sampleHeader + 0x38, sample.loopEnd);
        put32(sampleHeader + 0x3C, 44100);
        put32(sampleHeader + 0x40, sample.sustainBegin);
        put32(sampleHeader + 0x44, sample.sustainEnd);
        size_t pattern = bytes.size();
        bytes.resize(bytes.size() + 8, 0);
        for (const std::vector<TrackerCell>& row : rows) {
            for (const TrackerCell& cell : row) {
                uint8_t mask = static_cast<uint8_t>((cell.note >= 0 ? 1 : 0) | (cell.instrument > 0 ? 2 : 0) | (cell.volume >= 0 ? 4 : 0) | (cell.command > 0 ? 8 : 0));
                bytes.push_back(static_cast<uint8_t>((cell.channel + 1) | 0x80));
                bytes.push_back(mask);
                if (cell.note >= 0) {
                    bytes.push_back(static_cast<uint8_t>(cell.note));
                }
                if (cell.instrument > 0) {
                    bytes.push_back(static_cast<uint8_t>(cell.instrument));
                }
                if (cell.volume >= 0) {
                    bytes.push_back(static_cast<uint8_t>(cell.volume));
                }
                if (cell.command > 0) {
                    bytes.push_back(static_cast<uint8_t>(cell.command));
                    bytes.push_back(static_cast<uint8_t>(cell.param));
                }
            }
            bytes.push_back(0);
        }
        for (size_t i = rows.size(); i < 64; i++) {
            bytes.push_back(0);
        }
        put16(pattern, static_cast<uint32_t>(bytes.size() - pattern - 8));
        put16(pattern + 2, 64);
        size_t data = bytes.size();
        for (int16_t value : sample.data) {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
        }
        put32(sampleTable, static_cast<uint32_t>(sampleHeader));
        put32(sampleTable + 4, static_cast<uint32_t>(pattern));
        put32(sampleHeader + 0x48, static_cast<uint32_t>(data));
        return bytes;
    }

    std::shared_ptr<const dx::audio::ItModule> trackerSong(const std::vector<std::vector<TrackerCell>>& rows, const TrackerSample& sample)
    {
        std::vector<uint8_t> bytes = trackerModule(rows, sample);
        auto module = std::make_shared<dx::audio::ItModule>();
        REQUIRE(module->load(bytes.data(), bytes.size()));
        return module;
    }

    TrackerSample constantSample(int16_t value)
    {
        TrackerSample sample;
        sample.data.assign(64, value);
        sample.loopEnd = 64;
        return sample;
    }

    std::vector<float> renderSong(std::shared_ptr<const dx::audio::ItModule> module, size_t frames)
    {
        dx::audio::ModulePlayer player(module, 44100);
        std::vector<float> out(frames * 2);
        player.render(out.data(), frames);
        return out;
    }
}

TEST_CASE("PCM wav sound decodes with header format and length", "[data]")
{
    std::vector<uint8_t> bytes = resourceBytes(dx::test::gameData() + "/System/DeusExSounds.u", "Sound", "AlarmUnitHum");
    auto pcm = dx::audio::decodeBytes(bytes);
    REQUIRE(pcm);
    CHECK(pcm->sampleRate == 22050);
    CHECK(pcm->channels == 1);
    CHECK(pcm->frames() == 76944);
    CHECK(pcm->fixed[0] == -453);
    CHECK(pcm->duration() == Catch::Approx(76944.0 / 22050.0).margin(1e-6));
}

TEST_CASE("mp3 speech decodes every mpeg frame", "[data]")
{
    std::vector<uint8_t> bytes = resourceBytes(dx::test::gameData() + "/System/DeusExConAudioMission01.u", "Sound", "ConAudioMission01_0");
    dx::USound sound;
    // The decoded-sound cache keys on the USound pointer; shutting audio down drops the entry
    // before this sound goes.
    dx::test::OfflineAudio audio;
    sound.bytes = bytes;
    auto pcm = dx::audio::decode(&sound);
    REQUIRE(pcm);
    CHECK(pcm->sampleRate == 44100);
    CHECK(pcm->channels == 1);
    CHECK(pcm->frames() == 71 * 1152);
    CHECK(dx::audio::duration(&sound) == Catch::Approx(81792.0 / 44100.0).margin(1e-6));
    CHECK(sound.duration == Catch::Approx(81792.0 / 44100.0).margin(1e-6));
}

TEST_CASE("garbage bytes do not decode")
{
    std::vector<uint8_t> bytes(64, 0x42);
    CHECK_FALSE(dx::audio::decodeBytes(bytes));
}

TEST_CASE("Galaxy pans by the view space angle over seven eighths of the range and attenuates linearly")
{
    dx::Vector listener;
    dx::Rotator facingX;
    dx::audio::Placement right = dx::audio::place(listener, facingX, { 0, 100, 0 }, 400.0f);
    CHECK(right.pan == 30718);
    CHECK(right.attenuation == Catch::Approx(0.75f));

    dx::audio::Placement left = dx::audio::place(listener, facingX, { 0, -100, 0 }, 400.0f);
    CHECK(left.pan == 2047);

    dx::audio::Placement front = dx::audio::place(listener, facingX, { 100, 0, 0 }, 200.0f);
    CHECK(front.pan == 16383);
    CHECK(front.attenuation == Catch::Approx(0.5f));

    dx::audio::Placement behind = dx::audio::place(listener, facingX, { -100, 0, 0 }, 400.0f);
    CHECK(behind.pan == 16383);

    dx::audio::Placement close = dx::audio::place(listener, facingX, { 0, 10, 0 }, 400.0f);
    CHECK(close.pan == 19966);

    dx::audio::Placement outside = dx::audio::place(listener, facingX, { 500, 0, 0 }, 200.0f);
    CHECK(outside.attenuation == 0.0f);

    dx::Rotator facingY;
    facingY.yaw = 16384;
    CHECK(dx::audio::place(listener, facingY, { 0, 100, 0 }, 400.0f).pan == 16383);
    CHECK(dx::audio::place(listener, facingY, { -100, 0, 0 }, 400.0f).pan == 30718);
}

TEST_CASE("Galaxy master volume takes the louder of sound and speech volume on a 127 scale")
{
    CHECK(dx::audio::masterVolume(255, 255) == 127);
    CHECK(dx::audio::masterVolume(204, 255) == 126);
    CHECK(dx::audio::masterVolume(255, 204) == 127);
    CHECK(dx::audio::masterVolume(0, 0) == 0);
}

TEST_CASE("Galaxy voice volume scales by category, obstruction and attenuation with a floor of 128")
{
    CHECK(dx::audio::voiceVolume(0.75f, 0.0f, 204, 255, false, 1.0f) == 19660);
    CHECK(dx::audio::voiceVolume(0.75f, 0.0f, 204, 255, true, 1.0f) == 24575);
    CHECK(dx::audio::voiceVolume(1.0f, 0.5f, 255, 255, false, 1.0f) == 10813);
    CHECK(dx::audio::voiceVolume(1.0f, 0.25f, 255, 255, false, 1.0f) == 16383);
    CHECK(dx::audio::voiceVolume(0.0f, 0.0f, 255, 255, false, 1.0f) == 128);
    CHECK(dx::audio::voiceVolume(0.75f, 0.0f, 204, 255, true, 2.0f) == 32767);
}

TEST_CASE("Galaxy channel gains use the square root pan table and the fixed point volume chain")
{
    dx::audio::Gains center = dx::audio::channelGains(32767, 16383, 127, 128);
    CHECK(center.left == Catch::Approx(22450.0 * 32767.0 / 1073741824.0).epsilon(1e-6));
    CHECK(center.right == Catch::Approx(22449.0 * 32767.0 / 1073741824.0).epsilon(1e-6));

    dx::audio::Gains send = dx::audio::channelGains(32767, 16383, 127, 127);
    CHECK(send.left == Catch::Approx(22274.0 * 32767.0 / 1073741824.0).epsilon(1e-6));

    dx::audio::Gains panned = dx::audio::channelGains(19660, 30718, 126, 128);
    CHECK(panned.left == Catch::Approx(4688.0 * 32767.0 / 1073741824.0).epsilon(1e-6));
    CHECK(panned.right == Catch::Approx(18154.0 * 32767.0 / 1073741824.0).epsilon(1e-6));
}

TEST_CASE("Galaxy Doppler shifts ambient pitch by the approach speed over DopplerSpeed")
{
    dx::Vector listener;
    dx::Vector source { 1000, 0, 0 };
    CHECK(dx::audio::dopplerShift({ 650, 0, 0 }, source, listener, 6500.0f) == Catch::Approx(0.9f));
    CHECK(dx::audio::dopplerShift({ -650, 0, 0 }, source, listener, 6500.0f) == Catch::Approx(1.1f));
    CHECK(dx::audio::dopplerShift({ -13000, 0, 0 }, source, listener, 6500.0f) == 2.0f);
    CHECK(dx::audio::dopplerShift({ 13000, 0, 0 }, source, listener, 6500.0f) == 0.5f);
    CHECK(dx::audio::dopplerShift({ 0, 650, 0 }, source, listener, 6500.0f) == 1.0f);

    CHECK(dx::audio::voiceStep(44100, 1.0f, 1.0f, 44100) == 65536);
    CHECK(dx::audio::voiceStep(11025, 1.0f, 1.0f, 44100) == 16384);
    CHECK(dx::audio::voiceStep(22050, 1.0f, 0.9f, 44100) == 29489);
    CHECK(dx::audio::voiceStep(22050, 1.0f, 1.1f, 44100) == 36044);
}

TEST_CASE("a centered full volume voice starts at the next update at the Galaxy constant power level", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound tone;
    tone.bytes = constantWave(16384, 44100, 8000);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    int32_t id = dx::audio::playSound(&tone, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false);
    REQUIRE(id != 0);
    std::vector<float> buffer(4000 * 2);
    dx::audio::render(buffer.data(), 4000);
    CHECK(dx::audio::isPlaying(id));
    CHECK(*std::max_element(buffer.begin(), buffer.end()) == 0.0f);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    dx::audio::render(buffer.data(), 4000);
    CHECK(buffer[0] == Catch::Approx(0.5 * 22450.0 * 32767.0 / 1073741824.0).epsilon(1e-6));
    CHECK(buffer[3999 * 2] == Catch::Approx(0.5 * 22450.0 * 32767.0 / 1073741824.0).epsilon(1e-5));
    CHECK(buffer[3999 * 2 + 1] == Catch::Approx(0.5 * 22449.0 * 32767.0 / 1073741824.0).epsilon(1e-5));
    audio.shutdown();
}

TEST_CASE("before any update an unplaced voice plays at once and ends, and a placed one waits", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound tone;
    tone.bytes = constantWave(16384, 44100, 4000);
    int32_t preview = dx::audio::playSound(&tone, nullptr, dx::audio::SlotNone, {}, 1.0f, 4096.0f, 1.0f, false);
    REQUIRE(preview != 0);
    std::vector<float> buffer(4000 * 2);
    dx::audio::render(buffer.data(), 4000);
    CHECK(buffer[0] == Catch::Approx(0.5 * 22450.0 * 32767.0 / 1073741824.0).epsilon(1e-6));
    dx::audio::render(buffer.data(), 4000);
    CHECK_FALSE(dx::audio::isPlaying(preview));

    int32_t placed = dx::audio::playSound(&tone, nullptr, dx::audio::SlotNone, { 100, 0, 0 }, 1.0f, 1000.0f, 1.0f, true);
    REQUIRE(placed != 0);
    dx::audio::render(buffer.data(), 4000);
    CHECK(dx::audio::isPlaying(placed));
    CHECK(*std::max_element(buffer.begin(), buffer.end()) == 0.0f);
    audio.shutdown();
}

TEST_CASE("voice steps stay exact at output rates of 65536 and above")
{
    CHECK(dx::audio::voiceStep(96000, 1.0f, 1.0f, 96000) == 65536);
    CHECK(dx::audio::voiceStep(48000, 1.0f, 1.0f, 96000) == 32768);
    CHECK(dx::audio::voiceStep(70000, 1.0f, 1.0f, 96000) == 47786);
    CHECK(dx::audio::voiceStep(22050, 1.0f, 1.0f, 192000) == 7526);
    CHECK(dx::audio::voiceStep(150000, 1.0f, 1.0f, 100000) == 98304);
}

TEST_CASE("the decoded sound cache drops the least recently used sound past its limit", "[data]")
{
    dx::test::OfflineAudio audio;
    CHECK(dx::audio::cacheStats().limitBytes == 64ull * 1024 * 1024);
    dx::USound first;
    dx::USound second;
    dx::USound third;
    first.bytes = constantWave(1000, 22050, 4000);
    second.bytes = constantWave(2000, 22050, 4000);
    third.bytes = constantWave(3000, 22050, 4000);
    // Each decodes to 4000 16-bit samples, so two fit under the limit and three do not.
    dx::audio::setSoundCacheLimit(20000);
    auto firstPcm = dx::audio::decode(&first);
    auto secondPcm = dx::audio::decode(&second);
    REQUIRE(firstPcm);
    REQUIRE(secondPcm);
    CHECK(dx::audio::cacheStats().sounds == 2);
    CHECK(dx::audio::cacheStats().decodedBytes == 16000);
    CHECK(dx::audio::decode(&first) == firstPcm);
    REQUIRE(dx::audio::decode(&third));
    CHECK(dx::audio::cacheStats().sounds == 2);
    CHECK(dx::audio::cacheStats().decodedBytes == 16000);
    // The first was used after the second, so the second went. Its samples stay valid for a holder.
    CHECK(dx::audio::decode(&first) == firstPcm);
    CHECK(secondPcm->frames() == 4000);
    CHECK(secondPcm->fixed[0] == 2000);
    auto again = dx::audio::decode(&second);
    REQUIRE(again);
    CHECK(again != secondPcm);
    CHECK(again->fixed == secondPcm->fixed);
    // No limit keeps everything.
    dx::audio::setSoundCacheLimit(0);
    REQUIRE(dx::audio::decode(&first));
    REQUIRE(dx::audio::decode(&third));
    CHECK(dx::audio::cacheStats().sounds == 3);
    audio.shutdown();
}

TEST_CASE("voices advance in 16.16 fixed point with 15 bit interpolation weights and no pitch clamp", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound ramp;
    ramp.bytes = sampleWave({ 0, 20000, 0, 0 }, 22050);
    REQUIRE(dx::audio::playSound(&ramp, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false) != 0);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    std::vector<float> buffer(8 * 2);
    dx::audio::render(buffer.data(), 8);
    const double unit = 22450.0 / 35184372088832.0;
    CHECK(buffer[0] == 0.0f);
    CHECK(buffer[2] == Catch::Approx(20000.0 * 16384.0 * unit).epsilon(1e-7));
    CHECK(buffer[4] == Catch::Approx(20000.0 * 32767.0 * unit).epsilon(1e-7));
    CHECK(buffer[6] == Catch::Approx(20000.0 * 16383.0 * unit).epsilon(1e-7));
    dx::audio::shutdown();

    REQUIRE(dx::audio::initializeOffline(44100));
    dx::USound tone;
    tone.bytes = constantWave(1000, 22050, 4000);
    int32_t fast = dx::audio::playSound(&tone, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 8.0f, false);
    REQUIRE(fast != 0);
    CHECK(dx::audio::voices().front().step == 4.0);
    audio.shutdown();
}

TEST_CASE("the interpolation at a loop end reads the raw next sample before wrapping", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound looped;
    looped.bytes = loopWave({ 0, 0, 20000, 0 }, 22050, 0, 1);
    REQUIRE(dx::audio::playSound(&looped, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false) != 0);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    std::vector<float> buffer(8 * 2);
    dx::audio::render(buffer.data(), 8);
    const double unit = 22450.0 / 35184372088832.0;
    CHECK(buffer[2 * 2] == 0.0f);
    CHECK(buffer[3 * 2] == Catch::Approx(20000.0 * 16384.0 * unit).epsilon(1e-7));
    CHECK(buffer[4 * 2] == 0.0f);
    CHECK(buffer[7 * 2] == Catch::Approx(20000.0 * 16384.0 * unit).epsilon(1e-7));
    audio.shutdown();
}

TEST_CASE("ambient light sources scale their ambient sound by the flicker and strobe light functions", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        const dx::UObject* chosen = nullptr;
        for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
            if (voice.ambient && voice.volume > 2000) {
                chosen = voice.actor;
            }
        }
        REQUIRE(chosen != nullptr);
        dx::UObject* source = const_cast<dx::UObject*>(chosen);
        dx::actor::SoundVolume(source) = 128;
        dx::actor::LightBrightness(source) = 255;
        auto level = [&]() {
            dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                if (voice.actor == source) {
                    return voice.volume;
                }
            }
            return 0;
        };
        const uint8_t steady = 1;
        const uint8_t flicker = 4;
        const uint8_t strobe = 5;
        dx::actor::LightType(source) = steady;
        int32_t full = level();
        REQUIRE(full > 1000);
        dx::audio::setLightRandom([](int32_t) {
            return 0.75f;
        });
        dx::actor::LightType(source) = flicker;
        CHECK(level() == Catch::Approx(full * 0.75).margin(1.0));
        dx::audio::setLightRandom([](int32_t) {
            return 0.4f;
        });
        CHECK(level() == 128);
        dx::actor::LightType(source) = strobe;
        std::vector<int32_t> levels;
        for (int i = 0; i < 4; i++) {
            dx::levelinfo::TimeSeconds(world.levelInfo) += 0.1f;
            levels.push_back(level());
        }
        CHECK(std::min(levels[0], levels[1]) == 128);
        CHECK(std::max(levels[0], levels[1]) == full);
        CHECK(levels[2] == levels[0]);
        CHECK(levels[3] == levels[1]);
        dx::audio::setLightRandom(nullptr);
        dx::actor::LightType(source) = 0;
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

TEST_CASE("a speaking pawn takes its phoneme from the window a tenth of a second past the elapsed time", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UObject* speaker = nullptr;
        for (dx::UObject* actor : world.actors) {
            if (actor && actor->data && actor != player && dx::isPawn(actor)) {
                speaker = actor;
                break;
            }
        }
        REQUIRE(speaker != nullptr);
        std::vector<int16_t> samples(20000, 0);
        for (size_t n = 13230; n < 13230 + 1024; n++) {
            samples[n] = static_cast<int16_t>(16000.0 * std::cos(2.0 * 3.14159265358979 * 20.0 * static_cast<double>(n - 13230) / 1024.0));
        }
        dx::USound speech;
        speech.bytes = sampleWave(samples, 22050);
        dx::pawn::bIsSpeaking(speaker) = 1;
        REQUIRE(dx::audio::playSound(&speech, speaker, dx::audio::SlotTalk, dx::actor::Location(speaker), 1.0f, 100000.0f, 1.0f, true) != 0);
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.5f);
        CHECK(dx::pawn::nextPhoneme(speaker) == "O");
        dx::pawn::bIsSpeaking(speaker) = 0;
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

TEST_CASE("the reverb network is three allpass stages with a crossed half gain feedback")
{
    dx::audio::ReverbSettings settings;
    settings.volume = 1.0f;
    settings.cutoff = 0.0f;
    settings.delay = { 441.5f / 44100.0f, 13230.5f / 44100.0f, 882.5f / 44100.0f, 13230.5f / 44100.0f, 1764.5f / 44100.0f, 13230.5f / 44100.0f };
    settings.gain = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    dx::audio::Reverb reverb;
    reverb.configure(settings, 44100);
    REQUIRE(reverb.active());
    std::vector<float> send(4000 * 2, 0.0f);
    send[0] = 1.0f;
    std::vector<float> out(4000 * 2, 0.0f);
    reverb.process(send.data(), out.data(), 4000);
    auto left = [&](size_t n) {
        return out[n * 2];
    };
    CHECK(left(0) == Catch::Approx(-0.0625).margin(1e-7));
    CHECK(left(441) == Catch::Approx(0.09375).margin(1e-7));
    CHECK(left(882) == Catch::Approx(0.09375).margin(1e-7));
    CHECK(left(1323) == Catch::Approx(-0.140625).margin(1e-7));
    CHECK(left(1764) == Catch::Approx(0.09375).margin(1e-7));
    CHECK(left(2205) == Catch::Approx(-0.140625).margin(1e-7));
    CHECK(left(2646) == Catch::Approx(-0.140625).margin(1e-7));
    CHECK(left(3087) == Catch::Approx(0.2109375).margin(1e-7));
    CHECK(left(1) == 0.0f);
    CHECK(left(100) == 0.0f);
    CHECK(left(2) == Catch::Approx(-0.000244140625).margin(1e-9));
    CHECK(out[1] == 0.0f);
    CHECK(out[3] == Catch::Approx(0.00390625).margin(1e-9));

    settings.volume = 0.0f;
    reverb.configure(settings, 44100);
    CHECK_FALSE(reverb.active());
}

TEST_CASE("the reverb cutoff lowpasses the recirculated delay line")
{
    dx::audio::ReverbSettings settings;
    settings.volume = 1.0f;
    settings.cutoff = static_cast<float>(std::acos(0.5) * 44100.0 / 6.282);
    settings.delay = { 441.5f / 44100.0f, 13230.5f / 44100.0f, 1500.5f / 44100.0f, 13230.5f / 44100.0f, 3000.5f / 44100.0f, 13230.5f / 44100.0f };
    settings.gain = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    dx::audio::Reverb reverb;
    reverb.configure(settings, 44100);
    std::vector<float> send(1000 * 2, 0.0f);
    send[0] = 1.0f;
    std::vector<float> out(1000 * 2, 0.0f);
    reverb.process(send.data(), out.data(), 1000);
    const double k = std::sqrt(1.25) - 0.5;
    CHECK(out[441 * 2] == Catch::Approx(0.09375).margin(1e-7));
    CHECK(out[882 * 2] == Catch::Approx(0.046875 * k).margin(1e-5));
    CHECK(out[883 * 2] == Catch::Approx(0.046875 * k * (1.0 - k)).margin(1e-5));
}

TEST_CASE("the view actor zone selects the Galaxy reverb and a tail follows the dry sound", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("06_HongKong_WanChai_Garage"));
    {
        dx::World world;
        REQUIRE(world.loadMap("06_HongKong_WanChai_Garage"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UObject* reverbZone = nullptr;
        for (dx::UObject* actor : world.actors) {
            if (actor && actor->data && actor->name == dx::Name("ZoneInfo0")) {
                reverbZone = actor;
            }
        }
        REQUIRE(reverbZone != nullptr);
        dx::audio::setVolumes(1.0f, 0.0f, 1.0f);
        for (dx::UObject* actor : world.actors) {
            if (actor && actor->data) {
                dx::actor::AmbientSound(actor) = nullptr;
            }
        }
        dx::USound tone;
        tone.bytes = constantWave(8000, 44100, 2000);
        // Placed at the listener; an unplaced sound (the interface) never reaches the reverb.
        auto tailPeak = [&](bool placed = true) {
            int32_t id = dx::audio::playSound(&tone, nullptr, dx::audio::SlotNone, dx::actor::Location(player), 1.0f, 1000.0f, 1.0f, placed);
            REQUIRE(id != 0);
            dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
            std::vector<float> buffer(4410 * 2);
            dx::audio::render(buffer.data(), 2100);
            CHECK_FALSE(dx::audio::isPlaying(id));
            dx::audio::render(buffer.data(), 4410);
            float peak = 0;
            for (float sample : buffer) {
                peak = std::max(peak, std::fabs(sample));
            }
            return peak;
        };

        REQUIRE(dx::region(player).zone == reverbZone);
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        dx::audio::ReverbSettings settings = dx::audio::reverbSettings();
        CHECK(settings.volume == Catch::Approx(100.0 / 255.0).epsilon(1e-6));
        CHECK(settings.cutoff == 1000.0f);
        CHECK(settings.delay[0] == Catch::Approx(0.08).epsilon(1e-6));
        CHECK(settings.delay[1] == Catch::Approx(0.14).epsilon(1e-6));
        CHECK(settings.delay[2] == 0.001f);
        CHECK(settings.delay[5] == 0.001f);
        CHECK(settings.gain[0] == Catch::Approx(44.0 / 255.0).epsilon(1e-6));
        CHECK(settings.gain[1] == Catch::Approx(140.0 / 255.0).epsilon(1e-6));
        CHECK(settings.gain[2] == 0.001f);
        // The unplaced one first, before a placed one's tail is ringing.
        CHECK(tailPeak(false) == 0.0f);
        CHECK(tailPeak() > 0.01f);

        // Turning UseReverb off in game takes effect at the next update, with no restart.
        std::string useReverb = dx::console::execute(nullptr, "get ini:Engine.Engine.AudioDevice UseReverb");
        dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice UseReverb False");
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        CHECK(dx::audio::reverbSettings().volume == 0.0f);
        dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice UseReverb True");
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        CHECK(dx::audio::reverbSettings().volume == Catch::Approx(100.0 / 255.0).epsilon(1e-6));
        dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice UseReverb " + (useReverb.empty() ? std::string("True") : useReverb));
        // Setting an ini key also applies the ini's volumes, which would bring the music back.
        dx::audio::setVolumes(1.0f, 0.0f, 1.0f);

        dx::zoneinfo::bReverbZone(reverbZone) = 0;
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        CHECK(dx::audio::reverbSettings().volume == 0.0f);
        CHECK(tailPeak() == 0.0f);
        dx::objects::unloadPackage(dx::Name("06_HongKong_WanChai_Garage"));
    }
    audio.shutdown();
}

TEST_CASE("an obstructed sound fades to a third over half a second and recovers when clear", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UObject* source = nullptr;
        for (dx::UObject* actor : world.actors) {
            if (actor && actor->data && actor != player && dx::isPawn(actor)) {
                source = actor;
                break;
            }
        }
        REQUIRE(source != nullptr);
        dx::Vector eye = dx::actor::Location(player);
        eye.z += dx::pawn::EyeHeight(player);
        dx::actor::Location(source) = { eye.x, eye.y, eye.z - 4000.0f };
        REQUIRE_FALSE(world.fastLineCheck(eye, dx::actor::Location(source)));
        dx::USound sound;
        loadSound(sound, "TechGogglesLoop");
        const int32_t slotMisc = 1;
        int32_t id = dx::audio::playSound(&sound, source, slotMisc, dx::actor::Location(source), 1.0f, 100000.0f, 1.0f, true);
        REQUIRE(id != 0);
        auto status = [id]() {
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                if (voice.id == id) {
                    return voice;
                }
            }
            return dx::audio::VoiceStatus {};
        };
        dx::audio::update(&world, eye, {}, 0.2f);
        CHECK(status().obstruction == Catch::Approx(0.2f));
        for (int i = 0; i < 3; i++) {
            dx::audio::update(&world, eye, {}, 0.2f);
        }
        CHECK(status().obstruction == 0.5f);
        CHECK(status().volume == 10380);

        dx::actor::Location(source) = { eye.x + 10.0f, eye.y, eye.z };
        REQUIRE(world.fastLineCheck(eye, dx::actor::Location(source)));
        dx::audio::update(&world, eye, {}, 0.2f);
        CHECK(status().obstruction == Catch::Approx(0.3f));
        dx::audio::update(&world, eye, {}, 0.5f);
        CHECK(status().obstruction == 0.0f);
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

TEST_CASE("each line check moves the obstruction in the update that made it, while the mixer runs and ends voices", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::UObject* source = nullptr;
        dx::UObject* other = nullptr;
        for (dx::UObject* actor : world.actors) {
            if (actor && actor->data && actor != player && dx::isPawn(actor)) {
                (source ? other : source) = actor;
                if (other) {
                    break;
                }
            }
        }
        REQUIRE(other != nullptr);
        dx::Vector eye = dx::actor::Location(player);
        eye.z += dx::pawn::EyeHeight(player);
        const dx::Vector behindWall { eye.x, eye.y, eye.z - 4000.0f };
        const dx::Vector inView { eye.x + 10.0f, eye.y, eye.z };
        REQUIRE_FALSE(world.fastLineCheck(eye, behindWall));
        REQUIRE(world.fastLineCheck(eye, inView));
        dx::actor::Location(other) = inView;

        dx::USound looped;
        loadSound(looped, "TechGogglesLoop");
        // A few milliseconds long, so the mixer ends and drops it within a render or two.
        dx::USound blip;
        blip.bytes = sampleWave(std::vector<int16_t>(64, 1000), 22050);
        const int32_t slotMisc = 1;
        int32_t id = dx::audio::playSound(&looped, source, slotMisc, behindWall, 1.0f, 100000.0f, 1.0f, true);
        REQUIRE(id != 0);
        auto obstruction = [id]() {
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                if (voice.id == id) {
                    return voice.obstruction;
                }
            }
            return -1.0f;
        };

        // The mixer pulls on its own thread all through the updates, as a device would, though
        // more often: a small block every tenth of a millisecond.
        std::atomic<bool> stop { false };
        std::thread mixer([&stop] {
            std::vector<float> out(64 * 2);
            while (!stop) {
                dx::audio::render(out.data(), 64);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        const float delta = 0.05f;
        float expected = 0;
        bool landed = true;
        for (int frame = 0; frame < 300 && landed; frame++) {
            bool hidden = (frame / 4) % 2 == 0;
            dx::actor::Location(source) = hidden ? behindWall : inView;
            if (frame % 2 == 0 && dx::audio::voices().size() < 8) {
                dx::audio::playSound(&blip, other, dx::audio::SlotNone, inView, 1.0f, 100000.0f, 1.0f, true);
            }
            dx::audio::update(&world, eye, {}, delta);
            expected = hidden ? std::min(expected + delta, 0.5f) : std::max(expected - delta, 0.0f);
            landed = obstruction() == expected;
        }
        stop = true;
        mixer.join();
        CHECK(landed);
        CHECK(obstruction() == expected);
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

TEST_CASE("ambient sounds stop while the game is paused and moving ambient sources are Doppler shifted", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        auto ambient = []() {
            std::vector<dx::audio::VoiceStatus> result;
            for (const dx::audio::VoiceStatus& voice : dx::audio::voices()) {
                if (voice.ambient) {
                    result.push_back(voice);
                }
            }
            return result;
        };
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        std::vector<dx::audio::VoiceStatus> playing = ambient();
        REQUIRE_FALSE(playing.empty());

        dx::levelinfo::Pauser(world.levelInfo) = "Player";
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        CHECK(ambient().empty());
        dx::levelinfo::Pauser(world.levelInfo).clear();
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        playing = ambient();
        REQUIRE_FALSE(playing.empty());

        dx::UObject* mover = const_cast<dx::UObject*>(playing.front().actor);
        double still = playing.front().step;
        dx::Vector away = dx::actor::Location(mover) - dx::actor::Location(player);
        dx::actor::Velocity(mover) = away.normal() * 650.0f;
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        double receding = 0;
        for (const dx::audio::VoiceStatus& voice : ambient()) {
            if (voice.actor == mover) {
                receding = voice.step;
            }
        }
        CHECK(receding / still == Catch::Approx(0.9).epsilon(1e-3));
        dx::actor::Velocity(mover) = {};
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

TEST_CASE("impulse tracker header of training music", "[data]")
{
    auto module = trainingMusic();
    CHECK(module->title == "Training Room ");
    REQUIRE(module->orders.size() == 16);
    CHECK(module->orders[0] == 0);
    CHECK(module->orders[1] == 1);
    CHECK(module->orders[2] == 10);
    CHECK(module->orders[15] == 255);
    CHECK(module->samples.size() == 14);
    CHECK(module->patterns.size() == 13);
    CHECK(module->instruments.empty());
    CHECK_FALSE(module->useInstruments());
    CHECK(module->speed == 6);
    CHECK(module->tempo == 116);
    CHECK(module->globalVolume == 128);
    CHECK(module->mixVolume == 48);
    CHECK(module->patterns[0].rows == 64);
    const dx::audio::ItCell& first = module->patterns[0].cells[0];
    CHECK(first.note == 60);
    CHECK(first.instrument == 2);
    const dx::audio::ItCell& cut = module->patterns[0].cells[4];
    CHECK(cut.note == dx::audio::NoteNone);
    CHECK(cut.volume == 0);
    const dx::audio::ItCell& reused = module->patterns[0].cells[5 * 64 + 3];
    CHECK(reused.volume == 0);
    CHECK(reused.note == dx::audio::NoteNone);
}

TEST_CASE("module player advances rows at the tempo rate and renders deterministic audio", "[data]")
{
    auto module = trainingMusic();
    dx::audio::ModulePlayer a(module, 44100);
    dx::audio::ModulePlayer b(module, 44100);
    std::vector<float> first(44100 * 2);
    std::vector<float> second(44100 * 2);
    a.render(first.data(), 44100);
    b.render(second.data(), 44100);
    CHECK(first == second);
    CHECK(a.order() == 0);
    CHECK(a.row() == 7);

    double energy = 0;
    float peak = 0;
    for (float value : first) {
        energy += static_cast<double>(value) * static_cast<double>(value);
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(std::sqrt(energy / static_cast<double>(first.size())) > 0.001);
    CHECK(std::isfinite(peak));
}

TEST_CASE("module player starts at the requested song section", "[data]")
{
    auto module = trainingMusic();
    dx::audio::ModulePlayer player(module, 44100);
    player.setPosition(2);
    CHECK(player.order() == 2);
    CHECK(player.row() == 0);
    player.setPosition(15);
    CHECK(player.order() == 0);
}

TEST_CASE("wav loop points come from the smpl chunk", "[data]")
{
    auto looped = dx::audio::decodeBytes(resourceBytes(dx::test::gameData() + "/System/DeusExSounds.u", "Sound", "TechGogglesLoop"));
    REQUIRE(looped);
    CHECK(looped->frames() == 55295);
    CHECK(looped->looped);
    CHECK(looped->loopStart == 13);
    CHECK(looped->loopEnd == 55252);

    auto oneShot = dx::audio::decodeBytes(resourceBytes(dx::test::gameData() + "/System/DeusExSounds.u", "Sound", "GlassBreakSmall"));
    REQUIRE(oneShot);
    CHECK_FALSE(oneShot->looped);
}

TEST_CASE("ambient sound radius follows the actor sound radius")
{
    CHECK(dx::audio::ambientRadius(0) == 25.0f);
    CHECK(dx::audio::ambientRadius(32) == 825.0f);
    CHECK(dx::audio::ambientRadius(255) == 6400.0f);
}

TEST_CASE("a looped sound keeps playing after a one shot sound has ended", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound looped;
    loadSound(looped, "TechGogglesLoop");
    dx::USound oneShot;
    loadSound(oneShot, "GlassBreakSmall");
    float loopedLength = dx::audio::duration(&looped);
    REQUIRE(dx::audio::duration(&oneShot) < loopedLength);
    int32_t loopedId = dx::audio::playSound(&looped, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false);
    int32_t oneShotId = dx::audio::playSound(&oneShot, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false);
    REQUIRE(loopedId != 0);
    REQUIRE(oneShotId != 0);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    renderFrames(static_cast<size_t>(loopedLength * 2.0f * 44100.0f));
    CHECK(dx::audio::isPlaying(loopedId));
    CHECK_FALSE(dx::audio::isPlaying(oneShotId));
    audio.shutdown();
}

TEST_CASE("lowering EffectsChannels in game stops sounds past the new count and limits new ones", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice EffectsChannels 16");
    dx::test::OfflineAudio audio;
    dx::USound sound;
    loadSound(sound, "TechGogglesLoop");
    auto play = [&] {
        return dx::audio::playSound(&sound, nullptr, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, false);
    };
    // Free channels fill from the top, so these are on channels 15 to 13.
    std::vector<int32_t> before = { play(), play(), play() };
    for (int32_t id : before) {
        REQUIRE(dx::audio::isPlaying(id));
    }

    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice EffectsChannels 2");
    for (int32_t id : before) {
        CHECK_FALSE(dx::audio::isPlaying(id));
    }
    std::vector<int32_t> after = { play(), play(), play() };
    CHECK(std::count_if(after.begin(), after.end(), [](int32_t id) { return dx::audio::isPlaying(id); }) == 2);

    audio.shutdown();
}

TEST_CASE("a sound on an occupied actor slot replaces the earlier one unless it is no override", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound sound;
    loadSound(sound, "TechGogglesLoop");
    dx::UObject actor;
    const int32_t slotMisc = 1;
    int32_t first = dx::audio::playSound(&sound, &actor, slotMisc, {}, 1.0f, 1000.0f, 1.0f, true);
    int32_t second = dx::audio::playSound(&sound, &actor, slotMisc, {}, 1.0f, 1000.0f, 1.0f, true);
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    CHECK_FALSE(dx::audio::isPlaying(first));
    CHECK(dx::audio::isPlaying(second));

    int32_t blocked = dx::audio::playSound(&sound, &actor, slotMisc, {}, 1.0f, 1000.0f, 1.0f, true, true);
    CHECK(blocked == 0);
    CHECK(dx::audio::isPlaying(second));

    int32_t a = dx::audio::playSound(&sound, &actor, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, true);
    int32_t b = dx::audio::playSound(&sound, &actor, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, true);
    CHECK(dx::audio::isPlaying(a));
    CHECK(dx::audio::isPlaying(b));
    CHECK(dx::audio::isPlaying(second));
    audio.shutdown();
}

TEST_CASE("StopSound by a script id stops what that actor slot plays now, and never a SLOT_None sound", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound sound;
    loadSound(sound, "TechGogglesLoop");
    dx::UObject actor;
    const int32_t slotTalk = 6;
    const int32_t scriptId = 5 * 16 + slotTalk * 2;
    int32_t first = dx::audio::playSound(&sound, &actor, slotTalk, {}, 1.0f, 1000.0f, 1.0f, true, false, scriptId);
    int32_t second = dx::audio::playSound(&sound, &actor, slotTalk, {}, 1.0f, 1000.0f, 1.0f, true, false, scriptId);
    REQUIRE_FALSE(dx::audio::isPlaying(first));
    REQUIRE(dx::audio::isPlaying(second));
    // The bNoOverride bit is ignored when matching.
    dx::audio::stopSoundId(scriptId + 1);
    CHECK_FALSE(dx::audio::isPlaying(second));

    const int32_t noneId = 5 * 16;
    int32_t none = dx::audio::playSound(&sound, &actor, dx::audio::SlotNone, {}, 1.0f, 1000.0f, 1.0f, true, false, noneId);
    dx::audio::stopSoundId(noneId);
    CHECK(dx::audio::isPlaying(none));
    audio.shutdown();
}

TEST_CASE("the voice limit steals the quietest sound and rejects quieter ones", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::USound sound;
    loadSound(sound, "TechGogglesLoop");
    std::vector<int32_t> distant;
    for (int i = 0; i < 16; i++) {
        int32_t id = dx::audio::playSound(&sound, nullptr, dx::audio::SlotNone, { 500, 0, 0 }, 1.0f, 1000.0f, 1.0f, true);
        REQUIRE(id != 0);
        distant.push_back(id);
    }
    CHECK(dx::audio::voices().size() == 16);
    CHECK(dx::audio::voices().front().priority == Catch::Approx(0.5f));

    int32_t near = dx::audio::playSound(&sound, nullptr, dx::audio::SlotNone, { 100, 0, 0 }, 1.0f, 1000.0f, 1.0f, true);
    CHECK(near != 0);
    CHECK(dx::audio::isPlaying(near));
    size_t stillPlaying = static_cast<size_t>(std::count_if(distant.begin(), distant.end(), dx::audio::isPlaying));
    CHECK(stillPlaying == 15);
    CHECK(dx::audio::voices().size() == 16);

    int32_t far = dx::audio::playSound(&sound, nullptr, dx::audio::SlotNone, { 900, 0, 0 }, 1.0f, 1000.0f, 1.0f, true);
    CHECK(far == 0);
    CHECK(dx::audio::voices().size() == 16);
    audio.shutdown();
}

TEST_CASE("a fast fade drops three per second and switches two latencies after silence", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::UMusic music;
    music.bytes = resourceBytes(dx::test::gameData() + "/Music/LibertyIsland_Music.umx", "Music", "LibertyIsland_Music");
    const uint8_t instant = 1;
    const uint8_t fade = 3;
    const uint8_t fastFade = 4;
    dx::audio::playMusic(&music, 0, instant);
    dx::audio::MusicStatus status = dx::audio::musicStatus();
    CHECK(status.music == &music);
    CHECK(status.section == 0);
    CHECK(status.order == 0);
    CHECK_FALSE(status.pending);

    renderFrames(4410);
    dx::audio::playMusic(&music, 3, fastFade);
    status = dx::audio::musicStatus();
    CHECK(status.section == 0);
    CHECK(status.pending);

    renderFrames(7000);
    status = dx::audio::musicStatus();
    CHECK(status.pending);
    CHECK(status.order == 0);
    CHECK(status.fade == Catch::Approx(0.5238f).margin(0.002f));

    renderFrames(8000);
    status = dx::audio::musicStatus();
    CHECK(status.pending);
    CHECK(status.order == 0);
    CHECK(status.fade == Catch::Approx(-0.0204f).margin(0.002f));

    renderFrames(4000);
    status = dx::audio::musicStatus();
    CHECK_FALSE(status.pending);
    CHECK(status.section == 3);
    CHECK(status.order == 3);
    CHECK(status.row == 0);
    CHECK(status.fade == 1.0f);

    dx::audio::playMusic(&music, 255, fade);
    CHECK(dx::audio::musicStatus().order == 3);
    CHECK(dx::audio::musicStatus().pending);
    renderFrames(44100 + 4096);
    status = dx::audio::musicStatus();
    CHECK(status.music == &music);
    CHECK(status.section == 255);
    CHECK(status.order == -1);
    CHECK_FALSE(status.pending);
    audio.shutdown();
}

TEST_CASE("asking again for the music a fade is leaving plays it whether the fade has ended or not", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::UMusic first;
    first.bytes = resourceBytes(dx::test::gameData() + "/Music/Training_Music.umx", "Music", "Training_Music");
    dx::UMusic second;
    second.bytes = resourceBytes(dx::test::gameData() + "/Music/LibertyIsland_Music.umx", "Music", "LibertyIsland_Music");
    const uint8_t instant = 1;
    const uint8_t fastFade = 4;
    auto heard = [] {
        std::vector<float> out(4096 * 2);
        float loudest = 0;
        for (int i = 0; i < 8; i++) {
            dx::audio::render(out.data(), 4096);
            for (float sample : out) {
                loudest = std::max(loudest, std::abs(sample));
            }
        }
        return loudest;
    };

    dx::audio::playMusic(&first, 0, instant);
    renderFrames(4410);
    dx::audio::playMusic(&second, 0, fastFade);
    renderFrames(44100);
    dx::audio::MusicStatus status = dx::audio::musicStatus();
    REQUIRE_FALSE(status.pending);
    REQUIRE(status.music == &second);
    dx::audio::playMusic(&first, 0, fastFade);
    renderFrames(44100);
    status = dx::audio::musicStatus();
    CHECK_FALSE(status.pending);
    CHECK(status.music == &first);
    CHECK(status.order == 0);
    CHECK(heard() > 0.0f);

    dx::audio::playMusic(&second, 0, fastFade);
    renderFrames(4410);
    REQUIRE(dx::audio::musicStatus().pending);
    dx::audio::playMusic(&first, 0, fastFade);
    renderFrames(44100);
    status = dx::audio::musicStatus();
    CHECK_FALSE(status.pending);
    CHECK(status.music == &first);
    CHECK(status.order == 0);
    CHECK(heard() > 0.0f);
    audio.shutdown();
}

TEST_CASE("a segue or a repeated section restarts the section at once and keeps the song tempo", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::UMusic music;
    music.bytes = resourceBytes(dx::test::gameData() + "/Music/Training_Music.umx", "Music", "Training_Music");
    const uint8_t instant = 1;
    const uint8_t segue = 2;
    dx::audio::playMusic(&music, 7, instant);
    renderFrames(44100);
    dx::audio::MusicStatus status = dx::audio::musicStatus();
    CHECK(status.order == 7);
    CHECK(status.row == 5);

    dx::audio::playMusic(&music, 7, instant);
    status = dx::audio::musicStatus();
    CHECK(status.order == 7);
    CHECK(status.row == 0);

    dx::audio::playMusic(&music, 1, segue);
    status = dx::audio::musicStatus();
    CHECK_FALSE(status.pending);
    CHECK(status.section == 1);
    CHECK(status.order == 1);
    CHECK(status.row == 0);
    renderFrames(44100);
    CHECK(dx::audio::musicStatus().row == 5);

    dx::audio::playMusic(&music, 0, instant);
    renderFrames(44100);
    CHECK(dx::audio::musicStatus().order == 0);
    CHECK(dx::audio::musicStatus().row == 7);
    audio.shutdown();
}

TEST_CASE("Galaxy ignores the impulse tracker mix volume", "[data]")
{
    auto module = trainingMusic();
    auto louder = std::make_shared<dx::audio::ItModule>(*module);
    louder->mixVolume = 96;
    dx::audio::ModulePlayer a(module, 44100);
    dx::audio::ModulePlayer b(louder, 44100);
    std::vector<float> first(44100 * 2);
    std::vector<float> second(44100 * 2);
    a.render(first.data(), 44100);
    b.render(second.data(), 44100);
    CHECK(first == second);
    float peak = 0;
    for (float value : first) {
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(peak > 0.01f);
}

TEST_CASE("Galaxy music voices run the fixed point gain chain through the square root pan table")
{
    std::vector<float> column = renderSong(trackerSong({ { { 0, 60, 1, 64 } } }, constantSample(16384)), 4096);
    CHECK(column[4000 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(column[4000 * 2 + 1] == Catch::Approx(0.5 * 13809.0 / 32768.0).epsilon(1e-5));
    std::vector<float> preset = renderSong(trackerSong({ { { 0, 60, 1 } } }, constantSample(16384)), 4096);
    CHECK(preset[4000 * 2] == Catch::Approx(0.5 * 13917.0 / 32768.0).epsilon(1e-5));
    CHECK(preset[4000 * 2 + 1] == Catch::Approx(0.5 * 13917.0 / 32768.0).epsilon(1e-5));
}

TEST_CASE("Galaxy drops channel volume, panbrello and the sample global volume")
{
    std::vector<float> plain = renderSong(trackerSong({ { { 0, 60, 1, 64 } }, {} }, constantSample(16384)), 12000);
    TrackerSample quiet = constantSample(16384);
    quiet.globalVolume = 16;
    std::vector<float> dropped = renderSong(trackerSong({ { { 0, 60, 1, 64, 13, 0x20 } }, { { 0, -1, 0, -1, 25, 0x44 } } }, quiet), 12000);
    CHECK(dropped == plain);
    CHECK(plain[4000 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0).epsilon(1e-5));
}

TEST_CASE("Galaxy ignores sustain loops and a note off cuts the sample at once")
{
    TrackerSample sustained;
    sustained.data.assign(2000, 16384);
    sustained.flags = 0x23;
    sustained.sustainBegin = 0;
    sustained.sustainEnd = 2000;
    std::vector<float> once = renderSong(trackerSong({ { { 0, 60, 1, 64 } } }, sustained), 4000);
    CHECK(once[1000 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(once[3000 * 2] == 0.0f);
    std::vector<float> released = renderSong(trackerSong({ { { 0, 60, 1, 64 } }, { { 0, 255 } } }, constantSample(16384)), 6000);
    CHECK(released[5000 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(released[5292 * 2 + 200] == 0.0f);
}

TEST_CASE("S9x centers the channel and inverts its right side for Galaxy surround")
{
    std::vector<float> out = renderSong(trackerSong({ { { 0, 60, 1, 64, 24, 0x00 } }, { { 0, -1, 0, -1, 19, 0x91 } } }, constantSample(16384)), 9000);
    CHECK(out[4000 * 2] == Catch::Approx(0.5 * 19528.0 / 32768.0).epsilon(1e-5));
    CHECK(out[4000 * 2 + 1] == 0.0f);
    CHECK(out[8800 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(out[8800 * 2 + 1] == Catch::Approx(-0.5 * 13809.0 / 32768.0).epsilon(1e-5));
}

TEST_CASE("Q keeps the volume and retriggers on every tick divisible by its interval")
{
    TrackerSample ramp;
    for (int i = 0; i < 2048; i++) {
        ramp.data.push_back(static_cast<int16_t>(i * 16));
    }
    ramp.flags = 0x03;
    std::vector<float> out = renderSong(trackerSong({ { { 0, 60, 1, 64, 17, 0x32 } } }, ramp), 4000);
    CHECK(out[1500 * 2] == Catch::Approx(1500.0 * 16.0 / 32768.0 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(out[1774 * 2] == Catch::Approx(10.0 * 16.0 / 32768.0 * 13808.0 / 32768.0).epsilon(1e-5));
    CHECK(out[3538 * 2] == Catch::Approx(10.0 * 16.0 / 32768.0 * 13808.0 / 32768.0).epsilon(1e-5));
}

TEST_CASE("vibrato bends by the Galaxy sine table times depth in 256ths of a semitone")
{
    TrackerSample ramp;
    for (int i = 0; i < 4000; i++) {
        ramp.data.push_back(static_cast<int16_t>(i * 8));
    }
    ramp.flags = 0x03;
    std::vector<float> out = renderSong(trackerSong({ { { 0, 60, 1, 64, 8, 0x48 } } }, ramp), 2000);
    double position = 882.0 + 882.0 * std::pow(2.0, 97.0 / 3072.0);
    CHECK(out[1764 * 2] == Catch::Approx(position * 8.0 / 32768.0 * 13808.0 / 32768.0).epsilon(1e-4));
}

TEST_CASE("the loader resolves effect memory per channel and drops what Galaxy drops")
{
    auto module = trackerSong({ { { 0, 60, 1, 32, 15, 0x10 } }, { { 0, -1, 0, -1, 4, 0x00 } }, { { 0, -1, 0, -1, 25, 0x44 } }, { { 0, -1, 0, -1, 1, 0x40 } }, { { 0, -1, 0, -1, 17, 0x32 } } }, constantSample(16384));
    const std::vector<dx::audio::ItCell>& cells = module->patterns[0].cells;
    CHECK(cells[64].command == 4);
    CHECK(cells[64].param == 0x10);
    CHECK(cells[2 * 64].command == 0);
    CHECK(cells[3 * 64].param == 0x20);
    CHECK(cells[4 * 64].param == 0x02);
}

TEST_CASE("the music volume is squared into the Galaxy master level", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::UMusic music;
    music.bytes = trackerModule({ { { 0, 60, 1, 64 } } }, constantSample(16384));
    dx::audio::setVolumes(1.0f, 0.5f, 1.0f);
    dx::audio::playMusic(&music, 0, 1);
    std::vector<float> out(4096 * 2);
    dx::audio::render(out.data(), 4096);
    CHECK(out[4000 * 2] == Catch::Approx(0.5 * 13808.0 / 32768.0 * 3969.0 / 16129.0).epsilon(1e-5));
    dx::audio::setVolumes(1.0f, 1.0f, 1.0f);
    audio.shutdown();
}

TEST_CASE("level music starts at login and an attacking enemy switches to the combat section", "[data]")
{
    dx::test::OfflineAudio audio;
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOIsland"));
        dx::UObject* player = world.player();
        REQUIRE(player != nullptr);
        dx::audio::update(&world, dx::actor::Location(player), {}, 0.0f);
        dx::audio::MusicStatus status = dx::audio::musicStatus();
        REQUIRE(status.music != nullptr);
        CHECK(status.music->name == dx::Name("LibertyIsland_Music"));
        CHECK(status.section == 0);
        // With no change asked for, the player's SongSection follows the order
        // the music has reached.
        dx::playerpawn::SongSection(player) = 77;
        renderFrames(1470);
        dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
        CHECK(dx::playerpawn::SongSection(player) == dx::audio::musicStatus().order);

        dx::console::execute(player, "teleport -1900 -653 -95 32768");
        bool combat = false;
        for (int i = 0; i < 450 && !combat; i++) {
            world.tick(1.0f / 30.0f);
            dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
            renderFrames(1470);
            combat = dx::audio::musicStatus().section == 3;
        }
        CHECK(combat);
        renderFrames(1470);
        dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
        CHECK(dx::playerpawn::SongSection(player) == dx::audio::musicStatus().order);

        // A slow fade keeps the request in the player until the new section
        // starts: Transition stays set and SongSection stays as asked.
        const uint8_t slowFade = 5;
        dx::playerpawn::SongSection(player) = 0;
        dx::playerpawn::Transition(player) = slowFade;
        for (int i = 0; i < 30; i++) {
            dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
            renderFrames(1470);
        }
        CHECK(dx::playerpawn::Transition(player) == slowFade);
        CHECK(dx::playerpawn::SongSection(player) == 0);
        CHECK(dx::audio::musicStatus().section == 3);
        bool switched = false;
        for (int i = 0; i < 300 && !switched; i++) {
            dx::audio::update(&world, dx::actor::Location(player), {}, 1.0f / 30.0f);
            renderFrames(1470);
            switched = dx::playerpawn::Transition(player) == 0;
        }
        CHECK(switched);
        CHECK(dx::audio::musicStatus().section == 0);

        dx::audio::setVolumes(1.0f, 1.0f, 1.0f);
        dx::vm::Call music(player, dx::Name("SetInstantMusicVolume"));
        REQUIRE(music.valid());
        music.byte(51).run();
        CHECK(dx::audio::volumes().music == Catch::Approx(0.2f));
        CHECK(dx::audio::volumes().sound == 1.0f);
        CHECK(dx::audio::volumes().speech == 1.0f);
        dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOIsland"));
    }
    audio.shutdown();
}

namespace {
    // A second of looped white noise, the broadband sound an HRTF is heard best with.
    std::vector<uint8_t> noiseWave(uint32_t seed)
    {
        std::vector<int16_t> samples(44100);
        uint32_t state = seed;
        for (int16_t& sample : samples) {
            state = state * 1664525u + 1013904223u;
            sample = static_cast<int16_t>(static_cast<int32_t>(state >> 16) - 32768);
            sample = static_cast<int16_t>(sample / 2);
        }
        return loopWave(samples, 44100, 0, static_cast<uint32_t>(samples.size() - 1));
    }

    struct EarLevels {
        double left = 0;
        double right = 0;
    };

    EarLevels earLevels(const std::vector<float>& stereo)
    {
        EarLevels levels;
        size_t frames = stereo.size() / 2;
        for (size_t i = 0; i < frames; i++) {
            levels.left += static_cast<double>(stereo[i * 2]) * static_cast<double>(stereo[i * 2]);
            levels.right += static_cast<double>(stereo[i * 2 + 1]) * static_cast<double>(stereo[i * 2 + 1]);
        }
        levels.left = std::sqrt(levels.left / static_cast<double>(frames));
        levels.right = std::sqrt(levels.right / static_cast<double>(frames));
        return levels;
    }

    std::string audioSection()
    {
        return dx::config::system().get("Engine.Engine", "AudioDevice").value_or("Galaxy.GalaxyAudioSubsystem");
    }

    // An offline mixer with the Ambrosia HRTF mode asked for or not, at full volume. It needs no
    // game data, so these tests also run where there is none.
    class Mixer {
    public:
        explicit Mixer(bool hrtf, int channels = 16, bool raytraced = false)
        {
            dx::config::system().set(audioSection(), "AmbrosiaHRTF", hrtf ? "True" : "False");
            dx::config::system().set(audioSection(), "AmbrosiaRaytracedAudio", raytraced ? "True" : "False");
            dx::config::system().set(audioSection(), "EffectsChannels", std::to_string(channels));
            REQUIRE(dx::audio::initializeOffline(44100));
            dx::audio::setVolumes(1.0f, 1.0f, 1.0f);
        }

        ~Mixer()
        {
            dx::audio::shutdown();
            dx::audio::setVolumes(before.sound, before.music, before.speech);
        }

        Mixer(const Mixer&) = delete;
        Mixer& operator=(const Mixer&) = delete;

    private:
        dx::test::ConfigSnapshot settings;
        dx::audio::Volumes before = dx::audio::volumes();
    };

    // Plays looped noise from location to a listener at the origin turned by yaw, and returns the
    // ear levels over frames once the first quarter second has passed.
    EarLevels heardFrom(const dx::Vector& location, int32_t yaw, size_t frames = 22050)
    {
        dx::USound noise;
        noise.bytes = noiseWave(1);
        dx::Rotator rotation;
        rotation.yaw = yaw;
        dx::audio::update(nullptr, {}, rotation, 0.0f);
        int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, location, 1.0f, 4000.0f, 1.0f, true);
        REQUIRE(id != 0);
        dx::audio::update(nullptr, {}, rotation, 0.0f);
        renderFrames(11025);
        std::vector<float> buffer(frames * 2);
        dx::audio::render(buffer.data(), frames);
        dx::audio::stopSound(id);
        renderFrames(4096);
        return earLevels(buffer);
    }
}

TEST_CASE("the HRTF mode is off unless AmbrosiaHRTF asks for it")
{
    {
        dx::test::ConfigSnapshot settings;
        dx::config::system().set(audioSection(), "AmbrosiaHRTF", "");
        REQUIRE(dx::audio::initializeOffline(44100));
        CHECK_FALSE(dx::audio::hrtfActive());
        dx::audio::shutdown();
    }
    Mixer mixer(false);
    CHECK_FALSE(dx::audio::hrtfActive());
}

TEST_CASE("in the HRTF mode a source on the left is louder in the left ear and one on the right in the right")
{
    Mixer mixer(true);
    if (!dx::audio::hrtfActive()) {
        SKIP("Steam Audio is not available");
    }
    // Unreal's y axis points to the right of a listener facing along x.
    EarLevels left = heardFrom({ 0, -1000, 0 }, 0);
    EarLevels right = heardFrom({ 0, 1000, 0 }, 0);
    CHECK(left.left > 3 * left.right);
    CHECK(right.right > 3 * right.left);
    // Turned a quarter (yaw 16384 faces +y), +x is on the left and -x on the right, as Unreal's
    // axes are left-handed.
    EarLevels turnedLeft = heardFrom({ 1000, 0, 0 }, 16384);
    EarLevels turnedRight = heardFrom({ -1000, 0, 0 }, 16384);
    CHECK(turnedRight.right > 3 * turnedRight.left);
    CHECK(turnedLeft.left > 3 * turnedLeft.right);
}

TEST_CASE("in the HRTF mode a source at the listener is centred at Galaxy's centred level")
{
    EarLevels stereo;
    {
        Mixer mixer(false);
        stereo = heardFrom({}, 0);
    }
    Mixer mixer(true);
    if (!dx::audio::hrtfActive()) {
        SKIP("Steam Audio is not available");
    }
    dx::USound noise;
    noise.bytes = noiseWave(1);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, {}, 1.0f, 4000.0f, 1.0f, true);
    REQUIRE(id != 0);
    dx::audio::update(nullptr, {}, {}, 0.0f);
    renderFrames(11025);
    std::vector<float> buffer(22050 * 2);
    dx::audio::render(buffer.data(), 22050);
    // A spatial blend of 0 gives both ears the same samples.
    float loudest = 0;
    size_t uneven = 0;
    for (size_t i = 0; i < 22050; i++) {
        if (buffer[i * 2] != buffer[i * 2 + 1]) {
            uneven++;
        }
        loudest = std::max(loudest, std::fabs(buffer[i * 2]));
    }
    CHECK(uneven == 0);
    CHECK(loudest > 0.01f);
    EarLevels hrtf = earLevels(buffer);
    CHECK(hrtf.left == Catch::Approx(stereo.left).epsilon(0.01));
    CHECK(hrtf.right == Catch::Approx(stereo.right).epsilon(0.01));
}

namespace {
    constexpr size_t SplitFrames = 20000;

    // Renders SplitFrames of a mix of positioned and unpositioned sounds in calls of the given
    // sizes, in turn, with the HRTF mode asked for. Silence when Steam Audio is not available.
    std::vector<float> splitRender(const std::vector<size_t>& chunks)
    {
        Mixer mixer(true);
        std::vector<float> out(SplitFrames * 2);
        if (!dx::audio::hrtfActive()) {
            return out;
        }
        dx::USound left;
        dx::USound right;
        dx::USound shortTone;
        dx::USound flat;
        left.bytes = noiseWave(1);
        right.bytes = noiseWave(2);
        shortTone.bytes = constantWave(8000, 22050, 3000);
        flat.bytes = constantWave(4000, 44100, 30000);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        REQUIRE(dx::audio::playSound(&left, nullptr, dx::audio::SlotNone, { 300, -800, 100 }, 1.0f, 4000.0f, 1.0f, true) != 0);
        REQUIRE(dx::audio::playSound(&right, nullptr, dx::audio::SlotNone, { -200, 150, 0 }, 0.8f, 4000.0f, 1.3f, true) != 0);
        // Ends partway through a block, so its effect's tail rings out after it.
        REQUIRE(dx::audio::playSound(&shortTone, nullptr, dx::audio::SlotNone, { 50, 0, 0 }, 1.0f, 1000.0f, 1.0f, true) != 0);
        // Not positioned, so mixed by the stereo path inside the same blocks.
        REQUIRE(dx::audio::playSound(&flat, nullptr, dx::audio::SlotNone, {}, 0.5f, 1000.0f, 1.0f, false) != 0);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        size_t done = 0;
        for (size_t i = 0; done < SplitFrames; i++) {
            size_t count = std::min(chunks[i % chunks.size()], SplitFrames - done);
            dx::audio::render(out.data() + done * 2, count);
            done += count;
        }
        return out;
    }
}

TEST_CASE("in the HRTF mode any split of render calls gives the same samples as one call")
{
    std::vector<float> whole = splitRender({ SplitFrames });
    {
        Mixer mixer(true);
        if (!dx::audio::hrtfActive()) {
            SKIP("Steam Audio is not available");
        }
    }
    CHECK(*std::max_element(whole.begin(), whole.end()) > 0.01f);
    CHECK(splitRender({ 1, 7, 511, 512, 513, 1000, 3, 2048, 441 }) == whole);
    CHECK(splitRender({ 333 }) == whole);
}

namespace {
    // Frames in the loops switchRender plays: a multiple of the HRTF block, at the output rate so
    // each voice steps one sample a frame and is back where it started every SwitchLoop frames.
    constexpr size_t SwitchLoop = 2048;

    std::vector<uint8_t> switchWave(uint32_t seed)
    {
        std::vector<int16_t> samples(SwitchLoop);
        uint32_t state = seed;
        for (int16_t& sample : samples) {
            state = state * 1664525u + 1013904223u;
            sample = static_cast<int16_t>((static_cast<int32_t>(state >> 16) - 32768) / 2);
        }
        return loopWave(samples, 44100, 0, static_cast<uint32_t>(SwitchLoop - 1));
    }

    struct SwitchSounds {
        dx::USound left;
        dx::USound right;
        dx::USound flat;

        SwitchSounds()
        {
            left.bytes = switchWave(1);
            right.bytes = switchWave(2);
            flat.bytes = switchWave(3);
            dx::audio::update(nullptr, {}, {}, 0.0f);
            REQUIRE(dx::audio::playSound(&left, nullptr, dx::audio::SlotNone, { 300, -800, 100 }, 1.0f, 4000.0f, 1.0f, true) != 0);
            REQUIRE(dx::audio::playSound(&right, nullptr, dx::audio::SlotNone, { -200, 150, 0 }, 0.8f, 4000.0f, 1.0f, true) != 0);
            REQUIRE(dx::audio::playSound(&flat, nullptr, dx::audio::SlotNone, {}, 0.5f, 1000.0f, 1.0f, false) != 0);
            dx::audio::update(nullptr, {}, {}, 0.0f);
        }
    };

    std::vector<float> renderSome(size_t frames)
    {
        std::vector<float> out(frames * 2);
        for (size_t done = 0; done < frames;) {
            size_t count = std::min<size_t>(frames - done, 300);
            dx::audio::render(out.data() + done * 2, count);
            done += count;
        }
        return out;
    }

    std::vector<float> slice(const std::vector<float>& stereo, size_t start, size_t frames)
    {
        return { stereo.begin() + static_cast<std::ptrdiff_t>(start * 2), stereo.begin() + static_cast<std::ptrdiff_t>((start + frames) * 2) };
    }

    void switchHrtf(bool on)
    {
        dx::config::system().set(audioSection(), "AmbrosiaHRTF", on ? "True" : "False");
        dx::audio::applyMenuSettings();
    }
}

TEST_CASE("Galaxy stereo, HRTF and raytraced switch any way round while sounds play, each as from a fresh start")
{
    auto fresh = [](bool hrtf, bool raytraced, size_t frames) {
        Mixer mixer(hrtf, 16, raytraced);
        SwitchSounds sounds;
        return renderSome(frames);
    };
    std::vector<float> stereo = fresh(false, false, SwitchLoop * 2);
    std::vector<float> hrtf = fresh(true, false, SwitchLoop);
    std::vector<float> raytraced;
    {
        Mixer probe(false, 16, true);
        if (!dx::audio::raytracedActive()) {
            SKIP("Steam Audio's simulation is not available");
        }
    }
    raytraced = fresh(false, true, SwitchLoop);
    // Without a scene the raytraced mode's simulation finds nothing in the way and no reverb,
    // but it still runs the direct effects, so it is checked against its own fresh start.
    auto mode = [](bool hrtfOn, bool raytracedOn) {
        dx::config::system().set(audioSection(), "AmbrosiaHRTF", hrtfOn ? "True" : "False");
        dx::config::system().set(audioSection(), "AmbrosiaRaytracedAudio", raytracedOn ? "True" : "False");
        dx::audio::applyMenuSettings();
    };
    auto passes = [] {
        return dx::audio::acousticStatus().simulation.passes;
    };
    Mixer mixer(false);
    SwitchSounds sounds;
    CHECK(renderSome(SwitchLoop * 2) == slice(stereo, 0, SwitchLoop * 2));
    mode(true, false);
    CHECK(renderSome(SwitchLoop) == slice(hrtf, 0, SwitchLoop));
    mode(true, true);
    CHECK(dx::audio::raytracedActive());
    CHECK(renderSome(SwitchLoop) == slice(raytraced, 0, SwitchLoop));
    dx::audio::settleAcoustics();
    CHECK(passes() > 0);
    mode(true, false);
    // Switched off, the worker sleeps at once: no pass starts or is counted after the switch,
    // however late the worker gets to run.
    uint64_t asleep = passes();
    CHECK_FALSE(dx::audio::raytracedActive());
    CHECK(dx::audio::hrtfActive());
    CHECK(renderSome(SwitchLoop) == slice(hrtf, 0, SwitchLoop));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(passes() == asleep);
    CHECK(dx::audio::acousticStatus().simulation.paused);
    mode(false, false);
    CHECK(renderSome(SwitchLoop) == slice(stereo, 0, SwitchLoop));
    // Straight from stereo to raytraced and back, which brings the HRTF on and off with it.
    mode(false, true);
    CHECK(dx::audio::raytracedActive());
    CHECK(renderSome(SwitchLoop) == slice(raytraced, 0, SwitchLoop));
    mode(false, false);
    CHECK_FALSE(dx::audio::hrtfActive());
    CHECK(renderSome(SwitchLoop) == slice(stereo, 0, SwitchLoop));
    CHECK(dx::audio::voices().size() == 3);
}

TEST_CASE("the HRTF mode switches on and off while sounds play, each time as from a fresh start")
{
    std::vector<float> stereo;
    {
        Mixer mixer(false);
        SwitchSounds sounds;
        stereo = renderSome(SwitchLoop * 3);
    }
    std::vector<float> hrtf;
    {
        Mixer mixer(true);
        if (!dx::audio::hrtfActive()) {
            SKIP("Steam Audio is not available");
        }
        SwitchSounds sounds;
        hrtf = renderSome(SwitchLoop * 2);
    }

    Mixer mixer(false);
    SwitchSounds sounds;
    CHECK(renderSome(SwitchLoop * 2) == slice(stereo, 0, SwitchLoop * 2));
    // Switched on with the voices back at their loop start: what a fresh start in the mode plays.
    switchHrtf(true);
    CHECK(dx::audio::hrtfActive());
    CHECK(dx::audio::voices().size() == 3);
    CHECK(renderSome(SwitchLoop) == slice(hrtf, 0, SwitchLoop));
    // Switched off partway through a block: the rest of the block and the effect tails are
    // dropped, and the stereo mixer goes on from where the voices have got to, a whole number of
    // blocks after the switch on.
    const size_t extra = 700;
    CHECK(renderSome(extra) == slice(hrtf, SwitchLoop, extra));
    switchHrtf(false);
    CHECK_FALSE(dx::audio::hrtfActive());
    CHECK(dx::audio::voices().size() == 3);
    size_t mixed = (SwitchLoop + extra + 511) / 512 * 512;
    CHECK(renderSome(SwitchLoop) == slice(stereo, mixed % SwitchLoop, SwitchLoop));
    // Setting it again to what it is changes nothing.
    switchHrtf(false);
    CHECK(renderSome(SwitchLoop) == slice(stereo, mixed % SwitchLoop, SwitchLoop));
}

namespace {
    // Renders SwitchLoop * 2 frames of up to two looped positioned noises, muting or soloing
    // the first or second after the first SwitchLoop frames as asked.
    std::vector<float> debugRender(bool hrtf, bool first, bool second, const std::function<void(int32_t, int32_t)>& change = {})
    {
        Mixer mixer(hrtf);
        std::vector<float> out;
        if (hrtf && !dx::audio::hrtfActive()) {
            return out;
        }
        dx::USound a;
        dx::USound b;
        a.bytes = switchWave(1);
        b.bytes = switchWave(2);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        int32_t idA = first ? dx::audio::playSound(&a, nullptr, dx::audio::SlotNone, { 300, -800, 100 }, 1.0f, 4000.0f, 1.0f, true) : 0;
        int32_t idB = second ? dx::audio::playSound(&b, nullptr, dx::audio::SlotNone, { -200, 150, 0 }, 0.8f, 4000.0f, 1.0f, true) : 0;
        dx::audio::update(nullptr, {}, {}, 0.0f);
        out = renderSome(SwitchLoop);
        if (change) {
            change(idA, idB);
        }
        std::vector<float> rest = renderSome(SwitchLoop);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
}

TEST_CASE("the debug overlay's mute and solo silence voices that play on")
{
    for (bool hrtf : { false, true }) {
        CAPTURE(hrtf);
        std::vector<float> both = debugRender(hrtf, true, true);
        if (both.empty()) {
            continue;
        }
        std::vector<float> onlyB = debugRender(hrtf, false, true);
        // In the HRTF mode the silenced voice's effect tail rings out in the first block after.
        size_t settle = hrtf ? 1024 : 0;
        size_t from = SwitchLoop + settle;
        size_t frames = SwitchLoop - settle;
        // Muting the first leaves the second as if it played alone.
        std::vector<float> muted = debugRender(hrtf, true, true, [](int32_t a, int32_t) {
            dx::audio::setVoiceMuted(a, true);
        });
        CHECK(slice(muted, from, frames) == slice(onlyB, from, frames));
        // Soloing the second does the same.
        std::vector<float> soloed = debugRender(hrtf, true, true, [](int32_t, int32_t b) {
            dx::audio::setVoiceSoloed(b, true);
            CHECK(dx::audio::voices().size() == 2);
        });
        CHECK(slice(soloed, from, frames) == slice(onlyB, from, frames));
        // Muting both is silence.
        std::vector<float> all = debugRender(hrtf, true, true, [](int32_t a, int32_t b) {
            dx::audio::setVoiceMuted(a, true);
            dx::audio::setVoiceMuted(b, true);
        });
        CHECK(slice(all, from, frames) == std::vector<float>(frames * 2, 0.0f));
    }
    // A muted voice still advances, so unmuted it carries on where it would have been.
    dx::USound a;
    a.bytes = switchWave(1);
    std::vector<float> reference;
    {
        Mixer mixer(false);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::playSound(&a, nullptr, dx::audio::SlotNone, { 300, -800, 100 }, 1.0f, 4000.0f, 1.0f, true);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        reference = renderSome(SwitchLoop * 2);
    }
    {
        Mixer mixer(false);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        int32_t id = dx::audio::playSound(&a, nullptr, dx::audio::SlotNone, { 300, -800, 100 }, 1.0f, 4000.0f, 1.0f, true);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::setVoiceMuted(id, true);
        std::vector<dx::audio::VoiceStatus> status = dx::audio::voices();
        REQUIRE(status.size() == 1);
        CHECK(status[0].muted);
        CHECK_FALSE(status[0].soloed);
        CHECK(status[0].attenuated);
        CHECK_FALSE(status[0].hrtf);
        CHECK(status[0].distance == Catch::Approx(std::sqrt(300.0 * 300.0 + 800.0 * 800.0 + 100.0 * 100.0)).epsilon(1e-5));
        CHECK(renderSome(SwitchLoop) == std::vector<float>(SwitchLoop * 2, 0.0f));
        dx::audio::setVoiceMuted(id, false);
        CHECK(renderSome(SwitchLoop) == slice(reference, SwitchLoop, SwitchLoop));
    }
}

TEST_CASE("set ini switches the HRTF mode from the console", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice AmbrosiaHRTF False");
    dx::test::OfflineAudio audio;
    CHECK_FALSE(dx::audio::hrtfActive());
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice AmbrosiaHRTF True");
    if (!dx::audio::hrtfActive()) {
        dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice AmbrosiaHRTF False");
        SKIP("Steam Audio is not available");
    }
    dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice AmbrosiaHRTF False");
    CHECK_FALSE(dx::audio::hrtfActive());
    audio.shutdown();
}

namespace {
    std::vector<uint8_t> checksumWave(uint32_t seed, uint32_t rate, uint32_t frames)
    {
        std::vector<int16_t> samples(frames);
        uint32_t state = seed;
        for (int16_t& sample : samples) {
            state = state * 1664525u + 1013904223u;
            sample = static_cast<int16_t>(static_cast<uint16_t>(state >> 16));
        }
        return sampleWave(samples, rate);
    }

    // A fingerprint of every sample of a mix of six placed and unplaced sounds, rendered in
    // uneven call sizes while the listener turns.
    uint64_t mixChecksum(bool hrtf)
    {
        Mixer mixer(hrtf);
        dx::audio::setVolumes(0.8f, 1.0f, 0.6f);
        std::vector<dx::USound> sounds(6);
        for (size_t i = 0; i < sounds.size(); i++) {
            sounds[i].bytes = checksumWave(static_cast<uint32_t>(i + 7), i % 2 ? 22050u : 44100u, 30000);
        }
        dx::Rotator rotation;
        rotation.yaw = 5000;
        rotation.pitch = 1200;
        dx::audio::update(nullptr, { 10, 20, 30 }, rotation, 0.0f);
        dx::audio::playSound(&sounds[0], nullptr, dx::audio::SlotNone, { 500, -300, 40 }, 1.0f, 2000.0f, 1.0f, true);
        dx::audio::playSound(&sounds[1], nullptr, dx::audio::SlotNone, { -100, 900, -40 }, 0.7f, 3000.0f, 1.2f, true);
        dx::audio::playSound(&sounds[2], nullptr, dx::audio::SlotNone, { 20, 30, 40 }, 1.0f, 1000.0f, 0.9f, true);
        dx::audio::playSound(&sounds[3], nullptr, dx::audio::SlotNone, {}, 0.5f, 1000.0f, 1.0f, false);
        dx::audio::playSound(&sounds[4], nullptr, dx::audio::SlotTalk, { 60, 10, 0 }, 1.0f, 1500.0f, 1.0f, true);
        dx::audio::playSound(&sounds[5], nullptr, dx::audio::SlotNone, { 0, 50, 0 }, 1.0f, 1000.0f, 1.0f, true);
        dx::audio::update(nullptr, { 10, 20, 30 }, rotation, 0.0f);
        uint64_t hash = 1469598103934665603ull;
        std::vector<float> buffer(4096 * 2);
        const size_t sizes[] = { 1, 441, 1024, 7, 333, 4096 };
        for (size_t i = 0; i < 60; i++) {
            size_t count = sizes[i % 6];
            dx::audio::render(buffer.data(), count);
            for (size_t k = 0; k < count * 2; k++) {
                uint32_t bits = 0;
                std::memcpy(&bits, &buffer[k], sizeof(bits));
                hash = (hash ^ bits) * 1099511628211ull;
            }
            if (i % 10 == 9) {
                rotation.yaw += 3000;
                dx::audio::update(nullptr, { 10, 20, 30 }, rotation, 0.05f);
            }
        }
        return hash;
    }
}

// The stereo and HRTF mixes must stay exactly as they are while Ambrosia's other sound modes are
// off. The values are this build's (MSVC, x64; Steam Audio 4.8.1 for the HRTF); other compilers
// round differently, so elsewhere the test only prints them.
TEST_CASE("the stereo and HRTF mixes are unchanged sample for sample")
{
    [[maybe_unused]] constexpr uint64_t HrtfChecksum = 0x9e161b90c8dc8602ull;
    uint64_t stereo = mixChecksum(false);
    std::printf("stereo mix checksum %016llx\n", static_cast<unsigned long long>(stereo));
#if defined(_MSC_VER)
    CHECK(stereo == 0x8dc32915d887aef1ull);
#endif
    {
        Mixer probe(true);
        if (!dx::audio::hrtfActive()) {
            SKIP("Steam Audio is not available");
        }
    }
    uint64_t hrtf = mixChecksum(true);
    std::printf("HRTF mix checksum %016llx\n", static_cast<unsigned long long>(hrtf));
#if defined(_MSC_VER)
    CHECK(hrtf == HrtfChecksum);
#endif
}

// Not run by default: prints how loud the HRTF mode is against Galaxy's pan around the listener.
TEST_CASE("HRTF loudness against the stereo pan", "[.hrtf-measure]")
{
    struct Place {
        const char* name;
        dx::Vector location;
    };

    std::vector<Place> places = { { "ahead", { 1000, 0, 0 } }, { "ahead right", { 707, 707, 0 } }, { "right", { 0, 1000, 0 } }, { "behind right", { -707, 707, 0 } }, { "behind", { -1000, 0, 0 } }, { "behind left", { -707, -707, 0 } }, { "left", { 0, -1000, 0 } }, { "ahead left", { 707, -707, 0 } }, { "above", { 0, 0, 1000 } }, { "below", { 0, 0, -1000 } }, { "near right, blend 1/4", { 0, 100, 0 } }, { "near right, blend 1/2", { 0, 200, 0 } }, { "near right, blend 3/4", { 0, 300, 0 } }, { "at the listener", { 0, 0, 0 } } };
    std::vector<EarLevels> stereo;
    {
        Mixer mixer(false);
        for (const Place& place : places) {
            stereo.push_back(heardFrom(place.location, 0));
        }
    }
    Mixer mixer(true);
    if (!dx::audio::hrtfActive()) {
        SKIP("Steam Audio is not available");
    }
    double horizontal = 0;
    for (size_t i = 0; i < places.size(); i++) {
        EarLevels hrtf = heardFrom(places[i].location, 0);
        double stereoPower = stereo[i].left * stereo[i].left + stereo[i].right * stereo[i].right;
        double hrtfPower = hrtf.left * hrtf.left + hrtf.right * hrtf.right;
        if (i < 8) {
            horizontal += hrtfPower / stereoPower / 8.0;
        }
        std::printf("%-24s stereo L %.4f R %.4f  hrtf L %.4f R %.4f  power %+.2f dB\n", places[i].name, stereo[i].left, stereo[i].right, hrtf.left, hrtf.right, 10.0 * std::log10(hrtfPower / stereoPower));
    }
    std::printf("average of the eight horizontal directions %+.2f dB\n", 10.0 * std::log10(horizontal));
}

namespace {
    // Seconds of one core the mixer takes per second of sound with every one of 64 channels
    // playing a positioned sound, in 10 ms callbacks with 50 updates a second. Negative when the
    // HRTF mode is asked for and Steam Audio is not available.
    double mixingCost(bool hrtf)
    {
        Mixer mixer(hrtf, 64);
        if (hrtf && !dx::audio::hrtfActive()) {
            return -1.0;
        }
        std::vector<dx::USound> sounds(64);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        for (size_t i = 0; i < sounds.size(); i++) {
            sounds[i].bytes = noiseWave(static_cast<uint32_t>(i + 1));
            double angle = static_cast<double>(i) * 0.7;
            dx::Vector location { static_cast<float>(1500.0 * std::cos(angle)), static_cast<float>(1500.0 * std::sin(angle)), static_cast<float>(static_cast<double>(i % 5) * 100.0 - 200.0) };
            REQUIRE(dx::audio::playSound(&sounds[i], nullptr, dx::audio::SlotNone, location, 1.0f, 4000.0f, 1.0f, true) != 0);
        }
        dx::audio::update(nullptr, {}, {}, 0.0f);
        REQUIRE(dx::audio::voices().size() == 64);
        const size_t seconds = 20;
        std::vector<float> buffer(441 * 2);
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < seconds * 100; i++) {
            if (i % 2 == 0) {
                dx::Rotator turning;
                turning.yaw = static_cast<int32_t>(i * 50);
                dx::audio::update(nullptr, {}, turning, 0.02f);
            }
            dx::audio::render(buffer.data(), 441);
        }
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REQUIRE(dx::audio::voices().size() == 64);
        return elapsed / static_cast<double>(seconds);
    }
}

// Not run by default: prints the mixer's cost with every channel playing a positioned sound.
TEST_CASE("HRTF mixing cost with 64 voices", "[.hrtf-measure]")
{
    double stereo = mixingCost(false);
    double hrtf = mixingCost(true);
    if (hrtf < 0) {
        SKIP("Steam Audio is not available");
    }
    std::printf("64 voices: stereo %.2f ms, HRTF %.2f ms of one core per second of sound (%.2f%% and %.2f%%)\n", stereo * 1000.0, hrtf * 1000.0, stereo * 100.0, hrtf * 100.0);
}

TEST_CASE("speech phonemes come from the strongest FFT bin of the window")
{
    auto tone = [](int bin) {
        dx::audio::DecodedSound pcm;
        pcm.sampleRate = 44100;
        pcm.channels = 1;
        pcm.fixed.resize(4096);
        for (size_t n = 0; n < pcm.fixed.size(); n++) {
            pcm.fixed[n] = bin < 0 ? int16_t { 0 } : static_cast<int16_t>(std::nearbyint(16384.0 * std::cos(2.0 * 3.14159265358979 * bin * static_cast<double>(n) / 1024.0)));
        }
        return dx::audio::speechPhoneme(pcm, 0);
    };
    CHECK(tone(-1) == 'X');
    CHECK(tone(3) == 'E');
    CHECK(tone(8) == 'U');
    CHECK(tone(12) == 'O');
    CHECK(tone(20) == 'A');
    CHECK(tone(40) == 'F');
    CHECK(tone(60) == 'T');
}
