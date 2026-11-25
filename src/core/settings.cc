#include "core/settings.h"

#include "core/log.h"
#include "core/package.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace dx::settings {
    namespace {
        const Setting* const table[] = {
            &viewportManager.setting,
            &audioDevice.setting,
            &gameRenderDevice.setting,
            &console.setting,
            &defaultGame.setting,
            &root.setting,
            &backgroundThreads.setting,
            &demoRecordingDevice.setting,
            &gameEngine.setting,
            &editorEngine.setting,
            &windowedRenderDevice.setting,
            &renderDeviceClass.setting,
            &defaultServerGame.setting,
            &renderClass.setting,
            &inputClass.setting,
            &canvasClass.setting,
            &cdPath.setting,
            &windowedViewportX.setting,
            &windowedViewportY.setting,
            &fullscreenViewportX.setting,
            &fullscreenViewportY.setting,
            &startupFullscreen.setting,
            &brightness.setting,
            &exclusiveFullscreen.setting,
            &fieldOfViewIndex.setting,
            &anisotropy.setting,
            &useTrilinear.setting,
            &multisampling.setting,
            &fxaa.setting,
            &smoothLightmaps.setting,
            &detailDistance.setting,
            &vsync.setting,
            &frameRateLimit.setting,
            &ambientOcclusion.setting,
            &uiScale.setting,
            &conversationFont.setting,
            &centeredConversations.setting,
            &frameTiming.setting,
            &gamepadGlyphs.setting,
            &gamepadAimAssist.setting,
            &gamepadGyro.setting,
            &gamepadGyroSpace.setting,
            &gamepadGyroSensitivity.setting,
            &gamepadLookSpeed.setting,
            &gamepadMoveDeadZone.setting,
            &gamepadMoveOuterZone.setting,
            &gamepadLookDeadZone.setting,
            &gamepadLookOuterZone.setting,
            &gamepadLookCurve.setting,
            &gamepadFlickStick.setting,
            &gamepadInputs[0].setting,
            &gamepadInputs[1].setting,
            &gamepadInputs[2].setting,
            &gamepadInputs[3].setting,
            &gamepadInputs[4].setting,
            &gamepadInputs[5].setting,
            &gamepadInputs[6].setting,
            &gamepadInputs[7].setting,
            &gamepadInputs[8].setting,
            &gamepadInputs[9].setting,
            &gamepadInputs[10].setting,
            &gamepadInputs[11].setting,
            &gamepadInputs[12].setting,
            &gamepadInputs[13].setting,
            &gamepadInputs[14].setting,
            &gamepadInputs[15].setting,
            &gamepadInputs[16].setting,
            &gamepadInputs[17].setting,
            &gamepadInputs[18].setting,
            &gamepadInputs[19].setting,
            &fullscreenColorBits.setting,
            &windowedColorBits.setting,
            &textureDetail.setting,
            &skinDetail.setting,
            &decals.setting,
            &mipFactor.setting,
            &useDirectDraw.setting,
            &useJoystick.setting,
            &captureMouse.setting,
            &curvedSurfaces.setting,
            &screenFlashes.setting,
            &noLighting.setting,
            &slowVideoBuffering.setting,
            &deadZoneXYZ.setting,
            &deadZoneRUV.setting,
            &invertVertical.setting,
            &scaleXYZ.setting,
            &scaleRUV.setting,
            &minDesiredFrameRate.setting,
            &detailTextures.setting,
            &musicVolume.setting,
            &soundVolume.setting,
            &speechVolume.setting,
            &effectsChannels.setting,
            &useReverb.setting,
            &useDigitalMusic.setting,
            &ambientFactor.setting,
            &dopplerSpeed.setting,
            &latency.setting,
            &soundCacheMegabytes.setting,
            &hrtf.setting,
            &raytracedAudio.setting,
            &reverbLevel.setting,
            &bakeAcoustics.setting,
            &bakedReverb.setting,
            &pathing.setting,
            &outputRate.setting,
            &lowSoundQuality.setting,
            &reverseStereo.setting,
            &useSurround.setting,
            &use3dHardware.setting,
            &useDirectSound.setting,
            &useFilter.setting,
            &useStereo.setting,
            &useCDMusic.setting,
            &useSpatial.setting,
            &garbageCollection.setting,
            &keepFreedObjects.setting,
            &localMap.setting,
            &port.setting,
            &urlProtocol.setting,
            &urlProtocolDescription.setting,
            &urlName.setting,
            &urlMap.setting,
            &urlHost.setting,
            &urlPortal.setting,
            &urlMapExt.setting,
            &urlSaveExt.setting,
            &urlClass.setting,
            &playerClass.setting,
        };

        // Omit these legacy DeusEx.ini sections when saving. Neither the engine nor script
        // config classes read them.
        const std::string_view unusedSections[] = {
            "SoftDrv.SoftwareRenderDevice",
            "GlideDrv.GlideRenderDevice",
            "MetalDrv.MetalRenderDevice",
            "OpenGLDrv.OpenGLRenderDevice",
            "D3DDrv.D3DRenderDevice",
            "SglDrv.SglRenderDevice",
            "DeusEx.DeusExGameEngine",
        };

        // The stored text, or empty for none: an empty value counts as none, as it does for
        // every setting Ambrosia reads.
        std::optional<std::string> raw(const Setting& setting)
        {
            std::optional<std::string> value = ini(setting.section.file).get(section(setting.section), setting.key);
            if (value && value->empty()) {
                return std::nullopt;
            }
            return value;
        }

        double clamped(const Setting& setting, double value)
        {
            return std::clamp(value, setting.minimum, setting.maximum);
        }

        // The value as a number, unclamped, when the ini has one of the setting's type.
        std::optional<double> storedNumber(const Setting& setting)
        {
            std::optional<std::string> value = raw(setting);
            if (!value) {
                return std::nullopt;
            }
            switch (setting.type) {
            case Type::Bool:
                if (std::optional<bool> parsed = parseBool(*value)) {
                    return *parsed ? 1.0 : 0.0;
                }
                return std::nullopt;
            case Type::Int:
                if (std::optional<int> parsed = parseInt(*value)) {
                    return *parsed;
                }
                return std::nullopt;
            case Type::Float:
                if (std::optional<float> parsed = parseFloat(*value)) {
                    return *parsed;
                }
                return std::nullopt;
            case Type::String:
                return std::nullopt;
            }
            return std::nullopt;
        }

        double number(const Setting& setting)
        {
            return clamped(setting, storedNumber(setting).value_or(defaultNumber(setting)));
        }

        std::string formatNumber(const Setting& setting, double value)
        {
            switch (setting.type) {
            case Type::Bool:
                return format(value != 0);
            case Type::Int:
                return format(static_cast<int>(value));
            case Type::Float:
                return format(static_cast<float>(value));
            case Type::String:
                return {};
            }
            return {};
        }
    }

    std::span<const Setting* const> all()
    {
        return table;
    }

    bool leftOut(File file, std::string_view sectionName, std::string_view key)
    {
        if (key.empty()) {
            return file == File::System && std::any_of(std::begin(unusedSections), std::end(unusedSections), [&](std::string_view name) { return iequals(name, sectionName); });
        }
        const Setting* setting = find(file, sectionName, key);
        return setting && setting->support == Support::Ignored;
    }

    Ini& ini(File file)
    {
        return file == File::User ? config::user() : config::system();
    }

    std::string section(const Section& place)
    {
        if (!place.namedBy) {
            return std::string(place.name);
        }
        std::optional<std::string> named = ini(place.namedBy->section.file).get(section(place.namedBy->section), place.namedBy->key);
        return named && !named->empty() ? *named : std::string(place.namedBy->text);
    }

    double defaultNumber(const Setting& setting)
    {
        return defaultNumber(setting, platform::kind());
    }

    std::optional<bool> parseBool(std::string_view text)
    {
        if (iequals(text, "true") || iequals(text, "t") || text == "1") {
            return true;
        }
        if (iequals(text, "false") || iequals(text, "f") || text == "0") {
            return false;
        }
        return std::nullopt;
    }

    std::optional<int> parseInt(std::string_view text)
    {
        int value = 0;
        const char* last = text.data() + text.size();
        auto [end, error] = std::from_chars(text.data(), last, value);
        if (error != std::errc()) {
            return std::nullopt;
        }
        // A slider writes its value with decimals (16.000000), which reads as its whole part.
        if (end != last && *end == '.') {
            end++;
            while (end != last && *end >= '0' && *end <= '9') {
                end++;
            }
        }
        if (end != last) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<float> parseFloat(std::string_view text)
    {
        float value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

    std::string format(bool value)
    {
        return value ? "True" : "False";
    }

    std::string format(int value)
    {
        return std::to_string(value);
    }

    std::string format(float value)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%f", static_cast<double>(value));
        return text;
    }

    bool get(const Bool& setting)
    {
        return number(setting.setting) != 0;
    }

    int get(const Int& setting)
    {
        return static_cast<int>(number(setting.setting));
    }

    float get(const Float& setting)
    {
        return static_cast<float>(number(setting.setting));
    }

    std::string get(const String& setting)
    {
        return raw(setting.setting).value_or(std::string(setting.setting.text));
    }

    bool stored(const Setting& setting)
    {
        return setting.type == Type::String ? raw(setting).has_value() : storedNumber(setting).has_value();
    }

    std::string valueText(const Setting& setting)
    {
        if (setting.type == Type::String) {
            return raw(setting).value_or(std::string(setting.text));
        }
        return formatNumber(setting, number(setting));
    }

    std::string defaultText(const Setting& setting)
    {
        if (setting.type == Type::String) {
            return std::string(setting.text);
        }
        return formatNumber(setting, clamped(setting, defaultNumber(setting)));
    }

    std::string iniText(std::string_view sectionName, std::string_view key)
    {
        const Setting* setting = find(File::System, sectionName, key);
        if (!setting) {
            return config::system().get(sectionName, key).value_or("");
        }
        // A number already in range keeps the ini's spelling of it (a slider's 16.000000).
        if (setting->type == Type::Int || setting->type == Type::Float) {
            std::optional<double> value = storedNumber(*setting);
            if (value && clamped(*setting, *value) == *value) {
                return *raw(*setting);
            }
        }
        return valueText(*setting);
    }

    const Setting* find(File file, std::string_view sectionName, std::string_view key)
    {
        for (const Setting* setting : table) {
            if (setting->section.file == file && iequals(setting->key, key) && iequals(section(setting->section), sectionName)) {
                return setting;
            }
        }
        return nullptr;
    }

    std::string iniPathSection(std::string_view path)
    {
        size_t dot = path.rfind('.');
        if (dot == std::string_view::npos) {
            return {};
        }
        std::string_view naming = path.substr(0, dot);
        std::string_view key = path.substr(dot + 1);
        if (const Setting* setting = find(File::System, naming, key); setting && setting->type == Type::String) {
            return valueText(*setting);
        }
        return config::system().get(naming, key).value_or("");
    }

    const Setting* forConfigSetting(std::string_view configSetting)
    {
        if (configSetting.size() <= 4 || !iequals(configSetting.substr(0, 4), "ini:")) {
            return nullptr;
        }
        std::string_view rest = configSetting.substr(4);
        size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
            return nullptr;
        }
        std::string_view key = rest.substr(space + 1);
        while (!key.empty() && key.front() == ' ') {
            key.remove_prefix(1);
        }
        std::string sectionName = iniPathSection(rest.substr(0, space));
        return sectionName.empty() ? nullptr : find(File::System, sectionName, key);
    }

    std::string configSetting(const Setting& setting)
    {
        const Setting* naming = setting.section.namedBy;
        if (!naming || naming->section.namedBy || naming->section.file != File::System || setting.section.file != File::System) {
            return {};
        }
        return "ini:" + std::string(naming->section.name) + "." + std::string(naming->key) + " " + std::string(setting.key);
    }

    bool write(Ini& target, std::initializer_list<Entry> entries)
    {
        return write(target, std::span<const Entry>(entries.begin(), entries.size()));
    }

    bool write(Ini& target, std::span<const Entry> entries)
    {
        for (const Entry& entry : entries) {
            target.set(entry.section, entry.key, entry.value);
        }
        bool saved = target.save();
        if (!saved) {
            log::error("DevConfig", "cannot save the settings to {}", target.path.string());
        }
        for (const Entry& entry : entries) {
            config::settingChanged(target, entry.section, entry.key);
        }
        return saved;
    }

    Assignment to(const Bool& setting, bool value)
    {
        return { &setting.setting, format(value) };
    }

    Assignment to(const Int& setting, int value)
    {
        return { &setting.setting, format(static_cast<int>(clamped(setting.setting, value))) };
    }

    Assignment to(const Float& setting, float value)
    {
        return { &setting.setting, format(static_cast<float>(clamped(setting.setting, value))) };
    }

    Assignment to(const String& setting, std::string_view value)
    {
        return { &setting.setting, std::string(value) };
    }

    bool set(std::initializer_list<Assignment> assignments)
    {
        std::map<File, std::vector<Entry>> byFile;
        for (const Assignment& assignment : assignments) {
            const Setting& setting = *assignment.setting;
            byFile[setting.section.file].push_back({ section(setting.section), std::string(setting.key), assignment.value });
        }
        bool saved = true;
        for (const auto& [file, entries] : byFile) {
            saved = write(ini(file), entries) && saved;
        }
        return saved;
    }

    bool isAudioMixer(std::string_view sectionName, std::string_view key)
    {
        return iequals(key, audioMixerKey) && iequals(sectionName, section(audio));
    }

    int audioMixer()
    {
        if (get(raytracedAudio)) {
            return 2;
        }
        return get(hrtf) ? 1 : 0;
    }

    void setAudioMixer(std::string_view value)
    {
        // The menu writes its value as a float (1.00).
        std::optional<float> chosen = parseFloat(value);
        if (!chosen || (*chosen != 0.0f && *chosen != 1.0f)) {
            return;
        }
        set({ to(hrtf, *chosen == 1.0f), to(raytracedAudio, false) });
    }

    std::string audioMixerConfigSetting()
    {
        std::string text = configSetting(hrtf.setting);
        return text.empty() ? text : text.substr(0, text.rfind(' ') + 1) + std::string(audioMixerKey);
    }
}
