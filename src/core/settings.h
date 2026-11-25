#pragma once

#include "core/config.h"
#include "core/platform.h"

#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The settings the engine and the game read and write in the ini files, each declared once
// below: where it is, its type, default and range, when a change to it applies and whether
// Ambrosia acts on it. Reading one goes through get, writing one through set, so the parsing, the
// default and the save are the same wherever it is used.
//
// UnrealScript class config (config::applyClassDefaults, SaveConfig) and the GetConfig native
// keep their own mechanism; this table is for the keys the C++ and the menus' ini: choices use.
namespace dx::settings {
    // The ini a setting is kept in: DeusEx.ini or User.ini (config::system, config::user).
    enum class File { System,
        User };

    enum class Type { Bool,
        Int,
        Float,
        String };

    // When a change takes effect: at once (read each time it is used, or re-read on its change
    // notice), the next time the thing it sets up is made (a map, a bake, a piece of music), or
    // only when the program starts again.
    enum class Applies { Live,
        NextUse,
        Restart };

    // Supported: one of Deus Ex's own keys, acted on. Ignored: one of Deus Ex's keys that Ambrosia
    // does not act on; the options screens leave out the choices that write one
    // (deusex::removeIgnoredChoices). AmbrosiaOnly: a key Ambrosia adds.
    enum class Support { Supported,
        Ignored,
        AmbrosiaOnly };

    struct Setting;

    // Where a setting is: a section of its own, or the section another setting names, as
    // [Engine.Engine] ViewportManager names the window's. The naming setting's default is the
    // section when the ini names none.
    struct Section {
        File file = File::System;
        std::string_view name;
        const Setting* namedBy = nullptr;
    };

    // A default that differs on the Switch.
    struct PerPlatform {
        double desktop = 0;
        double nintendoSwitch = 0;
    };

    struct Setting {
        Section section;
        std::string_view key;
        Type type = Type::Bool;
        // The default of a bool (0 or 1), int or float, and where the Switch's differs, its own.
        double number = 0;
        std::optional<double> switchNumber;
        // The default of a string.
        std::string_view text;
        // The range a bool, int or float is clamped to, when read and when written.
        double minimum = -std::numeric_limits<double>::infinity();
        double maximum = std::numeric_limits<double>::infinity();
        Applies applies = Applies::Live;
        Support support = Support::Supported;
    };

    // A setting with the C++ type it is read as.
    template <typename T>
    struct Typed {
        Setting setting;
    };

    using Bool = Typed<bool>;
    using Int = Typed<int>;
    using Float = Typed<float>;
    using String = Typed<std::string>;

    namespace declare {
        constexpr double unbounded = std::numeric_limits<double>::infinity();

        struct Range {
            double minimum = -unbounded;
            double maximum = unbounded;
        };

        constexpr Setting make(Section section, std::string_view key, Type type, Range range, Applies applies, Support support)
        {
            Setting setting;
            setting.section = section;
            setting.key = key;
            setting.type = type;
            setting.minimum = range.minimum;
            setting.maximum = range.maximum;
            setting.applies = applies;
            setting.support = support;
            return setting;
        }

        constexpr Bool boolean(Section section, std::string_view key, bool value, Applies applies, Support support)
        {
            Bool made { make(section, key, Type::Bool, { 0, 1 }, applies, support) };
            made.setting.number = value ? 1.0 : 0.0;
            return made;
        }

        constexpr Int integer(Section section, std::string_view key, int value, Range range, Applies applies, Support support)
        {
            Int made { make(section, key, Type::Int, range, applies, support) };
            made.setting.number = static_cast<double>(value);
            return made;
        }

        constexpr Int integer(Section section, std::string_view key, PerPlatform value, Range range, Applies applies, Support support)
        {
            Int made { make(section, key, Type::Int, range, applies, support) };
            made.setting.number = value.desktop;
            made.setting.switchNumber = value.nintendoSwitch;
            return made;
        }

