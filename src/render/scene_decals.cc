#include "render/scene_renderer.h"

#include "engine/props_engine.h"
#include "engine/world.h"
#include "render/scene_renderer_internal.h"

namespace dx {
    void SceneRenderer::drawDecal(UObject* actor, const View& view)
    {
        World* world = sceneWorld;
        UModel* model = levelData.model;
        if (!world || !model || world->level != levelData.level) {
            return;
        }
        auto found = world->decals.pieces.find(actor);
        if (found == world->decals.pieces.end()) {
            return;
        }
        UTexture* texture = asTexture(actor::Texture(actor));
        if (!texture && levelData.levelInfo) {
            texture = asTexture(levelinfo::DefaultTexture(levelData.levelInfo));
        }
        if (UTexture* matrix = matrixTexture(world->player())) {
            texture = matrix;
        }
        texture = device->animated(texture);
        const GLTextureInfo* info = device->texture(texture);
        if (!info) {
            return;
        }
        float textureScale = texture->data && texture::DrawScale(texture) > 0 ? texture::DrawScale(texture) : 1.0f;
        float uSize = static_cast<float>(info->width) * textureScale * actor::DrawScale(actor);
        float vSize = static_cast<float>(info->height) * textureScale * actor::DrawScale(actor);
        std::vector<GLVertex> vertices;
        std::vector<Vector> polygon;
        for (const DecalPiece& piece : found->second) {
            if (piece.surf < 0 || static_cast<size_t>(piece.surf) >= model->surfs.size()) {
                continue;
            }
            const BspSurf& surf = model->surfs[static_cast<size_t>(piece.surf)];
            Vector lift = model->vectors[static_cast<size_t>(surf.vNormal)].normal() * 0.75f;
            Vector uAxis = (piece.vertices[1] - piece.vertices[0]).normal();
            Vector vAxis = (piece.vertices[3] - piece.vertices[0]).normal();
            const std::vector<int32_t>& nodes = piece.nodes.empty() ? levelData.surfNodes[static_cast<size_t>(piece.surf)] : piece.nodes;
            for (int32_t node : nodes) {
                const BspNode& bspNode = model->nodes[static_cast<size_t>(node)];
                if (bspNode.numVertices < 3 || !(view.zoneMask & (1ull << (bspNode.zone[1] & 63)))) {
                    continue;
                }
                polygon.assign(piece.vertices.begin(), piece.vertices.end());
                clipDecalToNode(model, node, polygon);
                if (polygon.size() < 3) {
                    continue;
                }
                for (size_t i = 1; i + 1 < polygon.size(); i++) {
                    for (size_t corner : { size_t { 0 }, i, i + 1 }) {
                        const Vector& point = polygon[corner];
                        GLVertex vertex;
                        vertex.x = point.x + lift.x;
                        vertex.y = point.y + lift.y;
                        vertex.z = point.z + lift.z;
                        vertex.u = (point - piece.vertices[0]).dot(uAxis) / uSize;
                        vertex.v = (point - piece.vertices[0]).dot(vAxis) / vSize;
                        vertex.r = 1.0f;
                        vertex.g = 1.0f;
                        vertex.b = 1.0f;
                        vertices.push_back(vertex);
                    }
                }
            }
        }
        if (vertices.empty()) {
            return;
        }
        if (levelData.levelInfo) {
            decal::LastRenderedTime(actor) = levelinfo::TimeSeconds(levelData.levelInfo);
        }
        applyBlend(PF_Modulated, 0);
        device->setCulling(false);
        device->setDepthWrite(false);
        device->bindTexture(info->id());
        device->draw(vertices, Primitive::Triangles);
        device->setBlend(BlendMode::Opaque);
    }
}
