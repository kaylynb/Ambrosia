#pragma once

#include "core/gc.h"
#include "engine/canvas.h"
#include "engine/mesh.h"
#include "engine/resources.h"
#include "render/gl_handle.h"
#include "render/lighting.h"
#include "render/lightmap.h"
#include "render/render_device.h"
#include "render/span_buffer.h"
#include "render/view_modes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dx {
    class GLRenderDevice;
    class UMesh;
    class World;
    struct MeshTriangle;

    UTexture* meshSkin(UObject* actor, UMesh* mesh, size_t index);
    UTexture* meshEnvironmentMap(UObject* actor, UMesh* mesh, UObject* zone, UObject* levelInfo, UTexture* skinOverride);
    void environmentCoords(const Vector& eyeToPoint, const Vector& normal, float& u, float& v);
    UObject* frameSkyZone(UObject* zone);
    UTexture* matrixTexture(UObject* viewport);
    UTexture* spriteAnimOnceFrame(UObject* actor, UTexture* texture);
    bool drawnAsSprite(uint8_t drawType);

    struct RenderOptions {
        float anisotropy = 1;
        bool trilinear = false;
        int multisample = 0;
        bool fxaa = false;
        bool smoothLightmaps = false;
        float detailDistance = 380;
        float textureRate = 0;
        OcclusionQuality ambientOcclusion = OcclusionQuality::Off;
    };

    // Every object pointer the renderer keeps between calls is a cache entry, so all of them are
    // weak: when an object is collected, objectsReleased drops what was kept for it. GL objects
    // that go with those entries are deleted at the start of the next draw call or frame, once
    // no canvas batch can still use them. A collection must not come in the middle of a draw
    // call, while the caches are being walked.
    class SceneRenderer : private gc::Holder {
    public:
        SceneRenderer();
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        void beginFrame(int width, int height);
        // world is the world in play, or null when there is none, as in the editor.
        void drawScene(World* world, ULevel* level, const std::vector<UObject*>& actors, const Vector& location, const Rotator& rotation, float fov, UObject* viewActor);
        void drawSceneRect(World* world, ULevel* level, const std::vector<UObject*>& actors, const Vector& location, const Rotator& rotation, float fov, UObject* viewActor, int x, int y, int width, int height);
        void drawActorOverlay(World* world, UObject* actor, const Vector& viewLocation, const Rotator& viewRotation, float fov, bool clearZ);
        Canvas2D& canvas();
        void endFrame();
        void applyBrightness(float brightness);
        void setOptions(const RenderOptions& newOptions);
        void readPixels(std::vector<uint8_t>& rgba, int& width, int& height);
        // The last frame shown, before brightness, top row first in RGBA. It stays readable
        // between frames, after the window's own buffer has been swapped away.
        bool readKeptFrame(std::vector<uint8_t>& rgba, int& width, int& height);
        void flushLevel();

        // Entries in the per-actor caches that flushLevel clears, how many belong to
        // destroyed actors, and approximate bytes (entry plus vector capacity).
        struct CacheStats {
            size_t meshDraw = 0;
            size_t meshDrawDead = 0;
            uint64_t meshDrawBytes = 0;
            size_t pose = 0;
            size_t poseDead = 0;
            uint64_t poseBytes = 0;
            size_t light = 0;
            size_t lightDead = 0;
            uint64_t lightBytes = 0;
            size_t occlusion = 0;
            size_t occlusionDead = 0;
            size_t occlusionQueries = 0;
            size_t brushDraw = 0;
            size_t brushDrawDead = 0;
        };

        CacheStats cacheStats() const;
        // Whether anything the renderer keeps between frames refers to this object. It only
        // compares addresses, so it may be asked about an object already deleted.
        bool holds(const UObject* object) const;
        void setTime(double seconds);
        void setViewportTime(double seconds);
        // Whether the level is paused (World::paused), for the flicker lights.
        void setPaused(bool paused);
        void setRenderMode(RenderMode mode);

        // Developer geometry drawn over the main view in one colour each: triangles shaded over
        // the surfaces they lie on, and lines, both hidden behind nearer geometry unless
        // throughWalls. Each layer is kept until replaced; an empty list clears it. Layers draw in
        // order of their number.
        struct DebugMesh {
            float r = 1;
            float g = 1;
            float b = 1;
            float a = 0.5f;
            bool throughWalls = false;
            std::vector<Vector> triangles;
            std::vector<Vector> lines;
        };

        void setDebugMeshes(int layer, std::vector<DebugMesh> meshes);

        std::function<bool(UObject* actor, const std::function<void(UObject* item)>& emit)> iteratorHook;

    private:
        // The world given to the draw call in progress.
        World* sceneWorld = nullptr;

        struct Batch {
            UTexture* texture = nullptr;
            uint32_t polyFlags = 0;
            int32_t lightmapPage = -1;
            uint8_t zone = 0;
            float uSize = 64;
            float vSize = 64;
            UTexture* detail = nullptr;
            std::vector<std::array<int32_t, 4>> detailPolys;
            std::vector<std::array<int32_t, 6>> surfPolys;
            std::vector<std::array<int32_t, 3>> nodePolys;
            bool fogZone = false;
            int32_t first = 0;
            int32_t count = 0;
            Vector boundsMin;
            Vector boundsMax;
        };

        struct MirrorSurface {
            Vector normal;
            float distance = 0;
            uint8_t zone = 0;
            uint32_t polyFlags = 0;
            int32_t first = 0;
            int32_t count = 0;
            std::vector<Vector> hull;
        };

        // Mirror surfaces sharing a plane and a zone, drawn as one mirror view; hull is the hull
        // of the pieces' hulls.
        struct MirrorPlane {
            std::vector<int32_t> pieces;
            std::vector<Vector> hull;
        };

        struct NodeEmit {
            int32_t batch = 0;
            int32_t first = 0;
            int32_t count = 0;
        };

        struct ViewVisibility {
            std::vector<int32_t> nodes;
            std::vector<int32_t> nodeSequence;
            std::vector<std::pair<int32_t, int32_t>> leaves;
            std::vector<std::pair<int32_t, int32_t>> culled;
            int32_t endSequence = 0;
            std::vector<std::vector<ScreenSpan>> mirrorSpans;
            // Where each of moverMirrors is on screen in this view, clipped to it but not
            // occluded (the face's depth pass and the drawing after it hide what is in front).
            std::vector<std::vector<ScreenSpan>> moverMirrorSpans;
            std::vector<ScreenSpan> backdropSpans;
        };

        // A mover face with the Mirror flag, where it is this frame. The original filters mover
        // faces into the level's BSP, and Render's OccludeBsp gives any
        // surface with the flag a mirror view, so these reflect as level mirrors do.
        struct MoverMirror {
            UObject* actor = nullptr;
            Vector normal;
            float distance = 0;
            uint8_t zone = 0;
            uint32_t polyFlags = 0;
            std::vector<Vector> points;
            std::vector<GLVertex> triangles;
        };

        struct FogCache {
            bool valid = false;
            bool fogged = false;
            uint64_t signature = 0;
            Vector eye;
            Vector center;
            float extent = -1;
            int32_t count = 0;
        };

        struct LevelData {
            ULevel* level = nullptr;
            UModel* model = nullptr;
            StaticGeometry geometry;
            StaticGeometry detailGeometry;
            std::vector<Vector> detailPoints;
            std::vector<Batch> batches;
            bool hasBackdrop = false;
            StaticGeometry mirrorGeometry;
            std::vector<MirrorSurface> mirrors;
            std::vector<MirrorPlane> mirrorPlanes;
            std::vector<std::vector<int32_t>> surfNodes;
            std::vector<int32_t> nodeVolumetrics;
            std::vector<int32_t> leafVolumetrics;
            bool anyVolumetric = false;
            std::vector<uint64_t> fogFrame;
            std::vector<FogCache> fogCache;
            std::vector<uint8_t> fogged;
            std::unordered_map<int32_t, std::vector<Vector>> fogPoints;
            std::vector<uint32_t> surfFlags;
            std::vector<int32_t> nodeMirror;
            std::vector<int32_t> nodeEmitStart;
            std::vector<NodeEmit> nodeEmits;
            std::vector<uint32_t> nodeStamp;
            std::vector<uint32_t> surfStamp;
            std::vector<uint32_t> leafStamp;
            std::vector<uint32_t> cullStamp;
            std::vector<int32_t> cullOrder;
            std::vector<std::vector<std::array<int32_t, 3>>> batchRanges;
            UObject* levelInfo = nullptr;
            std::vector<GLVertex> vertices;
            std::vector<GLVertex> wireVertices;
            std::vector<GLVertex> portalVertices;
            StaticGeometry flatGeometry[2];
        };

        struct TranslucentSurface {
            int32_t order = 0;
            const Batch* batch = nullptr;
            int32_t first = 0;
            int32_t count = 0;
            int32_t node = -1;
        };

        struct GroupExtra {
            uint32_t lightmap = 0;
            uint32_t lightMask = 0;
            bool cull = false;
            // A mover's brush, drawn pushed back in depth: see EmittedGroup::brush.
            bool brush = false;
            // Mover faces with the Mirror flag. Where mover mirrors are active, opaque ones are
            // left undrawn, as their reflection and depth are already there, and translucent
            // ones are drawn lit in one pass over the reflection, as level mirror surfaces are,
            // rather than lightened and then darkened by their lightmap, which would darken the
            // reflection behind them too.
            bool mirror = false;
        };

        struct TranslucentItem {
            int32_t order = 0;
            float distance = 0;
            uint32_t flags = 0;
            uint32_t texture = 0;
            GroupExtra extra;
            std::vector<GLVertex> vertices;
        };

        struct MeshTransform {
            Vector origin;
            Vector x;
            Vector y;
            Vector z;
        };

        struct EmittedGroup {
            uint64_t key = 0;
            uint32_t flags = 0;
            uint32_t texture = 0;
            bool translucent = false;
            uint32_t lightmap = 0;
            uint32_t lightMask = 0;
            bool cull = false;
            // A brush actor's (a mover's) surfaces. The original filters them into the level's
            // BSP (FMovingBrushTracker), where a level surface on the same plane is drawn first
            // and wins; drawn on their own they would fight it for depth, as the slab set flush
            // in 00_Intro's mirror floor did, so they are drawn a little behind instead.
            bool brush = false;
            // See GroupExtra::mirror.
            bool mirror = false;
            Vector low;
            Vector high;
            std::vector<GLVertex> vertices;
        };

        struct LightState {
            std::vector<UObject*> actors;
            std::vector<PreparedLight> prepared;
            std::vector<Vector> colors;
        };

        struct MeshDrawCache {
            bool valid = false;
            bool cacheable = false;
            MeshTransform transform;
            Name sequence;
            float frame = 0;
            Name blendSequences[4];
            float blendFrames[4] = {};
            Vector sphereCenter;
            float sphereRadius = 0;
            float lodTarget = -1;
            std::vector<Vector> world;
            std::vector<Vector> worldNormals;
            bool lodTriangles = false;
            std::vector<MeshTriangle> triangles;
            std::vector<const GLTextureInfo*> slotInfo;
            std::vector<uint32_t> slotFlags;
            // The texture generation slotInfo and the groups' texture ids were taken in.
            uint64_t textureGeneration = 0;
            uint32_t actorFlags = 0;
            bool unlitActor = false;
            float scaleGlow = 0;
            float glowScale = 0;
            Vector ambient;
            Vector eye;
            LightState lights;
            std::vector<EmittedGroup> groups;
        };

        struct BrushDrawCache {
            bool valid = false;
            Vector location;
            Rotator rotation;
            Vector scale;
            Vector prePivot;
            UModel* brush = nullptr;
            Vector sphereCenter;
            float sphereRadius = 0;
            std::vector<const GLTextureInfo*> polyInfo;
            std::vector<uint32_t> polyFlags;
            uint64_t textureGeneration = 0;
            Vector ambient;
            LightState lights;
            std::vector<EmittedGroup> groups;
            std::shared_ptr<UModel> frameModel;
            std::shared_ptr<LightmapAtlas> atlas;
            uint64_t litFrame = 0;
            UTexture* textureOverride = nullptr;
        };

        struct OcclusionState {
            QueryHandle query;
            bool pending = false;
            bool known = false;
            bool visible = true;
            uint64_t issuedFrame = 0;
            uint64_t resultFrame = 0;
        };

        struct View {
            Vector location;
            Rotator rotation;
            Mat4 viewProj {};
            Coords axes;
            float fov = 90;
            float tanHalfWidth = 1;
            float tanHalfHeight = 1;
            uint64_t zoneMask = ~0ull;
            int32_t recursion = 0;
            bool mirrored = false;
            int32_t clipCount = 0;
            std::array<Plane, 8> clipPlanes {};
            std::vector<Plane> cullPlanes;
            // A mirror view of several pieces: a sphere must also be inside one of these pyramids.
            std::vector<std::vector<Plane>> cullPieces;
            std::vector<Vector> portal;
            bool fog = false;
            bool sky = false;
            std::shared_ptr<ViewVisibility> visibility;
        };

        void objectsReleased(const gc::ReleasedSet& released) override;
        // Deletes the GL objects of entries dropped by objectsReleased, and the level if it went.
        void settleReleased();
        void dropRetired();
        void buildLevel(ULevel* level, const std::vector<UObject*>& actors);
        void drawLevelGeometry(const View& view, bool translucentPass);
        void drawDetail(const Batch& batch, const View& view, int32_t onlyNode);
        void drawBatch(const Batch& batch, const View& view, const int32_t* firsts, const int32_t* counts, int32_t ranges, int32_t onlyNode, uint32_t& uploadedLights);
        void occludeView(View& view, const std::vector<ScreenSpan>* open);
        void gatherVisible(const View& view);
        void drawMirrors(const View& view, const std::vector<UObject*>& actors);
        void drawFrameContent(const View& view, const std::vector<UObject*>& actors, UObject* viewActor, bool occlude);
        void applyClip(const View& view);
        void drawSky(const View& frame, const std::vector<UObject*>& actors, UObject* viewActor);
        void markMirror(const View& view, const int32_t* pieces, size_t count);
        // markMirror for a mover face: the same marking, from its triangles.
        void markMoverMirror(const View& view, const MoverMirror& mirror);
        // Finds this frame's mover mirror faces (moverMirrors) among the actors.
        void gatherMoverMirrors(const std::vector<UObject*>& actors);
        // Whether mover faces give mirror views in this view; when they do, the opaque ones are
        // drawn by their reflection alone, as level mirror surfaces are.
        bool moverMirrorsActive(int32_t recursion) const;
        // Writes the depth of the mover mirror faces facing the view, before level geometry, as
        // the level mirror surfaces' depth is, so what is behind them cannot cover the reflection.
        void drawMoverMirrorDepth(const View& view);
        void collectVolumetrics(const View& view);
        bool surfaceFog(int32_t surfIndex, const View& view);
        void drawFog(const Batch& batch, const View& view, int32_t onlyNode);
        Vector meshFog(const Vector& point, const View& view, size_t count) const;
        void drawActors(const std::vector<UObject*>& actors, const View& view, UObject* viewActor, bool occlude);
        // Known: the bounds a query is drawn with. Small: a mesh too small to be worth a query,
        // drawn on the CPU culling alone as sprites are. Unknown: not drawn at this scale yet.
        enum class ActorBounds { Unknown, Small, Known };
        ActorBounds actorBounds(UObject* actor, Vector& center, float& radius) const;
        void cullOccluded(std::vector<UObject*>& candidates, const View& view);
        void applyView(const View& view);
        void drawActor(UObject* actor, const View& view);
        void drawMesh(UObject* actor, const View& view);
        void drawMeshWith(UObject* actor, UObject* lightActor, UMesh* mesh, UObject* animSource, const MeshTransform& transform, const View& view, std::vector<Vector>* worldOut);
        void drawSprite(UObject* actor, const View& view);
        void drawBrush(UObject* actor, const View& view);
        void drawMoverSurfaces(UObject* actor, UModel* brush);
        void flushMeshBatches();
        void flushTranslucent(const View& view);
        void sortTranslucent();
        void drawTranslucentItem(size_t index);
        void drawGroup(uint32_t flags, uint32_t texture, const GroupExtra& extra, const std::vector<GLVertex>& vertices);
        void setActorOrder(UObject* actor, const View& view);
        void queueTranslucent(uint32_t flags, uint32_t texture, const GroupExtra& extra, const std::vector<GLVertex>& vertices);
        void flushBatches(std::unordered_map<uint64_t, std::vector<GLVertex>>& batches);
        View makeView(const Vector& location, const Rotator& rotation, float fov, float nearPlane);
        View makeView(const Vector& location, const Coords& axes, float fov, float nearPlane);
        UObject* zoneActor(uint8_t zone) const;
        UObject* zoneForActor(UObject* actor);
        uint8_t pointZone(const Vector& location) const;
        int32_t pointLeaf(const Vector& location) const;
        uint8_t actorZone(UObject* actor) const;
        uint64_t visibleZones(uint8_t zone) const;
        bool inFrustum(const View& view, const Vector& center, float radius) const;
        bool sphereReachesVisible(const View& view, const Vector& center, float radius);
        uint32_t litMask(const Vector& low, const Vector& high, bool specialLit) const;
        void addCorona(UObject* light, const Vector& origin, float delta, size_t& free, UObject* viewportActor);
        void drawCoronas(const View& view, int x, int y, int width, int height);
        bool sphereOutsideView(const View& view, const Vector& location, float radius) const;
        bool sphereReachesLeaf(const Vector& center, float radius, int32_t leaf) const;
        float actorRadius(UObject* actor) const;
        Vector actorLight(UObject* actor, const Vector& point, const Vector& normal, const Vector& ambient, float scaleGlow, bool unlit, const Vector* eye);
        void collectLights(const std::vector<UObject*>& actors);
        void collectDynamicLights(const Vector& viewLocation);
        void drawDecal(UObject* actor, const View& view);
        bool sameLights(const LightState& state) const;
        void storeLights(LightState& state) const;
        void appendGroups(const std::vector<EmittedGroup>& groups, std::unordered_map<uint64_t, std::vector<GLVertex>>& opaque);
        void selectLights(const Vector& location, float extent);
        void selectActorLights(UObject* actor);
        void applyBlend(uint32_t polyFlags, int32_t extraFlags);
        const std::vector<MeshTriangle>& meshTriangles(UMesh* mesh);
        uint32_t flatVao();
        void drawWireframe(const View& view, const std::vector<UObject*>& actors, UObject* viewActor);
        void drawPortals(const View& view);
        void drawDebugMeshes(const View& view);

        struct DebugBatch {
            DebugMesh mesh;
            std::vector<GLVertex> triangles;
            std::vector<GLVertex> lines;
        };

        std::map<int, std::vector<DebugBatch>> debugBatches;

        std::unique_ptr<GLRenderDevice> device;
        LightmapAtlas lightmaps;
        LevelData levelData;
        std::vector<UObject*> lights;
        std::vector<PreparedLight> lightPrepared;
        std::vector<Vector> lightColors;
        std::vector<UObject*> actorLights;
        std::vector<DynamicLight> dynamicLights;
        std::vector<DynamicLight> batchLights;
        std::vector<PreparedLight> actorLightPrepared;
        std::vector<Vector> actorLightColors;

        struct LightSender {
            UObject* light = nullptr;
            int32_t factor = 0;
        };

        struct CoronaLight {
            UObject* actor = nullptr;
            float bright = 0;
        };

        std::unordered_map<UObject*, std::vector<LightSender>> actorLightCache;
        CoronaLight coronas[32];
        std::vector<VolumetricLight> volumetrics;
        std::vector<std::array<int64_t, 3>> volumetricPalettes;
        std::vector<Vector> volumetricColors;
        uint64_t volumetricSignature = 0;
        uint64_t fogStamp = 0;
        std::vector<uint8_t> scratchFog;
        std::vector<Vector> scratchFogLit;
        double coronaTime = -1;
        std::unordered_map<UObject*, size_t> lightIndex;
        std::vector<UObject*> lightConsider;
        int32_t considerTag = 0;
        int32_t lightDelta = 0;
        int32_t lightFrameCount = 0;
        double lastFrameTime = -1;
        std::vector<Vector> scratchPose;
        std::vector<Vector> scratchNormals;
        std::vector<Vector> scratchWorld;
        std::vector<Vector> scratchWorldNormals;
        std::vector<MeshTriangle> scratchLodTriangles;
        std::vector<float> scratchLodAlphas;
        std::vector<Vector> scratchLit;
        std::vector<uint8_t> scratchLitDone;
        std::vector<UTexture*> scratchSkins;
        std::vector<const GLTextureInfo*> scratchSlotInfo;
        std::vector<uint32_t> scratchSlotFlags;
        std::vector<size_t> scratchSlotGroups;
        std::unordered_map<UMesh*, std::vector<MeshTriangle>> triangleCache;
        std::map<std::pair<UObject*, UMesh*>, MeshPoseCaches> poseCaches;
        std::map<std::tuple<UObject*, UMesh*, UObject*>, MeshDrawCache> meshDrawCaches;
        std::unordered_map<UObject*, BrushDrawCache> brushDrawCaches;
        std::unordered_map<UObject*, OcclusionState> occlusion;
        std::vector<UObject*> scratchCandidates;
        std::vector<UObject*> scratchDecals;
        std::vector<GLVertex> occlusionBoxes;
        SpanBuffer spans;
        std::vector<int32_t> touchedBatches;
        std::vector<int32_t> scratchNodes;
        std::vector<int32_t> scratchFirsts;
        std::vector<int32_t> scratchCounts;
        std::vector<ScreenSpan> scratchSpans;
        std::vector<ScreenSpan> scratchMirrorSpans;
        std::vector<MoverMirror> moverMirrors;
        // The recursion of the view being drawn, for the brush groups (moverMirrorsActive).
        int32_t drawRecursion = 0;
        uint32_t visibleStamp = 0;
        int viewportWidth = 1;
        int viewportHeight = 1;
        std::vector<OcclusionState*> occlusionIssued;
        std::unordered_set<UObject*> occlusionUnconfirmed;
        uint64_t sceneFrame = 0;
        std::unordered_map<uint64_t, std::vector<GLVertex>> meshBatches;
        std::unordered_map<uint64_t, std::vector<GLVertex>> brushBatches;
        std::vector<TranslucentItem> translucentItems;
        size_t translucentCount = 0;
        std::vector<TranslucentSurface> scratchTranslucentSurfaces;
        std::vector<size_t> scratchTranslucentOrder;
        std::vector<int32_t> nodeOrder;
        std::vector<int32_t> leafOrder;
        int32_t currentOrder = 0;
        int32_t fallbackOrder = 0;
        float currentDistance = 0;
        std::unordered_map<uint64_t, uint32_t> meshBatchFlags;
        std::unordered_map<uint64_t, uint32_t> meshBatchTextures;
        std::unordered_map<uint64_t, GroupExtra> meshBatchExtras;
        int frameWidth = 1;
        int frameHeight = 1;
        float viewAspect = 1;
        double time = 0;
        bool timeSet = false;
        double viewportTime = -1;
        bool levelPaused = false;
        double currentTime = 0;
        double startTime = 0;
        float gammaExponent = 1;
        bool offscreen = false;
        RenderOptions options;
        RenderMode renderMode = RenderMode::Textured;
        std::vector<QueryHandle> retiredQueries;
        std::vector<BrushDrawCache> retiredBrushes;
        // The level drawn last, or something its built geometry refers to, was collected.
        bool levelReleased = false;
        int drawing = 0;
    };
}