        constexpr Float decimal(Section section, std::string_view key, double value, Range range, Applies applies, Support support)
        {
            Float made { make(section, key, Type::Float, range, applies, support) };
            made.setting.number = value;
            return made;
        }

        constexpr String text(Section section, std::string_view key, std::string_view value, Applies applies, Support support)
        {
            String made { make(section, key, Type::String, {}, applies, support) };
            made.setting.text = value;
            return made;
        }

        // Neither the engine nor scripts read this legacy key. Register it so saves omit it.
        constexpr String unused(Section section, std::string_view key)
        {
            return text(section, key, "", Applies::Live, Support::Ignored);
        }

        constexpr Section fixed(std::string_view name, File file = File::System)
        {
            Section section;
            section.file = file;
            section.name = name;
            return section;
        }

        constexpr Section namedBy(const String& setting)
        {
            Section section;
            section.name = setting.setting.key;
            section.namedBy = &setting.setting;
            return section;
        }
    }

    // The settings. Defaults are Deus Ex's own (Default.ini, DefUser.ini) where it has the key.
    // A new one goes here and into all() in settings.cc.
    namespace keys {
        using namespace declare;
        using enum Applies;
        using enum Support;

        // [Engine.Engine]: the classes and sections the engine is made of.
        inline constexpr Section engine = fixed("Engine.Engine");
        inline constexpr String viewportManager = text(engine, "ViewportManager", "WinDrv.WindowsClient", Live, Supported);
        inline constexpr String audioDevice = text(engine, "AudioDevice", "Galaxy.GalaxyAudioSubsystem", Live, Supported);
        // Read by the save screen's script (whether to take a snapshot) and naming the Detail
        // Textures choice's section.
        inline constexpr String gameRenderDevice = text(engine, "GameRenderDevice", "GlideDrv.GlideRenderDevice", NextUse, Supported);
        inline constexpr String console = text(engine, "Console", "Engine.Console", Restart, Supported);
        inline constexpr String defaultGame = text(engine, "DefaultGame", "DeusEx.DeusExGameInfo", NextUse, Supported);
        inline constexpr String root = text(engine, "Root", "DeusEx.DeusExRootWindow", Restart, Supported);
        // Threads a bake or an acoustics simulation may use; 0 for the platform's own number
        // (platform::backgroundThreads).
        inline constexpr Int backgroundThreads = integer(engine, "AmbrosiaBackgroundThreads", 0, { 0, 1024 }, NextUse, AmbrosiaOnly);
        // NetworkDevice and Language stay: the Engine class declares them config.
        inline constexpr String demoRecordingDevice = unused(engine, "DemoRecordingDevice");
        inline constexpr String gameEngine = unused(engine, "GameEngine");
        inline constexpr String editorEngine = unused(engine, "EditorEngine");
        inline constexpr String windowedRenderDevice = unused(engine, "WindowedRenderDevice");
        inline constexpr String renderDeviceClass = unused(engine, "RenderDevice");
        inline constexpr String defaultServerGame = unused(engine, "DefaultServerGame");
        inline constexpr String renderClass = unused(engine, "Render");
        inline constexpr String inputClass = unused(engine, "Input");
        inline constexpr String canvasClass = unused(engine, "Canvas");
        inline constexpr String cdPath = unused(engine, "CdPath");

