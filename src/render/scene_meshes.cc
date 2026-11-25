#include "render/scene_renderer.h"

#include "core/objects.h"
#include "core/profile.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/lighting.h"
#include "render/scene_renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace dx {
    UTexture* meshSkin(UObject* actor, UMesh* mesh, size_t index)
    {
        UTexture* skin = index < 8 ? asTexture(actor::MultiSkins(actor, static_cast<int>(index))) : nullptr;
        if (skin) {
            return skin;
        }
        UTexture* meshTexture = index < mesh->textures.size() ? asTexture(mesh->textures[index]) : nullptr;
        if (index != 0 && meshTexture) {
            return meshTexture;
        }
        UTexture* actorSkin = asTexture(actor::Skin(actor));
        if (actorSkin) {
            return actorSkin;
        }
        return meshTexture;
    }

    UTexture* meshEnvironmentMap(UObject* actor, UMesh* mesh, UObject* zone, UObject* levelInfo, UTexture* skinOverride)
    {
        if (UTexture* texture = asTexture(actor::Texture(actor))) {
            return texture;
        }
        if (zone && zone->data && zone->isA(nameZoneInfo)) {
            if (UTexture* texture = asTexture(zoneinfo::EnvironmentMap(zone))) {
                return texture;
            }
        }
        if (levelInfo && levelInfo->data) {
            if (UTexture* texture = asTexture(zoneinfo::EnvironmentMap(levelInfo))) {
                return texture;
            }
        }
        UTexture* last = nullptr;
        for (size_t i = 0; i < mesh->textures.size(); i++) {
            if (UTexture* texture = skinOverride ? skinOverride : meshSkin(actor, mesh, i)) {
                last = texture;
            }
        }
        return last;
    }

    void environmentCoords(const Vector& eyeToPoint, const Vector& normal, float& u, float& v)
    {
        Vector direction = eyeToPoint.normal();
        Vector reflected = direction - normal * (2.0f * direction.dot(normal));
        u = (reflected.x + 1.0f) * 0.5f;
        v = (reflected.y + 1.0f) * 0.5f;
    }

    UTexture* matrixTexture(UObject* viewport)
    {
        if (!viewport || !viewport->data || viewport->destroyed || !viewport->isA(namePlayerPawn)) {
            return nullptr;
        }
        return asTexture(actor::Sprite(viewport));
    }

    const std::vector<MeshTriangle>& SceneRenderer::meshTriangles(UMesh* mesh)
    {
        auto it = triangleCache.find(mesh);
        if (it == triangleCache.end()) {
            it = triangleCache.emplace(mesh, mesh->triangles()).first;
        }
        return it->second;
    }

    void SceneRenderer::drawMesh(UObject* actor, const View& view)
    {
        UObject* meshObject = actor::Mesh(actor);
        if (!meshObject) {
            return;
        }
        objects::ensureLoaded(meshObject);
        UMesh* mesh = cast<UMesh>(meshObject);
        if (!mesh || mesh->frameVerts <= 0) {
            return;
        }
        UObject* animSource = actor;
        UObject* owner = actor::Owner(actor);
        if (actor::bAnimByOwner(actor) && owner && owner->data) {
            animSource = owner;
        }
        Coords rotation = rotationAxes(actor::Rotation(actor));
        float drawScale = actor::DrawScale(actor);
        MeshTransform transform;
        transform.origin = actor::Location(actor) + actor::PrePivot(actor);
        transform.x = rotation.xAxis * drawScale;
        transform.y = rotation.yAxis * drawScale;
        transform.z = rotation.zAxis * drawScale;
        std::vector<Vector> world;
        drawMeshWith(actor, actor, mesh, animSource, transform, view, &world);

        ULodMesh* lodMesh = cast<ULodMesh>(mesh);
        if (!lodMesh || lodMesh->specialFaces.empty() || !actor->isA(namePawn)) {
            return;
        }
        UObject* weapon = pawn::Weapon(actor);
        if (!weapon || !weapon->data || weapon->destroyed) {
            return;
        }
        UObject* weaponMeshObject = inventory::ThirdPersonMesh(weapon);
        if (!weaponMeshObject) {
            return;
        }
        objects::ensureLoaded(weaponMeshObject);
        UMesh* weaponMesh = cast<UMesh>(weaponMeshObject);
        if (!weaponMesh || weaponMesh->frameVerts <= 0) {
            return;
        }
        const MeshFace& face = lodMesh->specialFaces.front();
        Vector corners[3];
        for (int i = 0; i < 3; i++) {
            size_t index = face.wedge[i];
            if (!lodMesh->remapAnimVerts.empty() && index < lodMesh->remapAnimVerts.size()) {
                index = lodMesh->remapAnimVerts[index];
            }
            if (index >= world.size()) {
                return;
            }
            corners[i] = world[index];
        }
        float scale = inventory::ThirdPersonScale(weapon);
        if (scale <= 0) {
            scale = 1;
        }
        Vector axisX = (corners[1] - corners[0]).normal();
        Vector axisY = axisX.cross(corners[2] - corners[0]).normal();
        Vector axisZ = axisX.cross(axisY).normal();
        MeshTransform attach;
        attach.origin = (corners[0] + corners[2]) * 0.5f;
        attach.x = axisX * scale;
        attach.y = axisY * scale;
        attach.z = axisZ * scale;
        drawMeshWith(weapon, actor, weaponMesh, weapon, attach, view, nullptr);
    }

    void SceneRenderer::drawMeshWith(UObject* actor, UObject* lightActor, UMesh* mesh, UObject* animSource, const MeshTransform& transform, const View& view, std::vector<Vector>* worldOut)
    {
        DX_TIME("Mesh build and vertex lighting");
        MeshAnimState animState;
        animState.sequence = actor::AnimSequence(animSource);
        animState.frame = actor::AnimFrame(animSource);
        for (int slot = 0; slot < 4; slot++) {
            animState.blendSequences[slot] = actor::BlendAnimSequence(animSource, slot);
            animState.blendFrames[slot] = actor::BlendAnimFrame(animSource, slot);
        }
        MeshDrawCache& cache = meshDrawCaches[{ actor, mesh, lightActor }];
        bool sameGeometry = cache.valid && sameVector(cache.transform.origin, transform.origin) && sameVector(cache.transform.x, transform.x) && sameVector(cache.transform.y, transform.y) && sameVector(cache.transform.z, transform.z) && cache.sequence == animState.sequence && cache.frame == animState.frame;
        for (int slot = 0; slot < 4 && sameGeometry; slot++) {
            sameGeometry = cache.blendSequences[slot] == animState.blendSequences[slot] && cache.blendFrames[slot] == animState.blendFrames[slot];
        }

        ULodMesh* lodMesh = cast<ULodMesh>(mesh);
        bool doLod = lodMesh && lodMesh->lodStrength != 0.0f && !lodMesh->collapsePointThus.empty();
        bool doMorph = false;
        float targetSubset = -1;
        int32_t vertexSubset = 0;
        int32_t nonMorph = 0;
        if (doLod) {
            int32_t modelVerts = static_cast<int32_t>(lodMesh->modelVerts);
            doMorph = lodMesh->lodMorph > 0.0f;
            float fovBias = static_cast<float>(std::tan(static_cast<double>(view.fov) * 0.008726646259971648));
            float meshVertLod = static_cast<float>(lodMesh->modelVerts) * 0.003f + 0.25f;
            float resFactor = static_cast<float>(static_cast<double>(frameWidth) * 0.00109375 + 0.3);
            float z = (actor::Location(actor) - view.location).dot(view.axes.xAxis);
            float dist = std::max(z - lodMesh->lodZDisplace, 1.0f);
            float den = 0.25f * lodMesh->lodStrength * dist * meshVertLod * fovBias;
            float num = lodMesh->meshScaleMax * actor::DrawScale(actor) * actor::LODBias(actor) * resFactor * 430.0f;
            float target = num / den * static_cast<float>(modelVerts);
            targetSubset = std::max(static_cast<float>(lodMesh->lodMinVerts), target);
            vertexSubset = std::min(static_cast<int32_t>(std::nearbyint(targetSubset)), modelVerts);
            if (doMorph) {
                nonMorph = static_cast<int32_t>(std::nearbyint((1.0f - lodMesh->lodMorph) * targetSubset));
                if (nonMorph >= vertexSubset) {
                    doMorph = false;
                }
            }
            if (!doMorph && vertexSubset >= modelVerts) {
                targetSubset = -1;
            }
        }
        if (sameGeometry && cache.lodTarget != targetSubset) {
            sameGeometry = false;
        }
        const std::vector<MeshTriangle>* triangleSource = &meshTriangles(mesh);
        std::vector<MeshTriangle>& lodTriangles = scratchLodTriangles;
        std::vector<float>& lodAlphas = scratchLodAlphas;
        std::vector<Vector>& pose = scratchPose;
        std::vector<Vector>& normals = scratchNormals;
        std::vector<Vector>& world = scratchWorld;
        std::vector<Vector>& worldNormals = scratchWorldNormals;
        auto computeGeometry = [&]() {
            int32_t requested = lodMesh && doLod ? vertexSubset + static_cast<int32_t>(lodMesh->specialVerts) : mesh->frameVerts;
            int32_t returned = 0;
            {
                DX_TIME("Mesh: pose");
                returned = mesh->animatedPose(animState, poseCaches[{ animSource, mesh }], pose, requested);
            }
            DX_TIME("Mesh: LOD normals and transform");
            if (lodMesh && doLod) {
                vertexSubset = std::max(returned - static_cast<int32_t>(lodMesh->specialVerts), 0);
                if (doMorph && nonMorph >= vertexSubset) {
                    doMorph = false;
                }
                if (!doMorph && vertexSubset >= static_cast<int32_t>(lodMesh->modelVerts)) {
                    doLod = false;
                }
            }
            if (doLod) {
                lodAlphas.assign(lodMesh->modelVerts, 0.0f);
                if (doMorph) {
                    float inverse = 1.0f / (targetSubset * lodMesh->lodMorph);
                    for (int32_t i = vertexSubset - 1; i > nonMorph; i--) {
                        float alpha = std::min(1.0f, static_cast<float>(i - nonMorph) * inverse);
                        int32_t target = static_cast<size_t>(i) < lodMesh->collapsePointThus.size() ? lodMesh->collapsePointThus[static_cast<size_t>(i)] : 0;
                        size_t from = static_cast<size_t>(lodMesh->poseVertex(i));
                        size_t to = static_cast<size_t>(lodMesh->poseVertex(target));
                        if (target > 0 && from < pose.size() && to < pose.size()) {
                            pose[from] += (pose[to] - pose[from]) * alpha;
                        } else {
                            alpha = 0.0f;
                        }
                        lodAlphas[static_cast<size_t>(i)] = alpha;
                    }
                }
                lodMesh->lodTriangles(vertexSubset, doMorph, nonMorph, lodAlphas, lodTriangles);
                triangleSource = &lodTriangles;
            }
            normals.assign(pose.size(), Vector {});
            for (const MeshTriangle& triangle : *triangleSource) {
                if (static_cast<size_t>(triangle.vertex[0]) >= pose.size() || static_cast<size_t>(triangle.vertex[1]) >= pose.size() || static_cast<size_t>(triangle.vertex[2]) >= pose.size()) {
                    continue;
                }
                const Vector& a = pose[static_cast<size_t>(triangle.vertex[0])];
                const Vector& b = pose[static_cast<size_t>(triangle.vertex[1])];
                const Vector& c = pose[static_cast<size_t>(triangle.vertex[2])];
                Vector normal = (b - a).cross(c - a);
                normal = normal * (1.0f / std::sqrt(normal.sizeSquared() + 0.001f));
                for (int i = 0; i < 3; i++) {
                    normals[static_cast<size_t>(triangle.vertex[i])] += normal;
                }
            }
            world.resize(pose.size());
            worldNormals.resize(pose.size());
            for (size_t i = 0; i < pose.size(); i++) {
                world[i] = transform.origin + transform.x * pose[i].x + transform.y * pose[i].y + transform.z * pose[i].z;
                worldNormals[i] = (transform.x * normals[i].x + transform.y * normals[i].y + transform.z * normals[i].z).normal();
            }
        };
        if (!sameGeometry) {
            computeGeometry();
            boundingSphere(world, cache.sphereCenter, cache.sphereRadius);
            cache.valid = true;
            cache.transform = transform;
            cache.sequence = animState.sequence;
            cache.frame = animState.frame;
            for (int slot = 0; slot < 4; slot++) {
                cache.blendSequences[slot] = animState.blendSequences[slot];
                cache.blendFrames[slot] = animState.blendFrames[slot];
            }
            cache.lodTarget = targetSubset;
            cache.world.swap(world);
            cache.worldNormals.swap(worldNormals);
            cache.lodTriangles = triangleSource == &lodTriangles;
            if (cache.lodTriangles) {
                cache.triangles.swap(lodTriangles);
            }
        }
        triangleSource = cache.lodTriangles ? &cache.triangles : &meshTriangles(mesh);
        const std::vector<Vector>& meshWorld = cache.world;
        const std::vector<Vector>& meshNormals = cache.worldNormals;

        {
            DX_TIME("Mesh: light selection");
            selectActorLights(lightActor);
        }
        UObject* zone = zoneForActor(lightActor);
        Vector ambient = zoneAmbient(zone);
        uint8_t glow = actor::AmbientGlow(actor);
        float glowAmount = glow == 255 ? static_cast<float>(0.25 + 0.2 * std::sin(8.0 * currentTime)) : static_cast<float>(glow) / 255.0f;
        ambient += Vector { glowAmount, glowAmount, glowAmount };
        float scaleGlow = actor::ScaleGlow(actor);
        uint32_t actorFlags = styleFlags(actor);
        bool unlitActor = actor::bUnlit(actor) != 0;
        float clampedGlow = std::clamp(0.5f * actor::ScaleGlow(actor) + static_cast<float>(glow) / 256.0f, 0.0f, 1.0f);

        static constinit profile::TimeSite skinsTime { "Mesh: skins and textures" };
        std::optional<profile::TimeScope> skinsScope;
        skinsScope.emplace(skinsTime);
        std::vector<UTexture*>& skins = scratchSkins;
        skins.assign(std::max<size_t>(mesh->textures.size(), 8), nullptr);
        UTexture* matrix = matrixTexture(sceneWorld ? sceneWorld->player() : nullptr);
        for (size_t i = 0; i < skins.size(); i++) {
            skins[i] = matrix && i < mesh->textures.size() ? matrix : meshSkin(actor, mesh, i);
        }
        size_t environmentSlot = skins.size();
        skins.push_back(meshEnvironmentMap(actor, mesh, zoneForActor(actor), levelData.levelInfo, matrix));

        std::vector<const GLTextureInfo*>& slotInfo = scratchSlotInfo;
        std::vector<uint32_t>& slotFlags = scratchSlotFlags;
        slotInfo.assign(skins.size(), nullptr);
        slotFlags.assign(skins.size(), 0);
        for (size_t i = 0; i < skins.size(); i++) {
            UTexture* texture = device->animated(skins[i]);
            if (texture) {
                slotInfo[i] = device->texture(texture);
                slotFlags[i] = textureFlags(texture) & (PF_Masked | PF_Unlit);
            }
        }

        skinsScope.reset();
        bool fogged = view.fog && !volumetrics.empty();
        uint64_t textureGeneration = device->textureGeneration();
        bool cacheHit = !fogged && sameGeometry && cache.cacheable && cache.textureGeneration == textureGeneration && sameVector(cache.eye, view.location) && cache.slotInfo == slotInfo && cache.slotFlags == slotFlags && cache.actorFlags == actorFlags && cache.unlitActor == unlitActor && cache.scaleGlow == scaleGlow && cache.glowScale == clampedGlow && sameVector(cache.ambient, ambient) && sameLights(cache.lights);
        if (cacheHit) {
            if (worldOut) {
                *worldOut = meshWorld;
            }
            appendGroups(cache.groups, meshBatches);
            return;
        }
        if (worldOut) {
            *worldOut = meshWorld;
        }

        bool cacheable = glow != 255;
        for (const PreparedLight& prepared : actorLightPrepared) {
            if (timeDependentEffect(prepared.effect)) {
                cacheable = false;
            }
        }
        if (fogged) {
            cacheable = false;
        }
        cache.slotInfo = slotInfo;
        cache.slotFlags = slotFlags;
        cache.textureGeneration = textureGeneration;
        cache.actorFlags = actorFlags;
        cache.unlitActor = unlitActor;
        cache.scaleGlow = scaleGlow;
        cache.glowScale = clampedGlow;
        cache.ambient = ambient;
        cache.eye = view.location;
        storeLights(cache.lights);
        cache.groups.clear();

        std::vector<size_t>& slotGroups = scratchSlotGroups;
        slotGroups.assign(skins.size() * 4, 0);
        std::vector<Vector>& lit = scratchLit;
        std::vector<uint8_t>& litDone = scratchLitDone;
        lit.resize(meshWorld.size());
        litDone.assign(meshWorld.size(), 0);
        std::vector<Vector>& fogAt = scratchFogLit;
        if (fogged) {
            int32_t leaf = *reinterpret_cast<int32_t*>(actor::Region(lightActor) + sizeof(UObject*));
            if (leaf < 0) {
                leaf = pointLeaf(actor::Location(lightActor));
            }
            size_t count = leaf >= 0 && static_cast<size_t>(leaf) < levelData.leafVolumetrics.size() && levelData.leafStamp[static_cast<size_t>(leaf)] == visibleStamp ? static_cast<size_t>(levelData.leafVolumetrics[static_cast<size_t>(leaf)]) : volumetrics.size();
            fogAt.resize(meshWorld.size());
            for (size_t i = 0; i < meshWorld.size(); i++) {
                fogAt[i] = meshFog(meshWorld[i], view, count);
            }
        }
        DX_TIME("Mesh: vertex lighting and emission");
        for (const MeshTriangle& triangle : *triangleSource) {
            uint32_t flags = triangle.polyFlags | actorFlags;
            if (flags & PF_Invisible) {
                continue;
            }
            if (triangle.textureIndex < 0 || static_cast<size_t>(triangle.textureIndex) >= environmentSlot) {
                continue;
            }
            if (static_cast<size_t>(triangle.vertex[0]) >= meshWorld.size() || static_cast<size_t>(triangle.vertex[1]) >= meshWorld.size() || static_cast<size_t>(triangle.vertex[2]) >= meshWorld.size()) {
                continue;
            }
            size_t slot = static_cast<size_t>(triangle.textureIndex);
            if ((flags & PF_Environment) || !skins[slot]) {
                slot = environmentSlot;
            }
            const GLTextureInfo* info = slotInfo[slot];
            if (!info) {
                continue;
            }
            flags |= slotFlags[slot];
            if (flags & PF_Environment) {
                cacheable = false;
            }
            bool translucent = isTranslucentFlags(flags);
            bool unlit = unlitActor || (flags & PF_Unlit);
            uint32_t mode = (flags & PF_Translucent) ? 1u : ((flags & PF_Modulated) ? 2u : ((flags & PF_Masked) ? 3u : 0u));
            size_t& groupNumber = slotGroups[slot * 4 + mode];
            if (!groupNumber) {
                EmittedGroup group;
                group.key = (static_cast<uint64_t>(info->id()) << 8) | mode;
                group.flags = flags & (PF_Translucent | PF_Modulated | PF_Masked);
                group.texture = info->id();
                group.translucent = translucent;
                cache.groups.push_back(std::move(group));
                groupNumber = cache.groups.size();
            }
            std::vector<GLVertex>& out = cache.groups[groupNumber - 1].vertices;
            for (int i = 0; i < 3; i++) {
                size_t index = static_cast<size_t>(triangle.vertex[i]);
                if (!litDone[index]) {
                    lit[index] = actorLight(lightActor, meshWorld[index], meshNormals[index], ambient, scaleGlow, unlit, &view.location);
                    litDone[index] = 1;
                }
                GLVertex vertex;
                vertex.x = meshWorld[index].x;
                vertex.y = meshWorld[index].y;
                vertex.z = meshWorld[index].z;
                vertex.u = triangle.u[i] / 256.0f;
                vertex.v = triangle.v[i] / 256.0f;
                if (flags & PF_Environment) {
                    environmentCoords(meshWorld[index] - view.location, meshNormals[index], vertex.u, vertex.v);
                }
                Vector color = unlit ? Vector { clampedGlow, clampedGlow, clampedGlow } : lit[index];
                if (flags & PF_Modulated) {
                    color = { 1, 1, 1 };
                }
                vertex.r = std::min(color.x, 1.0f);
                vertex.g = std::min(color.y, 1.0f);
                vertex.b = std::min(color.z, 1.0f);
                vertex.a = 1;
                if (fogged) {
                    vertex.fogR = fogAt[index].x;
                    vertex.fogG = fogAt[index].y;
                    vertex.fogB = fogAt[index].z;
                }
                out.push_back(vertex);
            }
        }
        cache.cacheable = cacheable;
        appendGroups(cache.groups, meshBatches);
    }
}
