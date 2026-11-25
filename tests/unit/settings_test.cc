#include "core/settings.h"
#include "fixtures.h"
#include "test_paths.h"
#include "core/objects.h"
#include "deusex/deusex.h"
#include "deusex/startup.h"
#include "engine/console.h"
#include "engine/resources.h"
#include "game/menu_options.h"

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace settings = dx::settings;

namespace {
    // Empty system and user inis saved in a folder of the test's own, put back at the end.
    struct EmptySettings {
        EmptySettings()
        {
            dx::config::system() = dx::Ini();
            dx::config::system().path = folder / "DeusEx.ini";
            dx::config::user() = dx::Ini();
            dx::config::user().path = folder / "User.ini";
        }

        // What a fresh load of the saved system ini holds for a key.
        std::optional<std::string> saved(std::string_view section, std::string_view key) const
        {
            dx::Ini ini;
            ini.load(folder / "DeusEx.ini");
            return ini.get(section, key);
        }

        dx::test::ConfigSnapshot snapshot;
        dx::test::TempDir folder { "settings" };
    };

    std::vector<std::string> heardChanges(const std::function<void()>& write)
    {
        std::vector<std::string> heard;
        dx::Subscription watch = dx::config::onSettingChanged([&](const dx::config::SettingChange& change) {
            heard.push_back(std::string(change.section) + " " + std::string(change.key));
        });
        write();
        return heard;
    }
}

TEST_CASE("a bool reads True, False, T, F, 1 and 0 in any case and nothing else", "[settings]")
{
    for (const char* text : { "True", "true", "TRUE", "tRuE", "T", "t", "1" }) {
        CHECK(settings::parseBool(text) == std::optional<bool>(true));
    }
    for (const char* text : { "False", "false", "FALSE", "F", "f", "0" }) {
        CHECK(settings::parseBool(text) == std::optional<bool>(false));
    }
    for (const char* text : { "", "yes", "no", "on", "off", "2", "Truth", " True" }) {
        CHECK_FALSE(settings::parseBool(text).has_value());
    }
    CHECK(settings::format(true) == "True");
    CHECK(settings::format(false) == "False");
}

TEST_CASE("numbers read as plain decimals, a slider's decimals as a whole number", "[settings]")
{
    CHECK(settings::parseInt("16") == std::optional<int>(16));
    CHECK(settings::parseInt("-3") == std::optional<int>(-3));
    CHECK(settings::parseInt("16.000000") == std::optional<int>(16));
    CHECK(settings::parseInt("7.9") == std::optional<int>(7));
    for (const char* text : { "", "abc", "3x", "True", "16.0.0" }) {
        CHECK_FALSE(settings::parseInt(text).has_value());
    }
    CHECK(settings::parseFloat("0.600000") == std::optional<float>(0.6f));
    CHECK(settings::parseFloat("153") == std::optional<float>(153.0f));
    for (const char* text : { "", "x", "0.5x", "nan", "inf" }) {
        CHECK_FALSE(settings::parseFloat(text).has_value());
    }
    CHECK(settings::format(0.6f) == "0.600000");
    CHECK(settings::format(12) == "12");
}

TEST_CASE("each setting is declared once, with its default in its range", "[settings]")
{
    std::set<std::string> places;
    for (const settings::Setting* setting : settings::all()) {
        std::string place = std::to_string(static_cast<int>(setting->section.file)) + " " + std::string(setting->section.name) + " " + std::string(setting->key);
        CHECK(places.insert(place).second);
        for (dx::platform::Kind platform : { dx::platform::Kind::Desktop, dx::platform::Kind::Switch }) {
            double value = settings::defaultNumber(*setting, platform);
            CHECK(value >= setting->minimum);
            CHECK(value <= setting->maximum);
        }
    }
    // The original's choices that Ambrosia has no use for, whose rows the options screens lose,
    // and keys nothing reads any more.
    std::set<std::string> ignored;
    for (const settings::Setting* setting : settings::all()) {
        if (setting->support == settings::Support::Ignored) {
            ignored.emplace(setting->key);
        }
    }
    for (const char* key : { "FullscreenColorBits", "WindowedColorBits", "TextureDetail", "SkinDetail", "Decals", "DetailTextures", "OutputRate", "LowSoundQuality", "ReverseStereo", "UseSurround", "Use3dHardware", "Latency", "MipFactor", "CdPath" }) {
        CHECK(ignored.count(key) == 1);
    }
    // Keys a script config class declares are not.
    for (const char* key : { "NetworkDevice", "Language", "UseTrilinear" }) {
        CHECK(ignored.count(key) == 0);
    }
}

