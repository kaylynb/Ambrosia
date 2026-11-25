#include "game/game.h"

#include "deusex/map_lookup.h"
#include "audio/audio.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/objects.h"
#include "core/sdl_handles.h"
#include "core/profile.h"
#include "core/vm.h"
#include "deusex/deusex.h"
#include "deusex/reading.h"
#include "deusex/terminals.h"
#include "engine/actor.h"
#include "engine/canvas_hooks.h"
#include "engine/console.h"
#include "engine/world.h"
#include "extension/extension.h"
#include "game/console_report.h"
#include "game/platform.h"
#include "render/gpu_profile.h"
#include "render/gpu_timers.h"
#include "render/render_debug.h"
#include "render/scene_renderer.h"
#include "core/text.h"

#include <SDL3/SDL.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {
    // Milliseconds of CPU the calling thread has run since it started, on every core, or a
    // negative number where the platform does not say. For telling a frame's work from its waits
    // on the console, where the main thread may be idle while the frame is long.
    double threadCpuMilliseconds()
    {
#if defined(__SWITCH__)
        u64 ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, static_cast<u64>(-1)))) {
            return -1;
        }
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(armGetSystemTickFreq());
#else
        return -1;
#endif
    }
}

namespace dx::game {
    bool Game::project(const dx::Vector& location, float& pixelX, float& pixelY)
    {
        dx::Coords axes = dx::rotationAxes(viewRotation);
        dx::Vector delta = location - viewLocation;
        float forward = delta.dot(axes.xAxis);
        if (forward <= 1.0f) {
            return false;
        }
        float right = delta.dot(axes.yAxis);
        float up = delta.dot(axes.zAxis);
        float halfWidth = static_cast<float>(frameWidth) * 0.5f;
        float halfHeight = static_cast<float>(frameHeight) * 0.5f;
        float scale = halfWidth / std::tan(viewFov * dx::Pi / 360.0f);
        pixelX = halfWidth + right / forward * scale;
        pixelY = halfHeight - up / forward * scale;
        return true;
    }

    dx::UObject* Game::canvas(int width, int height)
    {
        if (!canvasObject) {
            canvasObject = dx::objects::newObject(dx::engineClasses().canvas, dx::objects::packageObject(dx::Name("Transient")), dx::Name("Canvas0"), 0);
        }
        // Made once, as the original's UViewport makes its canvas; the viewport is made by the
        // first level's login, which may come after.
        dx::canvas::Viewport(canvasObject) = world->viewport;
        dx::vm::event(canvasObject, dx::Name("Reset"));
        dx::canvas::SizeX(canvasObject) = width;
        dx::canvas::SizeY(canvasObject) = height;
        dx::canvas::ClipX(canvasObject) = static_cast<float>(width);
        dx::canvas::ClipY(canvasObject) = static_cast<float>(height);
        return canvasObject;
    }

    dx::UObject* Game::scriptConsole()
    {
        if (!world || !world->viewport || !world->player()) {
            return nullptr;
        }
        return dx::player::Console(world->viewport);
    }

    void Game::render(int width, int height)
    {
        frameWidth = width;
        frameHeight = height;
        dx::extension::setViewportSize(static_cast<float>(width), static_cast<float>(height));
        if (world) {
            renderer->setTime(world->timeSeconds);
        }
        renderer->setPaused(world && world->paused);
        renderer->setViewportTime(viewportTime);
        DX_ZONE_NAMED("Render");
        // The HUD's GPU time runs from its scripts' drawing to the canvas flush.
        std::optional<dx::gputimers::Scope> hudGpu;
        renderer->beginFrame(width, height);
        viewLocation = {};
        viewRotation = {};
        viewFov = 90;
        if (world && world->calcView(viewLocation, viewRotation, viewFov)) {
            viewFov = displayFov(viewFov, width, height);
            dx::UObject* player = world->player();
            dx::UObject* viewActor = nullptr;
            if (player && !dx::pawn::bBehindView(player)) {
                viewActor = dx::playerpawn::ViewTarget(player) ? dx::playerpawn::ViewTarget(player) : player;
            }
            {
                DX_ZONE_NAMED("Scene");
                renderer->drawScene(world.get(), world->level, world->actors, viewLocation, viewRotation, viewFov, viewActor);
            }
            if (progressFrame) {
                // PaintProgress draws the world at a tenth of its brightness; the HUD and the
                // console's message on top are not dimmed.
                renderer->canvas().drawRect(0, 0, renderer->canvas().width(), renderer->canvas().height(), { 0, 0, 0, 0.9f }, dx::BlendStyle::Normal);
            }
            dx::UObject* canvasActor = canvas(width, height);
            hudGpu.emplace(dx::gputimers::Stage::Hud);
            if (player && !player->destroyed) {
                DX_ZONE_NAMED("HUD scripts");
                // The Render tool can leave out the HUD's script drawing, but not the console's.
                if (dx::renderDebug.hudScripts) {
                    {
                        DX_ZONE_NAMED("HUD PreRender");
                        dx::vm::Call(player, dx::Name("PreRender")).obj(canvasActor).run();
                    }
                    if (viewActor) {
                        DX_ZONE_NAMED("HUD RenderOverlays");
                        dx::vm::Call(viewActor, dx::Name("RenderOverlays")).obj(canvasActor).run();
                    }
                    {
                        DX_ZONE_NAMED("HUD PostRenderFlash");
                        dx::vm::Call(player, dx::Name("PostRenderFlash")).obj(canvasActor).run();
                    }
                    {
                        DX_ZONE_NAMED("HUD PostRender");
                        dx::vm::Call(player, dx::Name("PostRender")).obj(canvasActor).run();
                    }
                }
                dx::UObject* console = scriptConsole();
                if (console) {
                    DX_ZONE_NAMED("HUD console PostRender");
                    dx::console::FrameX(console) = static_cast<float>(width);
                    dx::console::FrameY(console) = static_cast<float>(height);
                    dx::canvasHooks().textScale = progressFrame ? dx::extension::uiScale() : 1.0f;
                    dx::vm::Call(console, dx::Name("PostRender")).obj(canvasActor).run();
                    dx::canvasHooks().textScale = 1;
                }
                pad.draw(renderer->canvas(), player);
            }
        }
        DX_ZONE_NAMED("Canvas flush");
        renderer->endFrame();
        hudGpu.reset();
    }