        // The window's section, [WinDrv.WindowsClient], where Ambrosia's own display and
        // controller choices are kept too, as each writes ini:Engine.Engine.ViewportManager.
        inline constexpr Section video = namedBy(viewportManager);
        // Game::windowedSize treats the original default, 640x480, as no explicit choice.
        inline constexpr Int windowedViewportX = integer(video, "WindowedViewportX", 640, { 0, 65535 }, NextUse, Supported);
        inline constexpr Int windowedViewportY = integer(video, "WindowedViewportY", 480, { 0, 65535 }, NextUse, Supported);
        inline constexpr Int fullscreenViewportX = integer(video, "FullscreenViewportX", 640, { 0, 65535 }, NextUse, Supported);
        inline constexpr Int fullscreenViewportY = integer(video, "FullscreenViewportY", 480, { 0, 65535 }, NextUse, Supported);
        inline constexpr Bool startupFullscreen = boolean(video, "StartupFullscreen", true, Restart, Supported);
        inline constexpr Float brightness = decimal(video, "Brightness", 0.6, { 0, 1 }, Live, Supported);
        inline constexpr Bool exclusiveFullscreen = boolean(video, "ExclusiveFullscreen", false, NextUse, AmbrosiaOnly);
        // 60 degrees and 5 more for each step.
        inline constexpr Int fieldOfViewIndex = integer(video, "FieldOfViewIndex", 3, { 0, 12 }, Live, AmbrosiaOnly);
        // 2 to the power of it.
        inline constexpr Int anisotropy = integer(video, "Anisotropy", 0, { 0, 4 }, Live, AmbrosiaOnly);
        inline constexpr Bool useTrilinear = boolean(video, "UseTrilinear", false, Live, AmbrosiaOnly);
        // Off, 2x, 4x, 8x.
        inline constexpr Int multisampling = integer(video, "Multisampling", 0, { 0, 3 }, Live, AmbrosiaOnly);
        inline constexpr Bool fxaa = boolean(video, "FXAA", false, Live, AmbrosiaOnly);
        inline constexpr Bool smoothLightmaps = boolean(video, "SmoothLightmaps", false, Live, AmbrosiaOnly);
        // Normal, Far, Very Far.
        inline constexpr Int detailDistance = integer(video, "DetailDistance", 0, { 0, 2 }, Live, AmbrosiaOnly);
        // Off, On, Adaptive.
        inline constexpr Int vsync = integer(video, "VSync", 1, { 0, 2 }, Live, AmbrosiaOnly);
        // Keep the original indices in saved settings. The last visible choice is the
        // monitor's own refresh rate, even when it falls between (or above) the presets.
        inline constexpr int frameRates[] = { 0, 30, 60, 75, 120, 144, 165, 240, 280, 360, 480, 540, 600 };
        inline constexpr Int frameRateLimit = integer(video, "FrameRateLimit", 0, { 0, std::size(frameRates) }, Live, AmbrosiaOnly);
        // Off, Low, Medium, High.
        inline constexpr Int ambientOcclusion = integer(video, "AmbientOcclusion", 0, { 0, 3 }, Live, AmbrosiaOnly);
        // Auto, 1x to 4x.
        inline constexpr Int uiScale = integer(video, "UIScale", 0, { 0, 4 }, Live, AmbrosiaOnly);
        inline constexpr Int conversationFont = integer(video, "ConversationFont", 0, { 0, 2 }, NextUse, AmbrosiaOnly);
        inline constexpr Bool centeredConversations = boolean(video, "CenteredConversations", true, Live, AmbrosiaOnly);
        // Steady, Original.
        inline constexpr Int frameTiming = integer(video, "FrameTiming", 0, { 0, 1 }, Live, AmbrosiaOnly);
        // Xbox, PlayStation, Switch, Auto (game/menu_options.h). The Switch has no choice of it
        // and always uses its own.
        inline constexpr Int gamepadGlyphs = integer(video, "GamepadGlyphs", PerPlatform { .desktop = 3, .nintendoSwitch = 2 }, { 0, 3 }, Live, AmbrosiaOnly);
        inline constexpr Bool gamepadAimAssist = boolean(video, "GamepadAimAssist", true, Live, AmbrosiaOnly);
        inline constexpr Bool gamepadGyro = boolean(video, "GamepadGyro", false, Live, AmbrosiaOnly);
        // Local, Player (gyro::Space). A handheld Switch is held level, where the local
        // space is the most exact; a controller elsewhere is held at any tilt.
        inline constexpr Int gamepadGyroSpace = integer(video, "GamepadGyroSpace", PerPlatform { .desktop = 1, .nintendoSwitch = 0 }, { 0, 1 }, Live, AmbrosiaOnly);
        // How far the view turns for the controller's turn, 1 to 10: 0.24 times this, so at 3
        // (0.72) a little less than the controller (see GamepadInput::update).
        inline constexpr Int gamepadGyroSensitivity = integer(video, "GamepadGyroSensitivity", 3, { 1, 10 }, Live, AmbrosiaOnly);
        // How fast the Look stick turns the view, 1 to 10; 5 is the speed it always had, and
        // each step is a fifth of it.
        inline constexpr Int gamepadLookSpeed = integer(video, "GamepadLookSpeed", 5, { 1, 10 }, Live, AmbrosiaOnly);
        // Stick tuning, in the ini only. A dead zone is how far a stick moves before it counts,
        // the outer zone how far it must go to count as all the way, both as a part of its
        // travel; the curve raises the Look stick's push to that power, so a light push aims
        // finely.
        inline constexpr Float gamepadMoveDeadZone = decimal(video, "GamepadMoveDeadZone", 0.2, { 0, 0.9 }, Live, AmbrosiaOnly);
        inline constexpr Float gamepadMoveOuterZone = decimal(video, "GamepadMoveOuterZone", 1.0, { 0.5, 1 }, Live, AmbrosiaOnly);
        inline constexpr Float gamepadLookDeadZone = decimal(video, "GamepadLookDeadZone", 0.15, { 0, 0.9 }, Live, AmbrosiaOnly);
        inline constexpr Float gamepadLookOuterZone = decimal(video, "GamepadLookOuterZone", 1.0, { 0.5, 1 }, Live, AmbrosiaOnly);
        inline constexpr Float gamepadLookCurve = decimal(video, "GamepadLookCurve", 2.0, { 1, 4 }, Live, AmbrosiaOnly);
        // With gyro aiming on, the Look stick flicks and turns the view (gamepad::FlickStick)
        // and the gyro does the rest of the aiming.
        inline constexpr Bool gamepadFlickStick = boolean(video, "GamepadFlickStick", false, Live, AmbrosiaOnly);
        // Controller remapping, in the ini only (gamepad::Layout): each button's key names the
        // button whose role it takes in play, by gamepad::buttonName ("y", "lb", "up"), or
        // "none". Empty keeps its own. In gamepad::Button's order.
        inline constexpr Section gamepadLayout = fixed("Ambrosia.Gamepad");
        inline constexpr std::array<String, 20> gamepadInputs { {
            text(gamepadLayout, "A", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "B", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "X", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Y", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "LB", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "RB", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "View", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Menu", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "LS", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "RS", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "LT", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "RT", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Up", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Down", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Left", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "Right", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "R4", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "L4", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "R5", "", Live, AmbrosiaOnly),
            text(gamepadLayout, "L5", "", Live, AmbrosiaOnly),
        } };
        // The original's texture and decal choices; Ambrosia's renderer has no use for them.
        inline constexpr Int fullscreenColorBits = integer(video, "FullscreenColorBits", 16, { 16, 32 }, Live, Ignored);
        inline constexpr Int windowedColorBits = integer(video, "WindowedColorBits", 16, { 16, 32 }, Live, Ignored);
        inline constexpr String textureDetail = text(video, "TextureDetail", "High", Live, Ignored);
        inline constexpr String skinDetail = text(video, "SkinDetail", "High", Live, Ignored);
        inline constexpr Bool decals = boolean(video, "Decals", true, Live, Ignored);
        inline constexpr String mipFactor = unused(video, "MipFactor");
        inline constexpr String useDirectDraw = unused(video, "UseDirectDraw");
        inline constexpr String useJoystick = unused(video, "UseJoystick");
        inline constexpr String captureMouse = unused(video, "CaptureMouse");
        inline constexpr String curvedSurfaces = unused(video, "CurvedSurfaces");
        inline constexpr String screenFlashes = unused(video, "ScreenFlashes");
        inline constexpr String noLighting = unused(video, "NoLighting");
        inline constexpr String slowVideoBuffering = unused(video, "SlowVideoBuffering");
        inline constexpr String deadZoneXYZ = unused(video, "DeadZoneXYZ");
        inline constexpr String deadZoneRUV = unused(video, "DeadZoneRUV");
        inline constexpr String invertVertical = unused(video, "InvertVertical");
        inline constexpr String scaleXYZ = unused(video, "ScaleXYZ");
        inline constexpr String scaleRUV = unused(video, "ScaleRUV");
        inline constexpr String minDesiredFrameRate = unused(video, "MinDesiredFrameRate");

        // The render device's section, where the Detail Textures choice writes.
        inline constexpr Section renderDevice = namedBy(gameRenderDevice);
        inline constexpr Bool detailTextures = boolean(renderDevice, "DetailTextures", true, Live, Ignored);

        // The audio device's section, [Galaxy.GalaxyAudioSubsystem]. All are applied on their
        // change notice (audio.cc).
        inline constexpr Section audio = namedBy(audioDevice);
        // Of 255.
        inline constexpr Float musicVolume = decimal(audio, "MusicVolume", 153, { 0, 255 }, Live, Supported);
        inline constexpr Float soundVolume = decimal(audio, "SoundVolume", 204, { 0, 255 }, Live, Supported);
        inline constexpr Float speechVolume = decimal(audio, "SpeechVolume", 255, { 0, 255 }, Live, Supported);
        inline constexpr Int effectsChannels = integer(audio, "EffectsChannels", 16, { 1, 64 }, Live, Supported);
        inline constexpr Bool useReverb = boolean(audio, "UseReverb", true, Live, Supported);
        // The next music played.
        inline constexpr Bool useDigitalMusic = boolean(audio, "UseDigitalMusic", true, NextUse, Supported);
        inline constexpr Float ambientFactor = decimal(audio, "AmbientFactor", 0.7, { 0, unbounded }, Live, Supported);
        inline constexpr Float dopplerSpeed = decimal(audio, "DopplerSpeed", 6500, { 0, unbounded }, Live, Supported);
        // The original game used output latency to time music changes. Ambrosia uses a fixed delay.
        inline constexpr Float latency = decimal(audio, "Latency", 40, { 0, unbounded }, Live, Ignored);
        // Megabytes of decoded sound kept; 0 for no limit.
        inline constexpr Int soundCacheMegabytes = integer(audio, "SoundCacheMegabytes", 64, { 0, 1 << 20 }, Live, AmbrosiaOnly);
        // Steam Audio's HRTF instead of Galaxy's stereo pan.
        inline constexpr Bool hrtf = boolean(audio, "AmbrosiaHRTF", false, Live, AmbrosiaOnly);
        // Occlusion and reverb simulated from the level, on top of the HRTF.
        inline constexpr Bool raytracedAudio = boolean(audio, "AmbrosiaRaytracedAudio", false, Live, AmbrosiaOnly);
        // The raytraced reverb's level: by default as loud, on average, as the authored zone
        // reverb standing in its zone in 06_HongKong_WanChai_Garage (where 0.67 matches) and
        // 03_NYC_AirfieldHeliBase (1.0), measured by acoustics_measure_test.
        inline constexpr Float reverbLevel = decimal(audio, "AmbrosiaReverbLevel", 0.8, { 0, unbounded }, Live, AmbrosiaOnly);
        inline constexpr Bool bakeAcoustics = boolean(audio, "AmbrosiaBakeAcoustics", true, Live, AmbrosiaOnly);
        inline constexpr Bool bakedReverb = boolean(audio, "AmbrosiaBakedReverb", true, Live, AmbrosiaOnly);
        inline constexpr Bool pathing = boolean(audio, "AmbrosiaPathing", true, Live, AmbrosiaOnly);
        // The original's sound choices; the mixer runs at 44100 Hz in stereo.
        inline constexpr String outputRate = text(audio, "OutputRate", "44100Hz", Live, Ignored);
        inline constexpr Bool lowSoundQuality = boolean(audio, "LowSoundQuality", false, Live, Ignored);
        inline constexpr Bool reverseStereo = boolean(audio, "ReverseStereo", false, Live, Ignored);
        inline constexpr Bool useSurround = boolean(audio, "UseSurround", false, Live, Ignored);
        inline constexpr Bool use3dHardware = boolean(audio, "Use3dHardware", false, Live, Ignored);
        inline constexpr String useDirectSound = unused(audio, "UseDirectSound");
        inline constexpr String useFilter = unused(audio, "UseFilter");
        inline constexpr String useStereo = unused(audio, "UseStereo");
        inline constexpr String useCDMusic = unused(audio, "UseCDMusic");
        inline constexpr String useSpatial = unused(audio, "UseSpatial");

        // Whether a level change releases the old level's objects.
        inline constexpr Bool garbageCollection = boolean(fixed("Engine.GameEngine"), "GarbageCollection", true, NextUse, AmbrosiaOnly);
        // Debugging, and a workaround for a bug found in play: windows, actors and AI events
        // deleted during play are kept until the next level change (or load) instead, with every
        // reference to them cleared then, and script that reads one is logged.
        inline constexpr Bool keepFreedObjects = boolean(fixed("Engine.GameEngine"), "KeepFreedObjects", false, NextUse, AmbrosiaOnly);

        inline constexpr Section url = fixed("URL");
        // The map played when none is given, and when a level fails to load.
        inline constexpr String localMap = text(url, "LocalMap", "DX.dx", NextUse, Supported);
        // Only reported to the scripts; there is no networking.
        inline constexpr Int port = integer(url, "Port", 7790, { 0, 65535 }, NextUse, Supported);
        // The original's URL defaults; Ambrosia's URLs take none of them from here.
        inline constexpr String urlProtocol = unused(url, "Protocol");
        inline constexpr String urlProtocolDescription = unused(url, "ProtocolDescription");
        inline constexpr String urlName = unused(url, "Name");
        inline constexpr String urlMap = unused(url, "Map");
        inline constexpr String urlHost = unused(url, "Host");
        inline constexpr String urlPortal = unused(url, "Portal");
        inline constexpr String urlMapExt = unused(url, "MapExt");
        inline constexpr String urlSaveExt = unused(url, "SaveExt");
        inline constexpr String urlClass = unused(url, "Class");

        // User.ini.
        inline constexpr String playerClass = text(fixed("DefaultPlayer", File::User), "Class", "DeusEx.JCDentonMale", NextUse, Supported);
    }

