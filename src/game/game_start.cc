#include "game/game.h"

#include "audio/audio.h"
#include "core/config.h"
#include "core/gc.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/objects.h"
#include "core/sdl_handles.h"
#include "core/version.h"
#include "debugui/acoustics_section.h"
#include "debugui/actor_marks.h"
#include "debugui/audio_section.h"
#include "debugui/game_debug_tool.h"
#include "debugui/input_tool.h"
#include "debugui/memory_tool.h"
#include "debugui/performance_tool.h"
#include "debugui/render_tool.h"
#include "deusex/deusex.h"
#include "deusex/game_state.h"
#include "deusex/gamepad_layout.h"
#include "deusex/gamepad_menus.h"
#include "deusex/hud.h"
#include "deusex/menu_layout.h"
#include "deusex/script.h"
#include "deusex/pad_inventory.h"
#include "deusex/startup.h"
#include "deusex/terminals.h"
#include "deusex/ui.h"
#include "engine/canvas_hooks.h"
#include "engine/console.h"
#include "engine/savegame.h"
#include "extension/extension.h"
#include "render/gpu_profile.h"
#include "render/lighting.h"
#include "render/scene_renderer.h"

#include "game/console_report.h"
#include "core/text.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace dx::game {
    void Game::loadDeusExClasses()
    {
        dx::Linker* linker = dx::objects::loadPackage(dx::Name("DeusEx"));
        if (!linker) {
            dx::log::error("Init", "cannot load the DeusEx package");
            return;
        }
        const dx::Package& file = linker->file();
        for (size_t i = 0; i < file.exports().size(); i++) {
            if (file.exports()[i].classRef == 0) {
                dx::objects::ensureLoaded(linker->object(static_cast<int32_t>(i + 1)));
            }
        }
        dx::objects::flushLoads();
    }

    bool Game::start()
    {
        dx::engine::initialize(dx::config::findGameRoot().string());
        if (options.hidden && !options.nullSound) {
            // Mixes as usual but without an output device.
            dx::audio::initializeOffline(44100);
        } else {
            bool audio = dx::audio::initialize(options.nullSound);
            dx::game::report::stage("audio %s", audio ? "opened" : "not opened");
        }
        dx::audio::setLightRandom(dx::lightRandomBase);
        // The game is a holder for its canvas, the pad lets go of what it remembers, and the
        // hooks set below capture only the game itself.
        dx::gc::libraryReported("game");
        renderer = std::make_unique<dx::SceneRenderer>();
        renderer->iteratorHook = [this](dx::UObject* actor, const std::function<void(dx::UObject*)>& emit) {
            return world && dx::deusex::renderIterate(*world, actor, emit);
        };
        dx::console::openMapHook = [this](const std::string& url, int32_t travelType) {
            pendingMap = url;
            pendingTravelType = travelType;
        };
        dx::savegame::setProgressPainter([this] { paintProgress(); });
        dx::extension::setFrameReader([this](std::vector<uint8_t>& rgba, int& width, int& height) {
            return renderer && renderer->readKeptFrame(rgba, width, height);
        });
        // DDeusExGameEngine::SaveGame sets the size and takes the
        // picture itself, so every save, quick or not, has one.
        dx::savegame::setSnapshotSource([] {
            dx::extension::setSnapshotSize(160, 120);
            return dx::extension::generateSnapshot(true);
        });
        dx::console::quitHook = [this]() {
            running = false;
        };
        dx::console::currentResHook = [this]() {
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(window, &width, &height);
            return std::to_string(width) + "x" + std::to_string(height);
        };
        dx::console::resolutionsHook = [this]() {
            return availableResolutions();
        };
        dx::console::setResHook = [this](int width, int height) {
            bool fullscreen = isFullscreen();
            // Borderless fullscreen is always the desktop's size.
            if (fullscreen && !exclusiveFullscreen()) {
                return;
            }
            applyVideoMode(fullscreen, width, height);
            saveVideoMode();
        };
        dx::console::toggleFullscreenHook = [this]() {
            bool fullscreen = !isFullscreen();
            dx::resolutions::Size size = fullscreen ? fullscreenSize() : windowedSize();
            applyVideoMode(fullscreen, size.width, size.height);
            saveVideoMode();
        };
        dx::console::screenshotHook = [this]() {
            std::filesystem::path directory = dx::config::system().path.parent_path() / "Screenshots";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                dx::log::error("Log", "cannot create {}: {}", directory.string(), error.message());
                return;
            }
            int index = 0;
            std::filesystem::path file;
            do {
                char name[32];
                std::snprintf(name, sizeof(name), "Shot%04d.bmp", index++);
                file = directory / name;
            } while (std::filesystem::exists(file, error));
            if (error) {
                dx::log::error("Log", "cannot pick a screenshot name in {}", directory.string());
                return;
            }
            pendingShot = file.string();
        };
        overlay = std::make_unique<dx::debugui::Overlay>(window);
        overlay->setSettingsFile(options.hidden ? std::string() : (dx::config::userDirectory() / "DebugOverlay.ini").string());
        dx::debugui::addGameDebugTool(*overlay);
        dx::debugui::addAudioSection(*overlay);
        dx::debugui::addAcousticsSection(*overlay);
        dx::debugui::addMemoryTool(*overlay);
        dx::debugui::addPerformanceTool(*overlay);
        dx::debugui::addRenderTool(*overlay);
        dx::debugui::addActorMarks(*overlay, [this] {
            return world.get();
        });
        dx::debugui::addInputTool(*overlay, [this] {
            auto styleName = [](dx::glyphs::Style style) {
                switch (style) {
                case dx::glyphs::Style::PlayStation:
                    return "PlayStation";
                case dx::glyphs::Style::Switch:
                    return "Switch";
                case dx::glyphs::Style::Xbox:
                    break;
                }
                return "Xbox";
            };
            dx::debugui::InputState state;
            std::optional<SDL_JoystickID> prompted = pad.promptPad();
            for (const dx::sdl::Gamepad& owned : pad.openPads()) {
                SDL_Gamepad* each = owned.get();
                dx::debugui::InputState::Pad entry;
                entry.pad = each;
                entry.promptStyle = styleName(dx::game::promptStyleFor(SDL_GetGamepadType(each), SDL_GetRealGamepadType(each), SDL_GetGamepadButtonLabel(each, SDL_GAMEPAD_BUTTON_SOUTH)));
                entry.inPlay = SDL_GetGamepadID(each) == pad.activeController();
                entry.prompts = prompted && *prompted == SDL_GetGamepadID(each);
                state.pads.push_back(std::move(entry));
            }
            state.padLastUsed = pad.gamepadLastUsed();
            state.shownStyle = styleName(dx::glyphs::style());
            state.gyroAiming = pad.gyroAiming();
            if (std::optional<dx::gyro::Turn> rate = pad.lastGyroRate()) {
                state.gyroRead = true;
                state.gyroPitch = rate->pitch;
                state.gyroYaw = rate->yaw;
                state.gyroRoll = rate->roll;
            }
            dx::Vector down = pad.gyroDown();
            dx::gyro::Turn drift = pad.gyroDriftTaken();
            state.down[0] = down.x;
            state.down[1] = down.y;
            state.down[2] = down.z;
            state.drift[0] = drift.pitch;
            state.drift[1] = drift.yaw;
            state.drift[2] = drift.roll;
            const char* const calibrations[] = { "idle", "measuring", "done", "failed: not still", "no gyro readings" };
            state.calibration = calibrations[static_cast<int>(pad.gyroCalibrationState())];
            state.gyroKeptBack = pad.gyroKeptBack();
            for (const dx::game::Finger& finger : touch.fingers()) {
                state.touches.push_back({ finger.id, finger.x, finger.y });
            }
            return state;
        });
        dx::console::debugOverlayHook = [this](const std::string& argument) -> std::string {
            std::string word = dx::lowered(argument);
            auto isOn = [](const std::string& value) {
                return value == "on" || value == "1" || value == "true";
            };
            auto isOff = [](const std::string& value) {
                return value == "off" || value == "0" || value == "false";
            };
            // "DebugOverlay <switch> on|off" sets one of the sections' switches, and
            // "DebugOverlay <command> <value>" runs one of their commands.
            // Marks show only while the overlay does, so marking with it hidden shows it passive,
            // with its tool windows closed so they don't cover the view.
            auto showMarks = [this] {
                if (overlay->mode() == dx::debugui::Overlay::Mode::Hidden) {
                    setOverlayMode(dx::debugui::Overlay::Mode::Passive);
                    std::string ignored;
                    overlay->runCommand("close", "all", ignored);
                }
            };
            size_t space = word.find(' ');
            if (space != std::string::npos) {
                std::string name = word.substr(0, space);
                std::string value = word.substr(word.find_first_not_of(' ', space));
                if ((isOn(value) || isOff(value)) && overlay->setSwitch(name, isOn(value))) {
                    if ((name == "light-markers" || name == "mirror-markers") && isOn(value)) {
                        showMarks();
                    }
                    return name + (isOn(value) ? " on" : " off");
                }
                std::string reply;
                if (overlay->runCommand(name, value, reply)) {
                    if (name == "mark") {
                        showMarks();
                    }
                    return reply;
                }
                std::string names;
                for (const std::string& known : overlay->switchNames()) {
                    names += " " + known;
                }
                std::string commands;
                for (const std::string& known : overlay->commandNames()) {
                    commands += " " + known;
                }
                return "DebugOverlay <switch> on|off, with a switch from:" + names + "; or DebugOverlay <command> <value>, with a command from:" + commands;
            }
            // With nothing after it, DebugOverlay opens the overlay (interactive) when hidden,
            // and switches it between interactive and passive while shown.
            using Mode = dx::debugui::Overlay::Mode;
            Mode mode = overlay->mode() == Mode::Interactive ? Mode::Passive : Mode::Interactive;
            if (isOn(word) || word == "interactive") {
                mode = Mode::Interactive;
            } else if (isOff(word)) {
                mode = Mode::Hidden;
            } else if (word == "passive") {
                mode = Mode::Passive;
            } else if (!word.empty()) {
                return "DebugOverlay takes on, off, passive, interactive or nothing to switch, or a switch and on or off";
            }
            setOverlayMode(mode);
            return mode == Mode::Hidden ? "debug overlay hidden" : (mode == Mode::Passive ? "debug overlay passive" : "debug overlay interactive");
        };
        dx::audio::applySettings();
        if (!options.automated() && !options.sizeGiven) {
            bool fullscreen = dx::settings::get(dx::settings::startupFullscreen);
            dx::resolutions::Size size = fullscreen ? fullscreenSize() : windowedSize();
            applyVideoMode(fullscreen, size.width, size.height);
            if (!fullscreen) {
                SDL_SyncWindow(window);
                centreWindow();
            }
        }
        if (!options.hidden) {
            // Black before and after the window appears, so neither buffer shows through.
            for (int pass = 0; pass < 2; pass++) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                SDL_GL_SwapWindow(window);
                if (pass == 0) {
                    SDL_ShowWindow(window);
                    SDL_SyncWindow(window);
                }
            }
        }
        dx::CanvasHooks& hooks = dx::canvasHooks();
        hooks.target = &renderer->canvas();
        hooks.drawActor = [this](dx::UObject* actor, bool) {
            renderer->drawActorOverlay(world.get(), actor, viewLocation, viewRotation, viewFov, true);
        };
        hooks.clipboardCopy = [](const std::string& text) {
            SDL_SetClipboardText(text.c_str());
        };
        hooks.clipboardPaste = []() {
            return dx::sdl::clipboardText();
        };
        dx::extension::setFrameCanvas(&renderer->canvas());
        dx::extension::setNavigationSkip(dx::gamepadmenu::skipForNavigation);
        dx::extension::setOwnFocusLook(dx::gamepadmenu::ownFocusLook);
        dx::extension::setNavigationWrap(dx::ui::isMenuWindow);
        dx::extension::setWindowBackdrop(dx::deusex::currentTabBackdrop);
        dx::extension::setOverlayDrawer([](dx::Canvas2D& canvas) {
            dx::padlayout::draw(canvas);
            UObject* top = extension::topModal();
            if (!script::isA(top, "MenuSettings")) {
                return;
            }
            auto panel = extension::windowArea(top);
            UFont* font = ui::font("DeusExUI.FontMenuSmall");
            if (!panel || !font) {
                return;
            }
            // MenuMain aligns its own stamp to the bottom right of the menu window.
            float scale = extension::uiScale();
            const char* stamp = version::label();
            float x = (panel->x + panel->width) * scale - textWidth(font, stamp, 0, scale);
            float y = (panel->y + panel->height) * scale - textHeight(font, scale);
            drawText(canvas, font, stamp, x, y, ui::theme(hud::viewer(), true).text, BlendStyle::Masked, 0, scale);
        });
        dx::extension::setButtonHook([this](dx::UObject* button) {
            if (dx::padlayout::isCalibrateButton(button)) {
                pad.startGyroCalibration();
                return true;
            }
            return dx::padlayout::buttonPressed(button) || dx::padinventory::buttonPressed(button) || dx::terminals::buttonPressed(button);
        });
        dx::extension::setMouseReleaseHook(dx::padinventory::mouseReleased);
        dx::gamestate::install();
        dx::extension::setProjector([this](const dx::Vector& location, float& pixelX, float& pixelY) {
            return project(location, pixelX, pixelY);
        });
        dx::extension::setActorDrawer([this](dx::Canvas2D&, const dx::extension::ActorDrawRequest& request) {
            if (request.actor) {
                renderer->drawActorOverlay(world.get(), request.actor, viewLocation, viewRotation, viewFov, request.clearZ);
            }
        });
        dx::extension::setViewportDrawer([this](dx::Canvas2D&, const dx::extension::ViewportRequest& request) {
            if (!world || !world->level) {
                return;
            }
            dx::UObject* hidden = request.showActor ? nullptr : request.originActor;
            int x = static_cast<int>(std::lround(request.x));
            int y = static_cast<int>(std::lround(request.y));
            int width = static_cast<int>(std::lround(request.width));
            int height = static_cast<int>(std::lround(request.height));
            renderer->drawSceneRect(world.get(), world->level, world->actors, request.location, request.rotation, request.fov, hidden, x, y, width, height);
        });
        DX_GPU_CONTEXT();
        loadDeusExClasses();
        // Before the menu changes: the Gamepad screen takes rows it makes.
        dx::padlayout::install();
        addFieldOfViewChoice();
        addExtraOptions();
        addControllerOptions();
        addMixerChoice();
        dx::deusex::removeIgnoredChoices();
        removeUnavailableOptions();
        // After every other change, so the layout it looks for is the one the screens end up with.
        dx::deusex::applyGeneratedMenuArt();
        if (!options.dumpMenuLayouts.empty()) {
            dx::deusex::dumpMenuLayouts(options.dumpMenuLayouts);
        }
        dx::deusex::skipMissionScripts = options.noMissionScript;
        dx::deusex::skipTriggers = options.noTriggers;
        std::string map = options.map;
        std::filesystem::path mapFile = map;
        if (mapFile.has_parent_path() && std::filesystem::is_regular_file(mapFile)) {
            map = mapFile.stem().string();
            dx::objects::loadPackageFile(dx::Name(map), mapFile, false);
            fileMap = map;
        }
        if (map.empty()) {
            map = entryMap();
        }
        return openMap(map);
    }
}