TEST_CASE("defaults are the original's, and the button prompts' differ on the Switch", "[settings]")
{
    EmptySettings empty;
    CHECK(settings::get(settings::brightness) == 0.6f);
    CHECK(settings::get(settings::musicVolume) == 153.0f);
    CHECK(settings::get(settings::soundVolume) == 204.0f);
    CHECK(settings::get(settings::speechVolume) == 255.0f);
    CHECK(settings::get(settings::port) == 7790);
    CHECK(settings::get(settings::startupFullscreen));
    CHECK(settings::get(settings::localMap) == "DX.dx");

    using dx::platform::Kind;
    CHECK(settings::defaultNumber(settings::gamepadGlyphs.setting, Kind::Desktop) == dx::game::autoButtonPrompts);
    CHECK(settings::defaultNumber(settings::gamepadGlyphs.setting, Kind::Switch) == 2);
    CHECK(settings::defaultNumber(settings::fieldOfViewIndex.setting, Kind::Desktop) == settings::defaultNumber(settings::fieldOfViewIndex.setting, Kind::Switch));
    CHECK(settings::get(settings::gamepadGlyphs) == static_cast<int>(settings::defaultNumber(settings::gamepadGlyphs.setting, dx::platform::kind())));
    // The menus' table takes the button prompts' default from the setting.
    CHECK(dx::game::menuOptionsFor(dx::game::Platform::Desktop).buttonPrompts.value == 3);
    CHECK(dx::game::menuOptionsFor(dx::game::Platform::Switch).buttonPrompts.value == 2);
}

TEST_CASE("a setting reads clamped, and as its default when the ini's value is not of its type", "[settings]")
{
    EmptySettings empty;
    dx::Ini& ini = dx::config::system();
    ini.set("WinDrv.WindowsClient", "FieldOfViewIndex", "20");
    CHECK(settings::get(settings::fieldOfViewIndex) == 12);
    ini.set("WinDrv.WindowsClient", "FieldOfViewIndex", "-5");
    CHECK(settings::get(settings::fieldOfViewIndex) == 0);
    ini.set("WinDrv.WindowsClient", "FieldOfViewIndex", "wide");
    CHECK(settings::get(settings::fieldOfViewIndex) == 3);
    CHECK_FALSE(settings::stored(settings::fieldOfViewIndex.setting));
    ini.set("WinDrv.WindowsClient", "Brightness", "2.5");
    CHECK(settings::get(settings::brightness) == 1.0f);
    // A hand-edited true is on, and so is a 1; a yes is not a bool, so the default.
    ini.set("WinDrv.WindowsClient", "UseTrilinear", "true");
    CHECK(settings::get(settings::useTrilinear));
    ini.set("WinDrv.WindowsClient", "UseTrilinear", "1");
    CHECK(settings::get(settings::useTrilinear));
    ini.set("WinDrv.WindowsClient", "UseTrilinear", "yes");
    CHECK_FALSE(settings::get(settings::useTrilinear));
    ini.set("Engine.GameEngine", "GarbageCollection", "0");
    CHECK_FALSE(settings::get(settings::garbageCollection));
    // Empty is no value, so the on-unless-off settings stay on.
    ini.set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaPathing", "");
    CHECK(settings::get(settings::pathing));
    ini.set("Galaxy.GalaxyAudioSubsystem", "AmbrosiaPathing", "False");
    CHECK_FALSE(settings::get(settings::pathing));
    ini.set("Galaxy.GalaxyAudioSubsystem", "EffectsChannels", "8.000000");
    CHECK(settings::get(settings::effectsChannels) == 8);
}

