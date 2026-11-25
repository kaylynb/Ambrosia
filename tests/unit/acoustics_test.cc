#include "audio/acoustic_scene.h"
#include "test_paths.h"
#include "audio/audio.h"
#include "audio/raytraced.h"
#include "core/config.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/bsp.h"
#include "engine/resources.h"
#include "engine/world.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
    using dx::audio::acoustics::Exclusion;
    using dx::audio::acoustics::Geometry;
    using dx::audio::acoustics::SurfaceInfo;

    std::string materialOf(const SurfaceInfo& surface)
    {
        int32_t material = dx::audio::acoustics::resolve(surface).material;
        return material < 0 ? "excluded" : dx::audio::acoustics::defaultMaterials()[static_cast<size_t>(material)].name;
    }

    struct Survey {
        const char* map;
        size_t surfaces;
        std::map<std::string, size_t> groups;
    };

    // From the audio map survey (every surface of the level's model by its texture's group).
    const std::vector<Survey> surveys = {
        { "01_NYC_UNATCOHQ", 3570, { { "Metal", 1236 }, { "Stone", 811 }, { "Wood", 755 }, { "Glass", 158 }, { "Concrete", 187 } } },
        { "02_NYC_Street", 6020, { { "Metal", 2795 }, { "Concrete", 1243 }, { "Brick", 665 }, { "Sky", 2 }, { "Background", 49 } } },
        { "15_Area51_Page", 11161, { { "Metal", 7300 }, { "Concrete", 1574 }, { "Ladder", 1245 }, { "Glass", 529 } } },
    };
}

namespace {
    std::string audioSection()
    {
        return dx::config::system().get("Engine.Engine", "AudioDevice").value_or("Galaxy.GalaxyAudioSubsystem");
    }

    // An offline mixer in the raytraced mode, at full volume, needing no game data.
    class RaytracedMixer {
    public:
        RaytracedMixer()
        {
            dx::config::system().set(audioSection(), "AmbrosiaRaytracedAudio", "True");
            REQUIRE(dx::audio::initializeOffline(44100));
            dx::audio::setVolumes(1.0f, 1.0f, 1.0f);
        }

        ~RaytracedMixer()
        {
            dx::audio::shutdown();
            dx::audio::setVolumes(before.sound, before.music, before.speech);
        }

        RaytracedMixer(const RaytracedMixer&) = delete;
        RaytracedMixer& operator=(const RaytracedMixer&) = delete;

    private:
        dx::test::ConfigSnapshot settings;
        dx::audio::Volumes before = dx::audio::volumes();
    };

