#include "game/game.h"

#include "core/settings.h"
#include "core/objects.h"
#include "deusex/script.h"
#include "deusex/ui.h"
#include "deusex/gamepad_layout.h"
#include "deusex/glyphs.h"
#include "extension/extension.h"
#include "game/platform.h"
#include "render/procedural.h"
#include "render/scene_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dx::game {
    float Game::displayFov(float fov, int width, int height)
    {
        int index = dx::settings::get(dx::settings::fieldOfViewIndex);
        float wanted = 60.0f + 5.0f * static_cast<float>(index);
        float adjusted = fov + (wanted - 75.0f) * std::clamp((fov - 25.0f) / 50.0f, 0.0f, 1.0f);
        float aspect = static_cast<float>(width) / static_cast<float>(height) * 0.75f;
        return std::atan(std::tan(adjusted * dx::Pi / 360.0f) * aspect) * 360.0f / dx::Pi;
    }

    void Game::applyControllerOptions()
    {
        const MenuOptions& available = platform::menuOptions();
        int chosen = dx::settings::get(dx::settings::gamepadGlyphs);
        // Run each frame, so Auto follows a change of pad on screens already showing prompts.
        int detected = static_cast<int>(dx::glyphs::padStyle());
        dx::glyphs::setStyle(static_cast<dx::glyphs::Style>(buttonPromptStyle(available, chosen, detected)));
        pad.setAimAssist(dx::settings::get(dx::settings::gamepadAimAssist) && !dx::settings::get(dx::settings::gamepadGyro));
        static_assert(dx::settings::gamepadInputs.size() == dx::gamepad::ButtonCount);
        std::array<std::string, dx::settings::gamepadInputs.size()> layout;
        for (size_t i = 0; i < layout.size(); i++) {
            layout[i] = dx::settings::get(dx::settings::gamepadInputs[i]);
        }
        dx::gamepad::setLayout(dx::gamepad::parseLayout(layout));
        // The Calibrate Gyro button tells how its calibration is going.
        switch (pad.gyroCalibrationState()) {
        case dx::gyro::Calibration::State::Idle:
            dx::padlayout::setCalibrateLabel(dx::padlayout::calibrateLabel);
            break;
        case dx::gyro::Calibration::State::Measuring:
            dx::padlayout::setCalibrateLabel("Set It Down...");
            break;
        case dx::gyro::Calibration::State::Done:
            dx::padlayout::setCalibrateLabel("Gyro Calibrated");
            break;
        case dx::gyro::Calibration::State::Failed:
            dx::padlayout::setCalibrateLabel("Moved, Retry");
            break;
        case dx::gyro::Calibration::State::NoGyro:
            dx::padlayout::setCalibrateLabel("No Gyro Found");
            break;
        }
        pad.setStickTuning({
            .moveDeadZone = dx::settings::get(dx::settings::gamepadMoveDeadZone),
            .moveOuterZone = dx::settings::get(dx::settings::gamepadMoveOuterZone),
            .lookDeadZone = dx::settings::get(dx::settings::gamepadLookDeadZone),
            .lookOuterZone = dx::settings::get(dx::settings::gamepadLookOuterZone),
            .lookCurve = dx::settings::get(dx::settings::gamepadLookCurve),
            .lookSpeed = static_cast<float>(dx::settings::get(dx::settings::gamepadLookSpeed)) / 5.0f,
            .flickStick = dx::settings::get(dx::settings::gamepadFlickStick),
        });
        pad.setGyro(dx::settings::get(dx::settings::gamepadGyro), static_cast<float>(dx::settings::get(dx::settings::gamepadGyroSensitivity)), static_cast<dx::gyro::Space>(dx::settings::get(dx::settings::gamepadGyroSpace)));
    }

    void Game::applyRenderOptions()
    {
        dx::RenderOptions render;
        render.anisotropy = static_cast<float>(1 << dx::settings::get(dx::settings::anisotropy));
        render.trilinear = dx::settings::get(dx::settings::useTrilinear);
        int multisample = dx::settings::get(dx::settings::multisampling);
        render.multisample = multisample > 0 ? 1 << multisample : 0;
        render.fxaa = dx::settings::get(dx::settings::fxaa);
        render.smoothLightmaps = dx::settings::get(dx::settings::smoothLightmaps);
        const float detailDistances[] = { 380.0f, 1024.0f, 2048.0f };
        render.detailDistance = detailDistances[dx::settings::get(dx::settings::detailDistance)];
        render.textureRate = dx::settings::get(dx::settings::frameTiming) == 0 ? dx::steadyTextureRate : 0.0f;
        render.ambientOcclusion = static_cast<dx::OcclusionQuality>(dx::settings::get(dx::settings::ambientOcclusion));
        renderer->setOptions(render);
        dx::extension::setUIScale(dx::settings::get(dx::settings::uiScale));
        int face = settings::get(settings::conversationFont);
        if (face == 0) {
            face = frameWidth > 800 ? 2 : 1;
        }
        if (face != appliedConversationFont) {
            // ConPlay is spawned for each conversation. Its original resolution test
            // falls back to the small font at 1280 and above. Both slots should use
            // the chosen face so speech, names and replies measure what they draw.
            if (UClass* cls = objects::findClass("DeusEx.ConPlay")) {
                for (int i = 0; i < 2; i++) {
                    if (auto* speech = script::field<UObject*>(cls->defaultObject, "ConversationSpeechFonts", i)) {
                        *speech = ui::font(face == 2 ? "DeusExUI.FontConversationLarge" : "DeusExUI.FontConversation");
                    }
                    if (auto* name = script::field<UObject*>(cls->defaultObject, "ConversationNameFonts", i)) {
                        *name = ui::font(face == 2 ? "DeusExUI.FontConversationLargeBold" : "DeusExUI.FontConversationBold");
                    }
                }
            }
            appliedConversationFont = face;
        }
        if (options.automated()) {
            return;
        }
        int vsync = dx::settings::get(dx::settings::vsync);
        if (vsync != appliedVSync) {
            appliedVSync = vsync;
            if (vsync != 2 || !SDL_GL_SetSwapInterval(-1)) {
                SDL_GL_SetSwapInterval(std::min(vsync, 1));
            }
        }
    }

    void Game::limitFrameRate()
    {
        size_t index = static_cast<size_t>(dx::settings::get(dx::settings::frameRateLimit));
        int limit = index < std::size(settings::frameRates) ? std::min(monitorHz, settings::frameRates[index]) : monitorHz;
        if (limit == 0 || options.automated()) {
            return;
        }
        uint64_t frequency = SDL_GetPerformanceFrequency();
        uint64_t deadline = frameDeadline + frequency / static_cast<uint64_t>(limit);
        uint64_t now = SDL_GetPerformanceCounter();
        if (now >= deadline) {
            frameDeadline = now;
            return;
        }
        while (now < deadline) {
            uint64_t remaining = (deadline - now) * 1000 / frequency;
            if (remaining > 1) {
                SDL_Delay(static_cast<uint32_t>(remaining - 1));
            }
            now = SDL_GetPerformanceCounter();
        }
        frameDeadline = deadline;
    }
}