TEST_CASE("a section named by Engine.Engine falls back to the original's when it names none", "[settings]")
{
    EmptySettings empty;
    dx::Ini& ini = dx::config::system();
    CHECK(settings::section(settings::video) == "WinDrv.WindowsClient");
    CHECK(settings::section(settings::audio) == "Galaxy.GalaxyAudioSubsystem");
    CHECK(settings::section(settings::renderDevice) == "GlideDrv.GlideRenderDevice");
    ini.set("Galaxy.GalaxyAudioSubsystem", "MusicVolume", "51");
    CHECK(settings::get(settings::musicVolume) == 51.0f);
    ini.set("Engine.Engine", "AudioDevice", "");
    CHECK(settings::section(settings::audio) == "Galaxy.GalaxyAudioSubsystem");
    ini.set("Engine.Engine", "AudioDevice", "Other.AudioSubsystem");
    CHECK(settings::section(settings::audio) == "Other.AudioSubsystem");
    CHECK(settings::get(settings::musicVolume) == 153.0f);
    ini.set("Engine.Engine", "ViewportManager", "SDLDrv.SDLClient");
    ini.set("SDLDrv.SDLClient", "VSync", "0");
    CHECK(settings::get(settings::vsync) == 0);
    CHECK(settings::iniPathSection("Engine.Engine.ViewportManager") == "SDLDrv.SDLClient");
    CHECK(settings::iniPathSection("Engine.Engine.Nothing").empty());
    CHECK(settings::iniPathSection("NoDot").empty());
}

TEST_CASE("get ini: answers a setting's default when the ini has none", "[settings]")
{
    EmptySettings empty;
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager FieldOfViewIndex") == "3");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager UseTrilinear") == "False");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager GamepadAimAssist") == "True");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager Brightness") == "0.600000");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.AudioDevice MusicVolume") == "153.000000");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager NoSuchKey").empty());
    dx::Ini& ini = dx::config::system();
    // What Ambrosia reads: a bool as the script's on/off choices compare it, a number out of range
    // clamped, one in range as the ini spells it.
    ini.set("WinDrv.WindowsClient", "FXAA", "true");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager FXAA") == "True");
    ini.set("WinDrv.WindowsClient", "FieldOfViewIndex", "99");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager FieldOfViewIndex") == "12");
    ini.set("Galaxy.GalaxyAudioSubsystem", "EffectsChannels", "8.000000");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.AudioDevice EffectsChannels") == "8.000000");
    // Keys the table does not have read as they are.
    ini.set("WinDrv.WindowsClient", "SomethingNew", "1.0");
    CHECK(dx::console::execute(nullptr, "get ini:Engine.Engine.ViewportManager SomethingNew") == "1.0");
}

TEST_CASE("settings::set saves the ini and announces each setting", "[settings]")
{
    EmptySettings empty;
    std::vector<std::string> heard = heardChanges([] { CHECK(settings::set(settings::fieldOfViewIndex, 5)); });
    CHECK(heard == std::vector<std::string> { "WinDrv.WindowsClient FieldOfViewIndex" });
    CHECK(empty.saved("WinDrv.WindowsClient", "FieldOfViewIndex") == "5");
    CHECK(settings::get(settings::fieldOfViewIndex) == 5);
    // Clamped, and a bool as True or False.
    CHECK(settings::set(settings::fieldOfViewIndex, 40));
    CHECK(empty.saved("WinDrv.WindowsClient", "FieldOfViewIndex") == "12");
    CHECK(settings::set(settings::gamepadGyro, true));
    CHECK(empty.saved("WinDrv.WindowsClient", "GamepadGyro") == "True");
    // Several at once: one save, a notice each, in the ini each is in.
    heard = heardChanges([] { CHECK(settings::set({ settings::to(settings::windowedViewportX, 1280), settings::to(settings::startupFullscreen, false), settings::to(settings::playerClass, "DeusEx.JCDentonFemale") })); });
    CHECK(heard == std::vector<std::string> { "WinDrv.WindowsClient WindowedViewportX", "WinDrv.WindowsClient StartupFullscreen", "DefaultPlayer Class" });
    CHECK(empty.saved("WinDrv.WindowsClient", "WindowedViewportX") == "1280");
    CHECK(empty.saved("WinDrv.WindowsClient", "StartupFullscreen") == "False");
    dx::Ini user;
    user.load(empty.folder / "User.ini");
    CHECK(user.get("DefaultPlayer", "Class") == "DeusEx.JCDentonFemale");
    // A save that fails says so, and the value is announced, as it is in effect.
    dx::config::system().path = empty.folder / "missing" / "DeusEx.ini";
    heard = heardChanges([] { CHECK_FALSE(settings::set(settings::vsync, 0)); });
    CHECK(heard == std::vector<std::string> { "WinDrv.WindowsClient VSync" });
    CHECK(settings::get(settings::vsync) == 0);
    CHECK_FALSE(dx::console::execute(nullptr, "set ini:Engine.Engine.ViewportManager VSync 2").empty());
}