    // A second of looped white noise as a mono 44.1 kHz wave with a smpl loop.
    std::vector<uint8_t> noiseWave()
    {
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
        const uint32_t frames = 44100;
        bytes.insert(bytes.end(), { 'R', 'I', 'F', 'F' });
        u32(36 + frames * 2 + 68);
        bytes.insert(bytes.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
        u32(16);
        u16(1);
        u16(1);
        u32(44100);
        u32(88200);
        u16(2);
        u16(16);
        bytes.insert(bytes.end(), { 'd', 'a', 't', 'a' });
        u32(frames * 2);
        uint32_t state = 1;
        for (uint32_t i = 0; i < frames; i++) {
            state = state * 1664525u + 1013904223u;
            u16(static_cast<uint16_t>(static_cast<int16_t>((static_cast<int32_t>(state >> 16) - 32768) / 2)));
        }
        bytes.insert(bytes.end(), { 's', 'm', 'p', 'l', 60, 0, 0, 0 });
        for (uint32_t i = 0; i < 15; i++) {
            u32(i == 7 ? 1u : (i == 12 ? frames - 1 : 0u));
        }
        return bytes;
    }

    // A box of six inward faces, centre and half sizes in units.
    void addBox(Geometry& geometry, const dx::Vector& centre, const dx::Vector& half, const SurfaceInfo& info, bool inward)
    {
        size_t surface = geometry.surfaceIndex(info);
        float sign = inward ? -1.0f : 1.0f;
        dx::Vector lo = centre - half;
        dx::Vector hi = centre + half;
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { lo.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { lo.x, lo.y, hi.z } }, { -sign, 0, 0 }, surface);
        geometry.addPolygon({ { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { hi.x, lo.y, hi.z } }, { sign, 0, 0 }, surface);
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z } }, { 0, -sign, 0 }, surface);
        geometry.addPolygon({ { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } }, { 0, sign, 0 }, surface);
        geometry.addPolygon({ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z } }, { 0, 0, -sign }, surface);
        geometry.addPolygon({ { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } }, { 0, 0, sign }, surface);
    }

    // A wall across x = at, spanning y and z far enough that nothing goes round it.
    void addWall(Geometry& geometry, float at, float thickness, const SurfaceInfo& info)
    {
        if (thickness <= 0) {
            size_t surface = geometry.surfaceIndex(info);
            geometry.addPolygon({ { at, -3000, -3000 }, { at, 3000, -3000 }, { at, 3000, 3000 }, { at, -3000, 3000 } }, { -1, 0, 0 }, surface);
            return;
        }
        addBox(geometry, { at, 0, 0 }, { thickness / 2, 3000, 3000 }, info, false);
    }

    struct Heard {
        double level = 0;
        // Energy of the sample-to-sample difference over energy: higher for a brighter sound.
        double brightness = 0;
        dx::audio::VoiceStatus status;
    };

    // Plays noise at source to a listener at the origin facing +x in geometry (or the scene as it
    // is, given null), and measures it.
    Heard hear(std::shared_ptr<const Geometry> geometry, const dx::Vector& source)
    {
        if (geometry) {
            dx::audio::setAcousticGeometry(std::move(geometry));
        }
        dx::USound noise;
        noise.bytes = noiseWave();
        dx::audio::update(nullptr, {}, {}, 0.0f);
        int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, source, 1.0f, 4000.0f, 1.0f, true);
        REQUIRE(id != 0);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::settleAcoustics();
        std::vector<float> buffer(4096 * 2);
        dx::audio::render(buffer.data(), 4096);
        buffer.assign(22050 * 2, 0.0f);
        dx::audio::render(buffer.data(), 22050);
        Heard heard;
        for (const dx::audio::VoiceStatus& status : dx::audio::voices()) {
            if (status.id == id) {
                heard.status = status;
            }
        }
        double energy = 0;
        double change = 0;
        for (size_t i = 1; i < 22050; i++) {
            for (size_t ear = 0; ear < 2; ear++) {
                double now = static_cast<double>(buffer[i * 2 + ear]);
                double before = static_cast<double>(buffer[(i - 1) * 2 + ear]);
                energy += now * now;
                change += (now - before) * (now - before);
            }
        }
        heard.level = std::sqrt(energy / 44100.0);
        heard.brightness = energy > 0 ? change / energy : 0;
        dx::audio::stopSound(id);
        dx::audio::render(buffer.data(), 4096);
        return heard;
    }
}

TEST_CASE("in the raytraced mode a source behind a wall is quieter and duller than in the open")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    auto open = std::make_shared<Geometry>();
    // Only a floor, below both.
    size_t floor = open->surfaceIndex({ "Concrete", "Floor", 0, false });
    open->addPolygon({ { -3000, -3000, -200 }, { 3000, -3000, -200 }, { 3000, 3000, -200 }, { -3000, 3000, -200 } }, { 0, 0, 1 }, floor);
    auto walled = std::make_shared<Geometry>(*open);
    addWall(*walled, 200, 20, { "Concrete", "Wall", 0, false });
    Heard inOpen = hear(open, { 400, 0, 0 });
    Heard behind = hear(walled, { 400, 0, 0 });
    std::printf("open: level %.4f brightness %.3f occlusion %.2f | behind concrete: level %.4f brightness %.3f occlusion %.2f transmission %.3f %.3f %.3f\n", inOpen.level, inOpen.brightness, static_cast<double>(inOpen.status.occlusion), behind.level, behind.brightness, static_cast<double>(behind.status.occlusion), static_cast<double>(behind.status.transmission[0]), static_cast<double>(behind.status.transmission[1]), static_cast<double>(behind.status.transmission[2]));
    REQUIRE(inOpen.status.simulated);
    REQUIRE(behind.status.simulated);
    CHECK(inOpen.status.occlusion == Catch::Approx(1.0f).margin(0.01));
    CHECK(behind.status.occlusion < 0.1f);
    CHECK(behind.status.transmission[2] < behind.status.transmission[0]);
    CHECK(behind.level < inOpen.level * 0.3);
    CHECK(behind.brightness < inOpen.brightness * 0.8);
    // Galaxy's line check and its floor of a third are not used in this mode.
    CHECK(behind.status.obstruction == 0.0f);
}