    void Game::setOverlayMode(dx::debugui::Overlay::Mode mode)
    {
        if (mode == overlay->mode()) {
            return;
        }
        // The label names the key bound to DebugOverlay, if any.
        std::string key;
        for (int code = 1; code < 256 && key.empty(); code++) {
            std::string command = dx::lowered(dx::console::binding(static_cast<uint8_t>(code)));
            if (command.find("debugoverlay") != std::string::npos && command.find("off") == std::string::npos) {
                key = dx::console::keyName(static_cast<uint8_t>(code));
            }
        }
        overlay->setKeyHint(key);
        overlay->setMode(mode);
        if (options.hidden) {
            return;
        }
        // Interactive, the overlay takes the mouse as a normal cursor; passive or hidden, the
        // game has the hold it had, and the next step follows any GUI change made meanwhile.
        if (mode == dx::debugui::Overlay::Mode::Interactive) {
            SDL_SetWindowRelativeMouseMode(window, false);
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
            SDL_SetWindowRelativeMouseMode(window, !guiMode);
        }
    }

    void Game::drawOverlay(int width, int height)
    {
        if (!overlay->isOpen()) {
            return;
        }
        dx::debugui::Frame view;
        view.world = world.get();
        view.player = world ? world->player() : nullptr;
        view.viewLocation = viewLocation;
        view.viewRotation = viewRotation;
        view.fov = viewFov;
        view.renderer = renderer.get();
        overlay->draw(view, width, height);
    }

    void Game::begin()
    {
        if (options.debugOverlay) {
            setOverlayMode(dx::debugui::Overlay::Mode::Interactive);
        }
        last = SDL_GetPerformanceCounter();
    }

    int Game::run()
    {
        begin();
        while (running) {
            step();
        }
        // The last level's memory record, for playtests, as each level change leaves one.
        if (world && world->level) {
            logLevelMemory(dx::maplookup::mapName(world->url), true);
        }
        // A resize still settling when the game quits is kept too.
        if (resizeSaveAt != 0) {
            resizeSaveAt = 1;
            saveSettledResize();
        }
        return 0;
    }