    using namespace keys;

    // Every setting above.
    std::span<const Setting* const> all();

    // Whether a save of the system or user ini leaves a key out (an empty key: the whole
    // section): a key the table marks Ignored, or a system ini section nothing reads (the render
    // devices Ambrosia does not have). Keys and sections the table does not know are kept, so
    // nothing a script reads goes. A key left out reads as its default.
    bool leftOut(File file, std::string_view section, std::string_view key);

    Ini& ini(File file);
    // The section a setting is in now: its own, or the one the ini names, or the naming
    // setting's default when it names none.
    std::string section(const Section& section);

    // The default of a bool (0 or 1), int or float on a platform.
    constexpr double defaultNumber(const Setting& setting, platform::Kind platform)
    {
        return platform == platform::Kind::Switch && setting.switchNumber ? *setting.switchNumber : setting.number;
    }

    // The default on the platform the build is for.
    double defaultNumber(const Setting& setting);

    // The parsers, one per type: empty when the text is not a value of that type. A bool is
    // True/False, T/F or 1/0 in any case; numbers are plain decimal.
    std::optional<bool> parseBool(std::string_view text);
    std::optional<int> parseInt(std::string_view text);
    std::optional<float> parseFloat(std::string_view text);

    // A value as the ini keeps it: True/False, a whole number, or six decimals as the original
    // does (0.600000).
    std::string format(bool value);
    std::string format(int value);
    std::string format(float value);