TEST_CASE("in the raytraced mode a source behind a window is less muffled than behind a wall, and the table changes it live")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    constexpr uint32_t translucent = 0x4;
    auto window = std::make_shared<Geometry>();
    addWall(*window, 200, 0, { "Glass", "WindowPane", translucent | dx::bsp::PF_NotSolid, false });
    auto wall = std::make_shared<Geometry>();
    addWall(*wall, 200, 20, { "Concrete", "Wall", 0, false });
    REQUIRE(window->triangleMaterial[0] == dx::audio::acoustics::materialIndex("glass"));
    Heard throughWindow = hear(window, { 400, 0, 0 });
    Heard throughWall = hear(wall, { 400, 0, 0 });
    std::printf("behind glass: level %.4f transmission %.3f %.3f %.3f | behind concrete: level %.4f transmission %.3f %.3f %.3f\n", throughWindow.level, static_cast<double>(throughWindow.status.transmission[0]), static_cast<double>(throughWindow.status.transmission[1]), static_cast<double>(throughWindow.status.transmission[2]), throughWall.level, static_cast<double>(throughWall.status.transmission[0]), static_cast<double>(throughWall.status.transmission[1]), static_cast<double>(throughWall.status.transmission[2]));
    CHECK(throughWindow.status.occlusion < 0.1f);
    CHECK(throughWindow.level > throughWall.level * 2);
    // Glass made opaque in the table, while the window's scene stays built.
    hear(window, { 400, 0, 0 });
    std::vector<dx::audio::acoustics::Material> table = dx::audio::acousticMaterials();
    table[static_cast<size_t>(dx::audio::acoustics::materialIndex("glass"))].transmission = { 0.0f, 0.0f, 0.0f };
    dx::audio::setAcousticMaterials(table);
    Heard sealed = hear(nullptr, { 400, 0, 0 });
    CHECK(sealed.level < throughWindow.level * 0.2);
    dx::audio::setAcousticMaterials(dx::audio::acoustics::defaultMaterials());
}

TEST_CASE("a mover's Steam Audio transform places its shape where the renderer draws it")
{
    dx::audio::acoustics::MoverPose pose;
    pose.location = { 100, -50, 30 };
    pose.rotation = { 1200, 9000, -400 };
    pose.scale = { 1.5f, 0.5f, 2.0f };
    pose.prePivot = { 8, 16, -4 };
    std::array<std::array<float, 4>, 4> m = dx::audio::acoustics::steamTransform(pose);
    for (const dx::Vector& local : { dx::Vector { 0, 0, 0 }, dx::Vector { 64, -32, 16 }, dx::Vector { -10, 200, 5 } }) {
        std::array<float, 3> in = dx::audio::acoustics::toSteam(local);
        std::array<float, 3> expected = dx::audio::acoustics::toSteam(dx::audio::acoustics::placePoint(pose, local));
        for (size_t row = 0; row < 3; row++) {
            float out = m[row][0] * in[0] + m[row][1] * in[1] + m[row][2] * in[2] + m[row][3];
            CHECK(out == Catch::Approx(expected[row]).margin(1e-4));
        }
    }
}

namespace {
    // A door slab in its own space, centred on its origin: thin in x, half sizes in units.
    std::shared_ptr<Geometry> slab(float halfY, float halfZ)
    {
        auto shape = std::make_shared<Geometry>();
        addBox(*shape, {}, { 10, halfY, halfZ }, { "Metal", "Door", 0, false }, false);
        return shape;
    }
}

