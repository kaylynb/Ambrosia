#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glad/gl.h>

namespace dx::test {
    // A hidden window and an OpenGL 3.3 context for the tests that draw through GL, made on
    // construction and released at the end. context is null when there is no 3.3 context to be
    // had, which the tests skip on: a display with no 3.3 driver can still give a 1.1 context,
    // and the 3.3 entry points would then be null.
    class OffscreenContext {
    public:
        OffscreenContext(const char* title, int width, int height)
        {
            SDL_SetMainReady();
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                return;
            }
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            window = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            if (!window) {
                return;
            }
            context = SDL_GL_CreateContext(window);
            if (context && gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)) < GLAD_MAKE_VERSION(3, 3)) {
                SDL_GL_DestroyContext(context);
                context = nullptr;
            }
        }

        ~OffscreenContext()
        {
            if (context) {
                SDL_GL_DestroyContext(context);
            }
            if (window) {
                SDL_DestroyWindow(window);
            }
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }

        OffscreenContext(const OffscreenContext&) = delete;
        OffscreenContext& operator=(const OffscreenContext&) = delete;

        SDL_Window* window = nullptr;
        SDL_GLContext context = nullptr;
    };
}