TEST_CASE("set ini: goes through the one write path", "[settings]")
{
    EmptySettings empty;
    std::vector<std::string> heard = heardChanges([] { CHECK(dx::console::execute(nullptr, "set ini:Engine.Engine.AudioDevice OutputRate 22050Hz").empty()); });
    CHECK(heard == std::vector<std::string> { "Galaxy.GalaxyAudioSubsystem OutputRate" });
    CHECK(empty.saved("Galaxy.GalaxyAudioSubsystem", "OutputRate") == "22050Hz");
}

TEST_CASE("a menu choice's configSetting names its setting", "[settings]")
{
    EmptySettings empty;
    CHECK(settings::configSetting(settings::fieldOfViewIndex.setting) == "ini:Engine.Engine.ViewportManager FieldOfViewIndex");
    CHECK(settings::configSetting(settings::musicVolume.setting) == "ini:Engine.Engine.AudioDevice MusicVolume");
    CHECK(settings::configSetting(settings::garbageCollection.setting).empty());
    CHECK(settings::forConfigSetting("ini:Engine.Engine.ViewportManager FieldOfViewIndex") == &settings::fieldOfViewIndex.setting);
    CHECK(settings::forConfigSetting("ini:Engine.Engine.AudioDevice OutputRate") == &settings::outputRate.setting);
    CHECK(settings::forConfigSetting("INI:engine.engine.audiodevice use3dhardware") == &settings::use3dHardware.setting);
    CHECK(settings::forConfigSetting("ini:Engine.Engine.GameRenderDevice DetailTextures") == &settings::detailTextures.setting);
    CHECK(settings::forConfigSetting("ini:Engine.Engine.ViewportManager SomethingNew") == nullptr);
    CHECK(settings::forConfigSetting("Engine.Engine.ViewportManager Brightness") == nullptr);
    CHECK(settings::forConfigSetting("") == nullptr);
}

