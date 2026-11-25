#pragma once

#include <cstdint>

namespace dx {
    // Developer overrides for bisecting render cost on slow hardware, set by the Render tool and
    // its console switches. Every one defaults to the normal path; none is saved to the ini.
    // Those that change what is drawn can change actor::LastRenderTime, which some scripts read.
    struct RenderDebug {
        // Reflections in mirror surfaces; off, a mirror surface draws as its own texture.
        bool mirrors = true;
        // The sky zone's view; off, fake backdrop surfaces draw as their own texture.
        bool sky = true;
        // Lightmap animation; off, animated lightmaps keep the frame they had.
        bool lightmapAnimation = true;
        // Procedural texture updates (fire, water, wet); off, each keeps the frame it had.
        bool proceduralTextures = true;
        // Actors drawn as meshes; off, they are not drawn (their LastRenderTime is still set).
        bool actorMeshes = true;
        // The translucent pass, for both actors and level surfaces.
        bool translucent = true;
        // Hardware occlusion queries that leave out actors hidden behind others; off, every
        // actor in view is drawn.
        bool occlusionQueries = true;
        bool coronas = true;
        // The post chain as a whole: multisampling, ambient occlusion and FXAA, each only when its
        // video setting asks for it (the Render tool sets those settings themselves).
        bool postChain = true;
        // The brightness (gamma) pass.
        bool gamma = true;
        // The HUD's script drawing: PreRender, RenderOverlays, PostRenderFlash and PostRender.
        // The console's own drawing stays, so it can be typed into to turn this back on.
        bool hudScripts = true;
    };

    inline RenderDebug renderDebug;

    // What the renderer did in a frame, counted from one SceneRenderer::beginFrame to the next.
    // Counts are summed over every view drawn: the main view, mirrors and the sky.
    struct RenderCounters {
        // Surfaces of the level found visible.
        uint32_t visibleSurfaces = 0;
        // Actors left to draw after the frustum, zone and occlusion tests.
        uint32_t visibleActors = 0;
        // GL draw calls of the scene, the canvas and the full-screen passes; a multi-draw of
        // several ranges counts once.
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
        // Textures uploaded whole, procedural textures included.
        uint32_t textureUploads = 0;
        uint32_t proceduralUpdates = 0;
        // Lightmap and fog map regions uploaded after a change.
        uint32_t lightmapUpdates = 0;
    };

    // The frame under way, and the last whole frame.
    inline RenderCounters renderCounters;
    inline RenderCounters lastRenderCounters;

    // A draw call of this many triangle vertices (0 for lines).
    inline void countDraw(int64_t triangleVertices)
    {
        renderCounters.drawCalls++;
        renderCounters.triangles += static_cast<uint32_t>(triangleVertices / 3);
    }
}
