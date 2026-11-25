#pragma once

#include "render/gl_canvas.h"
#include "render/gl_shader.h"
#include "render/render_device.h"
#include "render/texture_cache.h"

#include <memory>

namespace dx {
    class GLRenderDevice {
    public:
        GLRenderDevice();
        ~GLRenderDevice();
        GLRenderDevice(const GLRenderDevice&) = delete;
        GLRenderDevice& operator=(const GLRenderDevice&) = delete;

        bool drawingToWindow();
        void bindGammaTarget(int width, int height);
        void resolveGamma(int width, int height, float exponent);
        void applyGamma(int width, int height, float exponent);
        void keepFrame(int width, int height);
        bool readKeptFrame(std::vector<uint8_t>& rgba, int& width, int& height);
        void beginScene(int width, int height, int samples, OcclusionQuality ambientOcclusion);
        void endScene(int width, int height, bool fxaa);
        void beginOcclusion();
        void setOcclusionSource(OcclusionSource source);
        void applyOcclusion(float nearPlane, float farPlane, float tanHalfWidth, float tanHalfHeight);
        void readPixels(int width, int height, uint8_t* rgba);

        void setViewport(int x, int y, int width, int height);
        void enableScissor(int x, int y, int width, int height);
        void disableScissor();
        void clear(bool color, bool depth, float depthValue);
        void setDepthTest(bool enabled);
        void setDepthWrite(bool enabled);
        void setDepthFunc(DepthFunc func);
        void setDepthRange(float nearValue, float farValue);
        void setColorWrite(bool enabled);
        void setCulling(bool enabled);
        void setFrontFace(bool clockwise);
        void setWireframe(bool enabled);
        void setPolygonOffset(bool enabled, float factor, float units);
        void setBlend(BlendMode mode);
        void setClipPlanes(const float* planes, int32_t count);

        void useShader();
        void setViewProj(const Mat4& matrix);
        void setFlags(int32_t flags);
        void setQuality(bool smoothLightmap, float detailDistance);
        void setPan(float u, float v);
        void setScale(float u, float v);
        void setTint(float r, float g, float b, float a);
        void setEye(const Vector& eye, const Vector& forward);
        void setDynamicLights(const std::vector<DynamicLight>& lights);
        void bindTexture(uint32_t texture);
        void bindLightmap(uint32_t texture);

        void draw(const std::vector<GLVertex>& vertices, Primitive primitive);
        void upload(const std::vector<GLVertex>& vertices);
        void drawRange(int32_t first, int32_t count);
        StaticGeometry createStatic(const std::vector<GLVertex>& vertices);
        void bindStatic(uint32_t geometry);
        void drawStatic(uint32_t geometry, int32_t first, int32_t count);
        void drawStaticRanges(uint32_t geometry, const int32_t* firsts, const int32_t* counts, int32_t ranges);

        QueryHandle createQuery();
        void beginQuery(uint32_t query);
        void endQuery();
        bool queryResult(uint32_t query, bool& visible);

        void beginTextureFrame(double time);
        UTexture* animated(UTexture* texture);
        const GLTextureInfo* texture(UTexture* texture);
        uint32_t whiteTexture();
        void setTextureOptions(float anisotropy, bool trilinear, float rate);
        void clearTextures();
        uint64_t textureGeneration() const;
        bool holdsTexture(const UObject* texture) const;

        TextureHandle createLightmap(int32_t size, const uint8_t* rgba);
        void updateLightmap(uint32_t texture, int32_t x, int32_t y, int32_t width, int32_t height, const uint8_t* rgba);
        // Deletes every page in one call and empties the list.
        void deleteLightmaps(std::vector<TextureHandle>& pages);

        Canvas2D& canvas();
        void beginCanvas(int width, int height);
        void flushCanvas();

    private:
        std::unique_ptr<UberShader> shader;
        std::unique_ptr<GammaPass> gammaPass;
        std::unique_ptr<ScenePass> scenePass;
        std::unique_ptr<OcclusionPass> occlusionPass;
        std::unique_ptr<TextureCache> textures;
        std::unique_ptr<GLCanvas> canvasImpl;
        int sceneWidth = 0;
        OcclusionQuality occlusionQuality = OcclusionQuality::Off;
        int sceneHeight = 0;
    };
}