    // What the setting is: the ini's value, clamped, or the default where the ini has none or
    // has something that is not of the setting's type.
    bool get(const Bool& setting);
    int get(const Int& setting);
    float get(const Float& setting);
    std::string get(const String& setting);
    // Whether the ini has a value of the setting's type for it.
    bool stored(const Setting& setting);
    // The setting's value, or its default, as text (as format gives it).
    std::string valueText(const Setting& setting);
    std::string defaultText(const Setting& setting);

    // The setting in a section (by name, any case) and key of an ini, or null.
    const Setting* find(File file, std::string_view section, std::string_view key);
    // The section an ini: path names, as the console's get ini: and set ini: take it:
    // Engine.Engine.ViewportManager is the section [Engine.Engine] ViewportManager names. Empty
    // when the path names none.
    std::string iniPathSection(std::string_view path);
    // What the console's get ini: answers for a key of the system ini: for a setting, what
    // Ambrosia reads (True or False for a bool, its default where the ini has no value), keeping
    // the ini's own text for a number already in range; for any other key, the ini's value or
    // nothing.
    std::string iniText(std::string_view section, std::string_view key);
    // The setting a menu choice's configSetting names ("ini:Engine.Engine.AudioDevice
    // OutputRate"), or null.
    const Setting* forConfigSetting(std::string_view configSetting);
    // The configSetting a menu choice gives to read and write the setting through the console
    // (ini:Engine.Engine.ViewportManager FieldOfViewIndex), or empty for a setting in a section
    // of its own, which ini: cannot name.
    std::string configSetting(const Setting& setting);