TEST_CASE("in the raytraced mode a mover opening and turning changes occlusion")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    auto scene = std::make_shared<Geometry>();
    scene->movers.push_back({ 7, slab(1000, 1000) });
    dx::audio::setAcousticGeometry(scene);
    dx::audio::acoustics::MoverPose closed;
    closed.id = 7;
    closed.location = { 200, 0, 0 };
    dx::audio::setAcousticMoverPoses({ closed });
    Heard shut = hear(nullptr, { 400, 0, 0 });
    // Slid 3000 units aside, as a door opening into its frame.
    dx::audio::acoustics::MoverPose open = closed;
    open.location = { 200, 3000, 0 };
    dx::audio::setAcousticMoverPoses({ open });
    Heard opened = hear(nullptr, { 400, 0, 0 });
    // Destroyed, as a broken DeusExMover is: left out of the scene.
    dx::audio::setAcousticMoverPoses({ closed });
    Heard reshut = hear(nullptr, { 400, 0, 0 });
    dx::audio::setAcousticMoverPoses({});
    Heard gone = hear(nullptr, { 400, 0, 0 });
    std::printf("door shut %.2f, open %.2f, shut again %.2f, destroyed %.2f\n", static_cast<double>(shut.status.occlusion), static_cast<double>(opened.status.occlusion), static_cast<double>(reshut.status.occlusion), static_cast<double>(gone.status.occlusion));
    CHECK(shut.status.occlusion < 0.1f);
    CHECK(opened.status.occlusion > 0.9f);
    CHECK(reshut.status.occlusion < 0.1f);
    CHECK(gone.status.occlusion > 0.9f);
    CHECK(opened.level > shut.level * 3);

    // A narrow slab turned an eighth (yaw 8192) hides a source on one side of the listener's
    // line and not its mirror image: right only if the turn is applied, not its inverse.
    auto narrow = std::make_shared<Geometry>();
    narrow->movers.push_back({ 8, slab(150, 1000) });
    dx::audio::setAcousticGeometry(narrow);
    dx::audio::acoustics::MoverPose turned;
    turned.id = 8;
    turned.location = { 300, 0, 0 };
    turned.rotation.yaw = 8192;
    dx::audio::setAcousticMoverPoses({ turned });
    Heard hidden = hear(nullptr, { 600, 212, 0 });
    Heard mirror = hear(nullptr, { 600, -212, 0 });
    std::printf("turned slab: source at +y occlusion %.2f, at -y %.2f\n", static_cast<double>(hidden.status.occlusion), static_cast<double>(mirror.status.occlusion));
    CHECK(hidden.status.occlusion < 0.3f);
    CHECK(mirror.status.occlusion > 0.8f);
}

namespace {
    struct Decay {
        std::array<float, 3> times {};
        // Output energy from a tenth to half a second after the sound stops, over the energy
        // while it played.
        double tail = 0;
    };

    // Noise for half a second from beside a listener at the origin in room, then silence.
    Decay decayIn(std::shared_ptr<const Geometry> room, bool placed = true)
    {
        dx::audio::setAcousticGeometry(std::move(room));
        dx::USound noise;
        noise.bytes = noiseWave();
        dx::audio::update(nullptr, {}, {}, 0.0f);
        int32_t id = dx::audio::playSound(&noise, nullptr, dx::audio::SlotNone, { 60, 30, 0 }, 1.0f, 4000.0f, 1.0f, placed);
        REQUIRE(id != 0);
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::settleAcoustics();
        std::vector<float> playing(22050 * 2);
        dx::audio::render(playing.data(), 22050);
        dx::audio::stopSound(id);
        std::vector<float> after(22050 * 2);
        dx::audio::render(after.data(), 22050);
        auto energy = [](const std::vector<float>& samples, size_t from, size_t to) {
            double sum = 0;
            for (size_t i = from * 2; i < to * 2; i++) {
                sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
            }
            return sum;
        };
        Decay decay;
        decay.times = dx::audio::acousticStatus().simulation.reverbTimes;
        decay.tail = energy(after, 4410, 22050) / std::max(energy(playing, 0, 22050), 1e-12);
        return decay;
    }
}