TEST_CASE("a saved ini leaves out what nothing reads and keeps what the table does not know", "[settings]")
{
    EmptySettings empty;
    dx::Ini ini;
    ini.path = empty.folder / "Saved.ini";
    ini.leaveOut = [](std::string_view section, std::string_view key) { return settings::leftOut(settings::File::System, section, key); };
    ini.set("Engine.Engine", "ViewportManager", "WinDrv.WindowsClient");
    ini.set("Engine.Engine", "Language", "int");
    ini.set("Engine.Engine", "CdPath", "..\\GOTY_1");
    ini.set("WinDrv.WindowsClient", "Brightness", "0.700000");
    ini.set("WinDrv.WindowsClient", "MipFactor", "1.000000");
    ini.set("WinDrv.WindowsClient", "TextureDetail", "High");
    ini.set("WinDrv.WindowsClient", "SomethingNew", "1");
    ini.set("Galaxy.GalaxyAudioSubsystem", "Latency", "40");
    ini.set("Galaxy.GalaxyAudioSubsystem", "MusicVolume", "153");
    ini.set("GlideDrv.GlideRenderDevice", "DetailTextures", "True");
    ini.set("OpenGLDrv.OpenGLRenderDevice", "UseTrilinear", "True");
    ini.set("IpDrv.TcpNetDriver", "MaxTicksPerSecond", "15");
    ini.set("URL", "Protocol", "deusex");
    ini.set("URL", "LocalMap", "DX.dx");
    ini.set("URL", "MapExt", "dx");
    REQUIRE(ini.save());
    // Still there to read until the next load.
    CHECK(ini.get("WinDrv.WindowsClient", "MipFactor") == "1.000000");
    dx::Ini saved;
    REQUIRE(saved.load(ini.path));
    CHECK(saved.get("Engine.Engine", "Language") == "int");
    CHECK_FALSE(saved.get("Engine.Engine", "CdPath").has_value());
    CHECK(saved.get("WinDrv.WindowsClient", "Brightness") == "0.700000");
    CHECK(saved.get("WinDrv.WindowsClient", "SomethingNew") == "1");
    for (const char* key : { "MipFactor", "TextureDetail" }) {
        CHECK_FALSE(saved.get("WinDrv.WindowsClient", key).has_value());
    }
    CHECK_FALSE(saved.get("Galaxy.GalaxyAudioSubsystem", "Latency").has_value());
    CHECK(saved.get("Galaxy.GalaxyAudioSubsystem", "MusicVolume") == "153");
    CHECK(saved.section("GlideDrv.GlideRenderDevice").empty());
    CHECK(saved.section("OpenGLDrv.OpenGLRenderDevice").empty());
    CHECK(saved.get("IpDrv.TcpNetDriver", "MaxTicksPerSecond") == "15");
    CHECK(saved.get("URL", "LocalMap") == "DX.dx");
    CHECK_FALSE(saved.get("URL", "Protocol").has_value());
    // A key left out reads as its default.
    dx::config::system() = saved;
    CHECK(settings::get(settings::detailTextures));
    CHECK(settings::valueText(settings::textureDetail.setting) == "High");
}

TEST_CASE("the first copy of the game's inis that Ambrosia saves leaves out what nothing reads", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::test::ConfigSnapshot snapshot;
    dx::test::TempDir folder("first-copy");
    for (auto [ini, name] : { std::pair { &dx::config::system(), "DeusEx.ini" }, std::pair { &dx::config::user(), "User.ini" } }) {
        dx::Ini copy = *ini;
        copy.path = folder / name;
        REQUIRE(copy.save());
    }
    dx::Ini system;
    REQUIRE(system.load(folder / "DeusEx.ini"));
    CHECK(system.section("GlideDrv.GlideRenderDevice").empty());
    CHECK(system.section("D3DDrv.D3DRenderDevice").empty());
    CHECK(system.section("DeusEx.DeusExGameEngine").empty());
    CHECK_FALSE(system.get("WinDrv.WindowsClient", "MipFactor").has_value());
    CHECK_FALSE(system.get("Galaxy.GalaxyAudioSubsystem", "OutputRate").has_value());
    CHECK(system.get("Engine.Engine", "GameRenderDevice").has_value());
    CHECK_FALSE(system.getAll("Editor.EditorEngine", "EditPackages").empty());
    CHECK_FALSE(system.section("IpDrv.TcpNetDriver").empty());
    dx::Ini user;
    REQUIRE(user.load(folder / "User.ini"));
    CHECK_FALSE(user.section("Engine.Input").empty());
    CHECK_FALSE(user.section("DeusEx.DeusExPlayer").empty());
}

namespace {
    std::vector<std::string> choiceNames(dx::UObject* screen)
    {
        std::vector<std::string> names;
        dx::UProperty* choices = screen->cls->findProperty(dx::Name("choices"));
        for (int i = 0; i < choices->arrayDim; i++) {
            dx::UObject* choice = *reinterpret_cast<dx::UObject**>(screen->propertyAddress(dx::Name("choices"), i));
            names.push_back(choice ? choice->name.str() : "");
        }
        return names;
    }

    int32_t& intField(dx::UObject* object, const char* name)
    {
        return *reinterpret_cast<int32_t*>(object->propertyAddress(dx::Name(name)));
    }

