#include "game/game.h"
#include "audio/audio.h"
#include "core/random.h"
#include "core/vm.h"
#include "core/workers.h"
#include "engine/canvas_hooks.h"
#include "engine/console.h"
#include "engine/savegame.h"
#include "core/config.h"
#include "core/platform.h"
#include "game/console_report.h"
#include "game/platform.h"
#include "extension/extension.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glad/gl.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <exception>
#include <optional>
#include <string>

using dx::game::Game;
using dx::game::Options;
using dx::game::parseOptions;

int main(int argc, char** argv)
{
    if (dx::cli::printVersion(argc, argv)) {
        return 0;
    }
    // An early exit says why: in the log, and on screen unless a script reads the log.
    auto fail = [](const std::string& message, int code) {
        dx::game::report::fatal(message);
        return code;
    };
    if (std::optional<int> stop = dx::game::platform::start(argc, argv)) {
        return *stop;
    }
    dx::random::seed(dx::random::clockSeed());
    Options options;
    if (std::optional<int> exit = parseOptions(argc, argv, options)) {
        // The log has the options, or what is wrong with them.
        if (*exit != 0) {
            dx::game::platform::reportInvalidOptions();
        }
        return *exit;
    }
    if (options.seed) {
        dx::random::seed(static_cast<uint64_t>(*options.seed));
    }
    if (options.clearShaderCache) {
        dx::game::platform::clearShaderCache();
    }
    // Said up front (the Switch says it in platform::start, with its own path): otherwise the
    // first sign is the engine failing to load its Core package, which names no game files.
    if (dx::config::findGameRoot().empty()) {
        return fail("The Deus Ex game files were not found. Copy the contents of your Deus Ex folder (System, Maps, Textures and the others) into a folder named game-data next to Ambrosia, or set the DEUSEX_GAME_DATA environment variable to your Deus Ex folder.", 1);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        return fail(std::string("SDL_Init failed: ") + SDL_GetError(), 1);
    }

    // SDL, the window and its context are released in reverse order however main returns.
    struct SdlQuit {
        ~SdlQuit()
        {
            SDL_Quit();
        }
    } sdlQuit;

    dx::game::report::stage("SDL initialised, video driver %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // Created hidden: the game shows it once it has the size from the ini and a black frame to
    // put in it, so the window never appears at a size it is about to leave.
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> windowOwner(SDL_CreateWindow("Ambrosia", options.width, options.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN), &SDL_DestroyWindow);
    SDL_Window* window = windowOwner.get();
    if (!window) {
        return fail(std::string("SDL_CreateWindow failed: ") + SDL_GetError(), 1);
    }
    dx::game::report::stage("window %dx%d created", options.width, options.height);
    dx::platform::setWindowIcon(window);
    std::unique_ptr<SDL_GLContextState, decltype(&SDL_GL_DestroyContext)> context(SDL_GL_CreateContext(window), &SDL_GL_DestroyContext);
    if (!context) {
        return fail(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError(), 1);
    }
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        return fail("failed to load OpenGL", 1);
    }
    dx::game::report::stage("OpenGL %s, %s, %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    SDL_GL_SetSwapInterval(options.automated() ? 0 : 1);
    dx::game::platform::startTextInput(window);
    SDL_HideCursor();
    dx::vm::setCriticalHandler([](const std::string& message) {
        // In the log, and in a message box unless a script reads the log (report::fatal).
        dx::game::report::fatal("Critical Error: " + message);
        std::_Exit(1);
    });
    int result = 0;
    try {
        // The game hands the engine callbacks that point back at it. Declared first, so it runs
        // after the game is gone and nothing is left pointing at it.
        struct HookReset {
            ~HookReset()
            {
                dx::console::openMapHook = nullptr;
                dx::console::quitHook = nullptr;
                dx::console::currentResHook = nullptr;
                dx::console::resolutionsHook = nullptr;
                dx::console::setResHook = nullptr;
                dx::console::toggleFullscreenHook = nullptr;
                dx::console::screenshotHook = nullptr;
                dx::console::debugOverlayHook = nullptr;
                dx::canvasHooks() = {};
                dx::savegame::setProgressPainter({});
                dx::extension::setFrameCanvas(nullptr);
                dx::extension::setButtonHook({});
                dx::extension::setMouseReleaseHook({});
                dx::extension::setProjector({});
                dx::extension::setActorDrawer({});
                dx::extension::setViewportDrawer({});
            }
        } hookReset;

        Game game(window, options);
        if (!game.start()) {
            result = fail("failed to start game", 1);
        } else {
            dx::game::report::stage("game started");
            result = game.run();
        }
    } catch (const std::exception& error) {
        result = fail(std::string("error: ") + error.what(), 1);
    }
    dx::game::report::stage("exiting with %d", result);
    dx::audio::shutdown();
    // Audio has stopped its own workers; any other still running stops before the statics go.
    dx::workers::stopAll();
    return result;
}