TEST_CASE("in the raytraced mode reverb decays longer in a large hard room than a small soft one")
{
    RaytracedMixer mixer;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    using dx::audio::acoustics::UnitsPerMetre;
    auto hall = std::make_shared<Geometry>();
    addBox(*hall, {}, { 20 * UnitsPerMetre, 15 * UnitsPerMetre, 5 * UnitsPerMetre }, { "Concrete", "Hall", 0, false }, true);
    auto booth = std::make_shared<Geometry>();
    addBox(*booth, {}, { 2 * UnitsPerMetre, 2 * UnitsPerMetre, 1.5f * UnitsPerMetre }, { "Textile", "Carpet", 0, false }, true);
    Decay large = decayIn(hall);
    Decay small = decayIn(booth);
    std::printf("hall 40x30x10 m concrete: RT60 %.2f %.2f %.2f s, tail %.4f | booth 4x4x3 m carpet: RT60 %.2f %.2f %.2f s, tail %.6f\n", static_cast<double>(large.times[0]), static_cast<double>(large.times[1]), static_cast<double>(large.times[2]), large.tail, static_cast<double>(small.times[0]), static_cast<double>(small.times[1]), static_cast<double>(small.times[2]), small.tail);
    CHECK(large.times[1] > 1.0f);
    CHECK(small.times[1] < 0.5f);
    CHECK(large.times[1] > small.times[1] * 4);
    CHECK(large.tail > small.tail * 10);
    // Unplaced sounds (the interface) stay dry, as in every mode, and music too.
    Decay unplaced = decayIn(hall, false);
    CHECK(unplaced.tail < large.tail * 0.001);
    // The zone's authored reverb instead, for comparison: none in this scene, so no tail.
    dx::audio::ReverbControls controls = dx::audio::reverbControls();
    controls.authored = true;
    dx::audio::setReverbControls(controls);
    Decay authored = decayIn(hall);
    CHECK(authored.tail < large.tail * 0.01);
}

TEST_CASE("Unreal units become metres at Deus Ex's 16 to the foot, on Steam Audio's axes")
{
    std::array<float, 3> point = dx::audio::acoustics::toSteam({ 52.4934f, 104.9868f, 157.4802f });
    CHECK(point[0] == Catch::Approx(1.0f).epsilon(1e-5));
    // Unreal's z (up) is Steam Audio's y, and Unreal's y its z.
    CHECK(point[1] == Catch::Approx(3.0f).epsilon(1e-5));
    CHECK(point[2] == Catch::Approx(2.0f).epsilon(1e-5));
    CHECK(dx::audio::acoustics::UnitsPerMetre == Catch::Approx(16.0 / 0.3048).epsilon(1e-6));
}

TEST_CASE("surfaces resolve to materials by group, flags and name, and the non-physical are left out")
{
    using dx::bsp::PF_FakeBackdrop;
    using dx::bsp::PF_Invisible;
    using dx::bsp::PF_Masked;
    using dx::bsp::PF_NotSolid;
    using dx::bsp::PF_Portal;
    constexpr uint32_t translucent = 0x4;
    CHECK(materialOf({ "Metal", "Any", 0, false }) == "metal");
    CHECK(materialOf({ "Ladder", "Any", 0, false }) == "metal");
    CHECK(materialOf({ "Stucco", "Any", 0, false }) == "plaster");
    // The Glass group is glass only when drawn see-through; its light fittings are not.
    CHECK(materialOf({ "Glass", "WindowPane", translucent | PF_NotSolid, false }) == "glass");
    CHECK(materialOf({ "Glass", "un_insetlight_b", 0, false }) == "fixture");
    CHECK(materialOf({ "Metal", "ChainFence", PF_Masked, false }) == "grate");
    // Mixed groups go by the texture's name, then to generic.
    CHECK(materialOf({ "HK_Helibase", "HK_WoodFloor_A", 0, false }) == "wood");
    CHECK(materialOf({ "CoreTexMisc", "Something", 0, false }) == "generic");
    CHECK(materialOf({ "Misc", "LightBeam", translucent | PF_NotSolid, false }) == "excluded");
    CHECK(dx::audio::acoustics::resolve({ "Metal", "x", PF_FakeBackdrop, false }).exclusion == Exclusion::Backdrop);
    CHECK(dx::audio::acoustics::resolve({ "Sky", "x", 0, false }).exclusion == Exclusion::Backdrop);
    CHECK(dx::audio::acoustics::resolve({ "Metal", "x", PF_Portal, false }).exclusion == Exclusion::Portal);
    CHECK(dx::audio::acoustics::resolve({ "Metal", "x", PF_Invisible, false }).exclusion == Exclusion::Invisible);
    CHECK(dx::audio::acoustics::resolve({ "Metal", "x", 0, true }).exclusion == Exclusion::SkyZone);
}

