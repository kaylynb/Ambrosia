#define SDL_MAIN_HANDLED

#include "offscreen_gl.h"
#include "render/gl_shader.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <catch_amalgamated.hpp>
#include <glad/gl.h>

#include <tuple>

namespace {
    constexpr int frameWidth = 320;
    constexpr int frameHeight = 240;

    float centerDepth()
    {
        float depth = -1;
        glReadPixels(frameWidth / 2, frameHeight / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        return depth;
    }
}

TEST_CASE("the multisampled and occlusion scene passes hand their depth to the target for later overlays")
{
    dx::test::OffscreenContext gl("scene pass", frameWidth, frameHeight);
    if (!gl.context) {
        SKIP("no OpenGL 3.3 context");
    }
    dx::GammaPass gamma;
    dx::ScenePass pass;
    while (glGetError() != GL_NO_ERROR) {
    }
    auto [samples, occlusion] = GENERATE(std::make_tuple(4, false), std::make_tuple(0, true), std::make_tuple(4, true));
    gamma.bindTarget(frameWidth, frameHeight);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    pass.begin(frameWidth, frameHeight, samples, occlusion);
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(1.0);
    pass.end(frameWidth, frameHeight, false);
    CHECK(centerDepth() == Catch::Approx(0.25).margin(1e-4));
    CHECK(glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