    void Game::step()
    {
        DX_THREAD_NAME("Main");
        DX_FRAME_ZONE("Frame");
        {
            DX_ZONE_NAMED("Events");
            processEvents();
            platform::readTouch(touch);
        }

        bool gui = dx::extension::wantsMouse();
        // While the developer overlay is open it keeps the mouse.
        if (gui != guiMode && !options.hidden && overlay->mode() != dx::debugui::Overlay::Mode::Interactive) {
            guiMode = gui;
            SDL_SetWindowRelativeMouseMode(window, !gui);
            float cursorX = 0;
            float cursorY = 0;
            if (gui && dx::extension::cursorPosition(cursorX, cursorY)) {
                int windowWidth = 0;
                int windowHeight = 0;
                SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                int drawWidth = 0;
                int drawHeight = 0;
                SDL_GetWindowSizeInPixels(window, &drawWidth, &drawHeight);
                if (drawWidth > 0 && drawHeight > 0) {
                    SDL_WarpMouseInWindow(window, cursorX * static_cast<float>(windowWidth) / static_cast<float>(drawWidth), cursorY * static_cast<float>(windowHeight) / static_cast<float>(drawHeight));
                }
            }
        }
        uint64_t now = SDL_GetPerformanceCounter();
        float delta = static_cast<float>(now - last) / static_cast<float>(SDL_GetPerformanceFrequency());
        last = now;
        if (options.automated()) {
            delta = 1.0f / 30.0f;
        }
        viewportTime += static_cast<double>(delta);
        // A point for the Memory window's plot, four a second: two counters copied.
        dx::gc::sampleMemory();
        {
            DX_ZONE_NAMED("Pad update");
            pad.update(world.get(), delta);
        }
        updateTextInput();
        saveSettledResize();
        {
            DX_ZONE_NAMED("Settings sync");
            // SDL applies fullscreen changes asynchronously, so follow the window rather than
            // the toggle.
            bool fullscreen = isFullscreen();
            std::string state = dx::console::currentResHook() + (fullscreen ? " fullscreen" : "");
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            monitorHz = mode && mode->refresh_rate > 0 ? static_cast<int>(std::lround(mode->refresh_rate)) : 60;
            dx::deusex::syncResolutionChoice(state, fullscreen && !exclusiveFullscreen(), monitorHz);
            dx::deusex::syncMixerChoice();
            dx::deusex::syncAimAssistChoice();
        }
        applyRenderOptions();
        applyControllerOptions();
        dx::frameTiming = dx::settings::get(dx::settings::frameTiming) == 1 ? dx::FrameTiming::Original : dx::FrameTiming::Steady;
        {
            DX_ZONE_NAMED("Terminals and reading");
            dx::terminals::tick(delta);
            dx::reading::tick();
        }
        // A zero-delta tick can still advance scripts (and original timing clamps it up),
        // so do not tick the world at all once a radial has eased into its full pause.
        if (cameraHold.active && cameraHold.releaseFrame >= 0 && frame >= cameraHold.releaseFrame) {
            cameraHold.active = false;
        }
        if (cameraHold.active) {
            holdCamera();
        }
        if (world && pad.timeScale() > 0) {
            DX_ZONE_NAMED("World tick");
            world->tick(delta * pad.timeScale());
        } else {
            // World::tick normally resets this per frame; UI scripts still run frozen.
            dx::vm::resetRunaway();
        }
        {
            DX_ZONE_NAMED("Extension tick");
            dx::extension::tick(delta);
        }
        dx::UObject* console = scriptConsole();
        if (console) {
            DX_ZONE_NAMED("Script console tick");
            dx::vm::Call(console, dx::Name("Tick")).f(delta).run();
            int height = 0;
            SDL_GetWindowSizeInPixels(window, nullptr, &height);
            dx::console::ConsoleLines(console) = static_cast<int32_t>(std::min(dx::console::ConsolePos(console) * static_cast<float>(height), static_cast<float>(height)));
        }
        handleTravel();
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        render(std::max(width, 1), std::max(height, 1));
        {
            DX_ZONE_NAMED("Gamma");
            renderer->applyBrightness(dx::settings::get(dx::settings::brightness));
        }
        dx::audio::update(world.get(), viewLocation, viewRotation, delta);
        {
            DX_ZONE_NAMED("Debug overlay");
            drawOverlay(std::max(width, 1), std::max(height, 1));
        }
        if (!pendingShot.empty()) {
            std::vector<uint8_t> pixels;
            int shotWidth = 0;
            int shotHeight = 0;
            renderer->readPixels(pixels, shotWidth, shotHeight);
            if (!dx::sdl::saveBmp(pendingShot, pixels, shotWidth, shotHeight)) {
                dx::log::error("Log", "cannot save {}: {}", pendingShot, SDL_GetError());
            }
            pendingShot.clear();
        }
        frame++;
        // The level is loaded once no level change is waiting; one made this frame was drawn.
        steps.tick([this] { return world && world->level && !world->travelPending && pendingMap.empty(); }, [this](const std::string& step) { scriptedAction(step); });
        // A quit waits for a shot asked for just before it, which the next frame saves.
        if ((steps.quitting() && pendingShot.empty()) || (options.frames >= 0 && frame >= options.frames)) {
            running = false;
        }
        double swapCpuStart = threadCpuMilliseconds();
        {
            DX_ZONE_NAMED("Swap");
            SDL_GL_SwapWindow(window);
        }
        // The main thread's CPU time over the whole frame (mark to mark) and in the swap; with the
        // frame's own time these say how much of it the thread spent running and how much waiting.
        // Only where the platform reports thread time (the Switch).
        if (swapCpuStart >= 0) {
            static double lastFrameCpu = -1;
            double frameCpuEnd = threadCpuMilliseconds();
            DX_PLOT("Main thread CPU ms", lastFrameCpu >= 0 ? frameCpuEnd - lastFrameCpu : 0);
            DX_PLOT("Swap CPU ms", frameCpuEnd - swapCpuStart);
            lastFrameCpu = frameCpuEnd;
        }
        DX_GPU_COLLECT();
        dx::gputimers::endFrame();
        DX_PLOT("Objects", dx::objects::all().size());
        DX_PLOT("Draw calls", dx::renderCounters.drawCalls);
        DX_PLOT("Triangles", dx::renderCounters.triangles);
        DX_PLOT("Visible surfaces", dx::renderCounters.visibleSurfaces);
        DX_PLOT("Visible actors", dx::renderCounters.visibleActors);
        DX_PLOT("Texture uploads", dx::renderCounters.textureUploads);
        DX_PLOT("Lightmap updates", dx::renderCounters.lightmapUpdates);
        DX_FRAME_MARK();
        dx::game::report::frameDone();
        dx::log::tick();
        DX_ZONE_NAMED("Frame rate limit");
        limitFrameRate();
    }
}