TEST_CASE("the material rules place windows, fittings, ceilings and markers as the levels use them")
{
    // Frosted windows in the Glass group are drawn opaque, and still glass.
    CHECK(materialOf({ "Glass", "07WindOpacStrek", 0, false }) == "glass");
    CHECK(materialOf({ "Glass", "pa_sqrlight", 0, false }) == "fixture");
    // Per-location groups go by name: light fittings, pipes, painted concrete.
    CHECK(materialOf({ "MJ12_lab", "un_mj12light_b", 0, false }) == "fixture");
    CHECK(materialOf({ "HK_Helibase", "Pipe_B", 0, false }) == "metal");
    CHECK(materialOf({ "V_Com_Center", "ClenCncrtWall_H", 0, false }) == "concrete");
    // Overrides for the commonest textures their group or name would misplace.
    CHECK(materialOf({ "Misc", "Uob_Floor_A", 0, false }) == "concrete");
    CHECK(materialOf({ "Wall_Objects", "ClenCmandWall_J", 0, false }) == "concrete");
    CHECK(materialOf({ "Tiles", "ClenGrayCeiln_A", 0, false }) == "ceiling");
    CHECK(materialOf({ "Tiles", "INDUgraytile", 0, false }) == "tiles");
    CHECK(materialOf({ "Paper", "ClenWhitPaint_A", 0, false }) == "paper");
    CHECK(dx::audio::acoustics::resolve({ "CoreTexMisc", "Marker_sky", 0, false }).exclusion == Exclusion::Invisible);
    // The table copies out as the code's own lines.
    std::string text = dx::audio::acoustics::materialTableText(dx::audio::acoustics::defaultMaterials());
    CHECK(text.rfind("{ \"generic\", { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.300f, 0.200f, 0.120f } },\n", 0) == 0);
    CHECK(text.find("{ \"metal\", { 0.20f, 0.07f, 0.06f }, 0.05f, { 0.300f, 0.200f, 0.120f } },\n") != std::string::npos);
}

TEST_CASE("the crosshair pick finds the nearest triangle on its ray")
{
    Geometry geometry;
    size_t near = geometry.surfaceIndex({ "Wood", "Near", 0, false });
    size_t far = geometry.surfaceIndex({ "Metal", "Far", 0, false });
    geometry.addPolygon({ { 200, -50, -50 }, { 200, 50, -50 }, { 200, 50, 50 }, { 200, -50, 50 } }, { -1, 0, 0 }, far);
    geometry.addPolygon({ { 100, -50, -50 }, { 100, 50, -50 }, { 100, 50, 50 }, { 100, -50, 50 } }, { -1, 0, 0 }, near);
    float distance = 0;
    int32_t hit = dx::audio::acoustics::pickTriangle(geometry, { 0, 10, 5 }, { 1, 0, 0 }, 1000.0f, distance);
    REQUIRE(hit >= 0);
    CHECK(geometry.triangleSurface[static_cast<size_t>(hit)] == static_cast<int32_t>(near));
    CHECK(distance == Catch::Approx(100.0f));
    CHECK(dx::audio::acoustics::pickTriangle(geometry, { 0, 10, 5 }, { -1, 0, 0 }, 1000.0f, distance) == -1);
}

TEST_CASE("a polygon becomes a fan of triangles wound for Steam Audio's front")
{
    Geometry geometry;
    size_t surface = geometry.surfaceIndex({ "Concrete", "Floor", 0, false });
    // A floor square facing up (Unreal +z).
    geometry.addPolygon({ { 0, 0, 0 }, { 100, 0, 0 }, { 100, 100, 0 }, { 0, 100, 0 } }, { 0, 0, 1 }, surface);
    REQUIRE(geometry.triangles.size() == 2);
    CHECK(geometry.triangleCounts[0] == 2);
    for (const auto& triangle : geometry.triangles) {
        auto a = dx::audio::acoustics::toSteam(geometry.vertices[static_cast<size_t>(triangle[0])]);
        auto b = dx::audio::acoustics::toSteam(geometry.vertices[static_cast<size_t>(triangle[1])]);
        auto c = dx::audio::acoustics::toSteam(geometry.vertices[static_cast<size_t>(triangle[2])]);
        // Counter-clockwise in Steam Audio's space faces +y, its up.
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        float normalY = uz * vx - ux * vz;
        CHECK(normalY > 0);
        (void)uy;
        (void)vy;
    }
    size_t sky = geometry.surfaceIndex({ "Sky", "Clouds", 0, false });
    geometry.addPolygon({ { 0, 0, 500 }, { 100, 0, 500 }, { 100, 100, 500 } }, { 0, 0, -1 }, sky);
    CHECK(geometry.triangles.size() == 2);
    CHECK(geometry.triangleCounts[static_cast<size_t>(Exclusion::Backdrop)] == 1);
}