    // The screen's background art, a tile at a time.
    std::vector<std::vector<uint8_t>> art(dx::UObject* screen)
    {
        std::vector<std::vector<uint8_t>> tiles;
        int count = intField(screen, "textureCols") * intField(screen, "textureRows");
        for (int i = 0; i < count; i++) {
            auto* tile = dx::cast<dx::UTexture>(*reinterpret_cast<dx::UObject**>(screen->propertyAddress(dx::Name("clientTextures"), i)));
            tiles.push_back(tile ? tile->rgba(0) : std::vector<uint8_t>());
        }
        return tiles;
    }

    // Takes the ignored choices out of a copy of a screen, and checks it ends with the choices
    // given, `removed` rows shorter, with the art the rows from `first` cut out at once would leave.
    void checkIgnoredRemoved(const char* className, const std::vector<std::string>& kept, int first, int removed)
    {
        dx::UClass* screenClass = dx::objects::findClass(std::string("DeusEx.") + className);
        REQUIRE(screenClass != nullptr);
        dx::UObject* screen = dx::objects::createSubclass(screenClass, dx::Name(std::string(className) + "IgnoredTest"))->defaultObject;
        dx::UObject* expected = dx::objects::createSubclass(screenClass, dx::Name(std::string(className) + "IgnoredExpected"))->defaultObject;
        int32_t height = intField(screen, "ClientHeight");
        int32_t help = intField(screen, "helpPosY");
        int32_t gap = intField(screen, "choiceVerticalGap");
        int32_t start = intField(screen, "choiceStartY");

        dx::deusex::removeIgnoredChoices(screen);
        std::vector<std::string> names = choiceNames(screen);
        std::vector<std::string> wanted = kept;
        wanted.resize(names.size());
        CHECK(names == wanted);
        CHECK(intField(screen, "ClientHeight") == height - removed * gap);
        CHECK(intField(screen, "helpPosY") == help - removed * gap);

        dx::deusex::MenuArt band(expected, "the expected art");
        band.remove(start + first * gap, start + (first + removed) * gap);
        band.apply();
        CHECK(intField(screen, "textureRows") == intField(expected, "textureRows"));
        CHECK(art(screen) == art(expected));
        // A screen with nothing ignored in it is left as it is.
        dx::deusex::removeIgnoredChoices(screen);
        CHECK(choiceNames(screen) == names);
        CHECK(intField(screen, "ClientHeight") == height - removed * gap);
    }
}

TEST_CASE("the options screens lose the choices of settings Ambrosia ignores", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    REQUIRE(dx::objects::loadPackage(dx::Name("DeusEx")) != nullptr);
    dx::test::ConfigSnapshot snapshot;
    checkIgnoredRemoved("MenuScreenSound", { "MenuChoice_MusicVolume", "MenuChoice_SoundVolume", "MenuChoice_SpeechVolume", "MenuChoice_EffectsChannels" }, 4, 5);
    checkIgnoredRemoved("MenuScreenDisplay", { "MenuChoice_Brightness", "MenuChoice_FullScreen", "MenuChoice_RenderDevice", "MenuChoice_Resolution" }, 4, 5);
    // The Detail Textures choice is found through the render device the ini names.
    dx::config::system().set("Engine.Engine", "GameRenderDevice", "OpenGLDrv.OpenGLRenderDevice");
    CHECK(dx::settings::forConfigSetting("ini:Engine.Engine.GameRenderDevice DetailTextures") == &dx::settings::detailTextures.setting);
    // The other screens have nothing ignored.
    for (const char* name : { "MenuScreenOptions", "MenuScreenControls" }) {
        dx::UClass* screenClass = dx::objects::findClass(std::string("DeusEx.") + name);
        REQUIRE(screenClass != nullptr);
        dx::UObject* screen = dx::objects::createSubclass(screenClass, dx::Name(std::string(name) + "IgnoredTest"))->defaultObject;
        std::vector<std::string> before = choiceNames(screen);
        dx::deusex::removeIgnoredChoices(screen);
        CHECK(choiceNames(screen) == before);
    }
}
