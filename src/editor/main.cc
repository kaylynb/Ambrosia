#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/random.h"
#include "debugui/imgui_setup.h"
#include "editor/editor.h"

#include <SDL3/SDL_main.h>

#include <tabler_icons.h>

#include <CLI11.hpp>

#include <memory>
#include <optional>

namespace {
    // Reads the command line into options: nothing when the editor should start, otherwise the
    // code to exit with (cli::parse).
    std::optional<int> parseOptions(int argc, char** argv, dx::editor::Options& options)
    {
        CLI::App app("DentEd, the Ambrosia level editor.", "dented");
        app.add_option("map", options.map, "Map to open: a map's name, or the path of a .dx file")->capture_default_str();
        app.add_option("--new", options.blankMap, "Start on a new, empty map of this name instead")->type_name("NAME");
        dx::cli::addRunOptions(app, options);
        return dx::cli::parse(app, argc, argv);
    }
}

int main(int argc, char** argv)
{
    if (dx::cli::printVersion(argc, argv)) {
        return 0;
    }
    // DentEd.log in the user directory, and a console with --console.
    dx::log::startProgram(dx::config::userDirectory() / "DentEd.log", dx::log::consoleRequested(argc, argv));
    dx::random::seed(dx::random::clockSeed());
    dx::editor::Options options;
    if (std::optional<int> exit = parseOptions(argc, argv, options)) {
        return *exit;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        dx::log::error("Critical", "SDL_Init failed: {}", SDL_GetError());
        return 1;
    }

    // SDL, the window and its context are released in reverse order however main returns.
    struct SdlQuit {
        ~SdlQuit()
        {
            SDL_Quit();
        }
    } sdlQuit;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> windowOwner(SDL_CreateWindow("DentEd", options.width, options.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (options.hidden ? SDL_WINDOW_HIDDEN : 0)), &SDL_DestroyWindow);
    SDL_Window* window = windowOwner.get();
    if (!window) {
        dx::log::error("Critical", "SDL_CreateWindow failed: {}", SDL_GetError());
        return 1;
    }
    dx::platform::setWindowIcon(window);
    std::unique_ptr<SDL_GLContextState, decltype(&SDL_GL_DestroyContext)> contextOwner(SDL_GL_CreateContext(window), &SDL_GL_DestroyContext);
    SDL_GLContext context = contextOwner.get();
    if (!context || !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        dx::log::error("Critical", "failed to create OpenGL 3.3 context");
        return 1;
    }
    SDL_GL_SetSwapInterval(options.automated() ? 0 : 1);
    dx::debugui::startImGui(window, context);
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.GlyphMinAdvanceX = 13.0f;
    iconConfig.GlyphOffset = ImVec2(0.0f, 1.0f);
    ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(TablerIcons_compressed_data, static_cast<int>(TablerIcons_compressed_size), 13.0f, &iconConfig);
    std::filesystem::path gamePath = "deusex";
    if (const char* base = SDL_GetBasePath()) {
        gamePath = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base))) / "deusex";
    }
    int result = 0;
    try {
        dx::editor::Editor editor(window, options, gamePath);
        editor.start();
        editor.run();
        if (editor.passedExpectations + editor.failedExpectations > 0) {
            dx::log::info("Log", "expectations: {} passed, {} failed", editor.passedExpectations, editor.failedExpectations);
        }
        result = editor.failedExpectations > 0 ? 1 : 0;
    } catch (const std::exception& failure) {
        dx::log::error("Critical", "error: {}", failure.what());
        result = 1;
    }
    dx::debugui::stopImGui();
    return result;
}