TEST_CASE("the exported scene agrees with the map survey's surface counts", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    for (const Survey& survey : surveys) {
        CAPTURE(survey.map);
        dx::objects::unloadPackage(dx::Name(survey.map));
        dx::World world;
        REQUIRE(world.loadMap(survey.map));
        REQUIRE(world.level);
        dx::UModel* model = world.level->model;
        REQUIRE(model);
        Geometry geometry = dx::audio::acoustics::levelGeometry(world.level);
        CHECK(geometry.modelSurfaces == survey.surfaces);
        // Every model surface by group, as the survey counted them.
        std::map<std::string, size_t> groups;
        for (const dx::BspSurf& surf : model->surfs) {
            groups[surf.texture && surf.texture->outer ? surf.texture->outer->name.str() : ""]++;
        }
        for (const auto& [group, count] : survey.groups) {
            CAPTURE(group);
            CHECK(groups[group] == count);
        }
        // Every BSP node's polygon is a triangle fan, kept or counted as left out.
        size_t fans = 0;
        for (const dx::BspNode& node : model->nodes) {
            if (node.numVertices >= 3 && node.surf >= 0 && static_cast<size_t>(node.surf) < model->surfs.size()) {
                fans += node.numVertices - 2u;
            }
        }
        size_t counted = 0;
        for (size_t count : geometry.triangleCounts) {
            counted += count;
        }
        CHECK(counted == fans);
        CHECK(geometry.triangles.size() == geometry.triangleCounts[0]);
        CHECK(geometry.triangles.size() > fans / 2);
        std::printf("%s: %zu surfaces, %zu triangles kept of %zu; left out: backdrop %zu, portal %zu, invisible %zu, skybox %zu, effects %zu\n", survey.map, geometry.modelSurfaces, geometry.triangles.size(), fans, geometry.triangleCounts[1], geometry.triangleCounts[2], geometry.triangleCounts[3], geometry.triangleCounts[4], geometry.triangleCounts[5]);
        dx::objects::unloadPackage(dx::Name(survey.map));
    }
}

TEST_CASE("the raytraced mode builds the level's scene on its worker and frees it with no level", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot settings;
    dx::config::system().set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaRaytracedAudio", "True");
    dx::test::OfflineAudio audio;
    if (!dx::audio::raytracedActive()) {
        SKIP("Steam Audio's simulation is not available");
    }
    CHECK(dx::audio::hrtfActive());
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    {
        dx::World world;
        REQUIRE(world.loadMap("01_NYC_UNATCOHQ"));
        dx::audio::update(&world, {}, {}, 0.0f);
        dx::audio::settleAcoustics();
        std::shared_ptr<const Geometry> geometry = dx::audio::acousticGeometry();
        REQUIRE(geometry);
        dx::audio::AcousticStatus status = dx::audio::acousticStatus();
        CHECK(status.simulation.scene);
        // The survey counts 27 movers in UNATCO HQ, all with brushes.
        CHECK(geometry->movers.size() == 27);
        CHECK(status.simulation.movers == 27);
        size_t moverTriangles = 0;
        for (const auto& mover : geometry->movers) {
            moverTriangles += mover.shape->triangles.size();
        }
        CHECK(status.simulation.triangles == geometry->triangles.size() + moverTriangles);
        std::printf("01_NYC_UNATCOHQ: exported in %.1f ms, scene built in %.1f ms with %s\n", status.exportMilliseconds, status.simulation.buildMilliseconds, status.simulation.embree ? "Embree" : "the built-in ray tracer");
        dx::audio::update(nullptr, {}, {}, 0.0f);
        dx::audio::settleAcoustics();
        CHECK_FALSE(dx::audio::acousticGeometry());
        CHECK(dx::audio::acousticStatus().simulation.triangles == 0);
    }
    dx::objects::unloadPackage(dx::Name("01_NYC_UNATCOHQ"));
    audio.shutdown();
}