    // The Audio Mixer menu choice's key in the audio section. It is not stored: it reads as 0
    // (Stereo), 1 (HRTF) or 2 (Raytraced), worked out from AmbrosiaHRTF and AmbrosiaRaytracedAudio.
    // Writing 0 or 1 sets both, so the raytraced mode goes off; writing 2, or anything else,
    // changes nothing, so the menu can show the raytraced mode but never choose it.
    inline constexpr std::string_view audioMixerKey = "AmbrosiaAudioMixer";
    bool isAudioMixer(std::string_view section, std::string_view key);
    int audioMixer();
    void setAudioMixer(std::string_view value);
    // The configSetting a menu choice gives for it (ini:Engine.Engine.AudioDevice
    // AmbrosiaAudioMixer).
    std::string audioMixerConfigSetting();

    // A write of one key: its section, key and the text stored.
    struct Entry {
        std::string section;
        std::string key;
        std::string value;
    };

    // The one write path: sets each key in the ini, saves it once (saying so in the log if that
    // fails) and gives each key's change notice (config::onSettingChanged), saved or not, as the
    // values are in effect either way. Returns whether the save worked.
    bool write(Ini& ini, std::initializer_list<Entry> entries);
    bool write(Ini& ini, std::span<const Entry> entries);

    // A setting and the value to give it, for set.
    struct Assignment {
        const Setting* setting;
        std::string value;
    };

    Assignment to(const Bool& setting, bool value);
    Assignment to(const Int& setting, int value);
    Assignment to(const Float& setting, float value);
    Assignment to(const String& setting, std::string_view value);

    // Writes the settings, clamped, through write: one save for each ini they are in.
    bool set(std::initializer_list<Assignment> assignments);

    template <typename T, typename V>
    bool set(const Typed<T>& setting, const V& value)
    {
        return set({ to(setting, value) });
    }
}
