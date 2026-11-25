#include "engine/mesh.h"

#include "core/gc.h"
#include "core/objects.h"
#include "core/reader.h"
#include "engine/reader_values.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dx {
    namespace {
        using readers::readBox;
        using readers::readCount;
        using readers::readVector;

        Plane readSphere(Reader& reader)
        {
            Plane sphere;
            sphere.x = reader.f32();
            sphere.y = reader.f32();
            sphere.z = reader.f32();
            sphere.w = reader.f32();
            return sphere;
        }

        void checkSkip(Reader& reader, uint32_t skipOffset)
        {
            if (reader.tell() != skipOffset) {
                throw std::runtime_error("unexpected lazy array size in mesh");
            }
        }

        MeshFace readFace(Reader& reader)
        {
            MeshFace face;
            face.wedge[0] = static_cast<uint16_t>(reader.i16());
            face.wedge[1] = static_cast<uint16_t>(reader.i16());
            face.wedge[2] = static_cast<uint16_t>(reader.i16());
            face.material = static_cast<uint16_t>(reader.i16());
            return face;
        }

        std::vector<uint16_t> readWords(Reader& reader)
        {
            std::vector<uint16_t> values(readCount(reader, "mesh"));
            for (uint16_t& value : values) {
                value = static_cast<uint16_t>(reader.i16());
            }
            return values;
        }
    }

    void UMesh::serialize(Reader& reader, Linker& source)
    {
        UPrimitive::serialize(reader, source);

        uint32_t skip = reader.u32();
        verts.resize(readCount(reader, "mesh"));
        for (Vector& vertex : verts) {
            int16_t x = reader.i16();
            int16_t y = reader.i16();
            int16_t z = reader.i16();
            reader.i16();
            vertex = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
        }
        checkSkip(reader, skip);

        skip = reader.u32();
        tris.resize(readCount(reader, "mesh"));
        for (MeshTri& tri : tris) {
            for (uint16_t& vertex : tri.vertex) {
                vertex = static_cast<uint16_t>(reader.i16());
            }
            for (auto& uv : tri.uv) {
                uv[0] = reader.u8();
                uv[1] = reader.u8();
            }
            tri.polyFlags = reader.u32();
            tri.textureIndex = reader.i32();
        }
        checkSkip(reader, skip);

        sequences.resize(readCount(reader, "mesh"));
        for (AnimSequence& sequence : sequences) {
            sequence.name = source.readName(reader);
            sequence.group = source.readName(reader);
            sequence.startFrame = reader.i32();
            sequence.numFrames = reader.i32();
            sequence.notifys.resize(readCount(reader, "mesh"));
            for (AnimNotify& notify : sequence.notifys) {
                notify.time = reader.f32();
                notify.function = source.readName(reader);
            }
            sequence.rate = reader.f32();
            std::stable_sort(sequence.notifys.begin(), sequence.notifys.end(), [](const AnimNotify& a, const AnimNotify& b) {
                return a.time < b.time;
            });
        }

        skip = reader.u32();
        connects.resize(readCount(reader, "mesh"));
        for (MeshConnect& connect : connects) {
            connect.numVertTriangles = reader.i32();
            connect.triangleListOffset = reader.i32();
        }
        checkSkip(reader, skip);

        meshBounds = readBox(reader);
        meshSphere = readSphere(reader);

        skip = reader.u32();
        vertLinks.resize(readCount(reader, "mesh"));
        for (int32_t& link : vertLinks) {
            link = reader.i32();
        }
        checkSkip(reader, skip);

        textures.resize(readCount(reader, "mesh"));
        for (UObject*& texture : textures) {
            texture = source.readObject(reader);
        }

        frameBounds.resize(readCount(reader, "mesh"));
        for (BoundingBox& box : frameBounds) {
            box = readBox(reader);
        }
        frameSpheres.resize(readCount(reader, "mesh"));
        for (Plane& frameSphere : frameSpheres) {
            frameSphere = readSphere(reader);
        }

        frameVerts = reader.i32();
        animFrames = reader.i32();
        andFlags = reader.u32();
        orFlags = reader.u32();
        scale = readVector(reader);
        origin = readVector(reader);
        rotOrigin.pitch = reader.i32();
        rotOrigin.yaw = reader.i32();
        rotOrigin.roll = reader.i32();
        curPoly = reader.i32();
        curVertex = reader.i32();
        textureLod.resize(readCount(reader, "mesh"));
        for (float& lod : textureLod) {
            lod = reader.f32();
        }
    }

    const AnimSequence* UMesh::findSequence(Name sequence) const
    {
        for (const AnimSequence& candidate : sequences) {
            if (candidate.name == sequence) {
                return &candidate;
            }
        }
        return nullptr;
    }

    std::vector<MeshTriangle> UMesh::triangles() const
    {
        std::vector<MeshTriangle> result;
        result.reserve(tris.size());
        for (const MeshTri& tri : tris) {
            MeshTriangle triangle;
            for (int i = 0; i < 3; i++) {
                triangle.vertex[i] = tri.vertex[i];
                triangle.u[i] = static_cast<float>(tri.uv[i][0]);
                triangle.v[i] = static_cast<float>(tri.uv[i][1]);
            }
            triangle.polyFlags = tri.polyFlags;
            triangle.textureIndex = tri.textureIndex;
            result.push_back(triangle);
        }
        return result;
    }

    void UMesh::framePose(Name sequence, float animFrame, std::vector<Vector>& positions) const
    {
        const AnimSequence* found = findSequence(sequence);
        if (!found && !sequences.empty()) {
            found = &sequences.front();
        }
        framePose(found, animFrame, positions);
    }

    namespace {
        bool interpolatedFrame(const UMesh& mesh, const AnimSequence* sequence, float animFrame, std::vector<Vector>& raw)
        {
            size_t count = static_cast<size_t>(std::max(mesh.frameVerts, 0));
            raw.resize(count);
            int32_t startFrame = 0;
            int32_t numFrames = 1;
            if (sequence && sequence->numFrames > 0) {
                startFrame = sequence->startFrame;
                numFrames = sequence->numFrames;
            }
            // Negative or NaN animation positions start at frame zero. If the scaled frame
            // number is too large for int32_t, use INT32_MIN to match the original behavior.
            float frame = !(animFrame >= 0.0f) ? 0.0f : animFrame * static_cast<float>(numFrames);
            int32_t whole = frame < 2147483648.0f ? static_cast<int32_t>(frame) : std::numeric_limits<int32_t>::min();
            float t = frame - static_cast<float>(whole);
            int32_t frame0 = whole % numFrames;
            // Add one before taking the remainder: when whole is negative, advancing
            // from frame0 instead can select a different frame.
            int32_t frame1 = static_cast<int32_t>((static_cast<int64_t>(whole) + 1) % numFrames);
            int64_t index0 = static_cast<int64_t>(startFrame) + frame0;
            int64_t index1 = static_cast<int64_t>(startFrame) + frame1;
            int64_t frames = static_cast<int64_t>(count == 0 ? 0 : mesh.verts.size() / count);
            size_t base0 = 0;
            size_t base1 = 0;
            if (count == 0 || (index0 >= 0 && index0 < frames && index1 >= 0 && index1 < frames)) {
                base0 = static_cast<size_t>(index0) * count;
                base1 = static_cast<size_t>(index1) * count;
            } else {
                // The original reads outside the mesh here.
                t = 0.0f;
                if (count > mesh.verts.size()) {
                    raw.assign(count, Vector {});
                    return false;
                }
            }
            for (size_t i = 0; i < count; i++) {
                const Vector& a = mesh.verts[base0 + i];
                const Vector& b = mesh.verts[base1 + i];
                raw[i] = a + (b - a) * t;
            }
            return true;
        }

        void transformPose(const UMesh& mesh, const std::vector<Vector>& raw, std::vector<Vector>& positions)
        {
            positions.resize(raw.size());
            Coords axes = rotationAxes(mesh.rotOrigin);
            for (size_t i = 0; i < raw.size(); i++) {
                positions[i] = untransformVector(axes, (raw[i] - mesh.origin) * mesh.scale);
            }
        }
    }

    void UMesh::framePose(const AnimSequence* sequence, float animFrame, std::vector<Vector>& positions) const
    {
        std::vector<Vector> raw;
        if (!interpolatedFrame(*this, sequence, animFrame, raw)) {
            positions = raw;
            return;
        }
        transformPose(*this, raw, positions);
    }

    int32_t UMesh::tweenFrame(Name sequence, float animFrame, MeshPoseCache& cache, int32_t requested) const
    {
        size_t count = static_cast<size_t>(std::max(frameVerts, 0));
        int32_t vertexNum = std::min(requested, static_cast<int32_t>(count));
        bool lod = dynamic_cast<const ULodMesh*>(this) != nullptr;
        bool wasCached = cache.mesh == this && cache.verts.size() == count;
        if (!wasCached) {
            cache.mesh = this;
            cache.sequence = Name();
            cache.frame = 0;
            cache.tweenIndicator = 1.0f;
            cache.lodVerts = 0;
            cache.verts.assign(count, Vector {});
        }
        const AnimSequence* found = findSequence(sequence);
        if (animFrame >= 0 || !wasCached) {
            interpolatedFrame(*this, found, animFrame, cache.verts);
            cache.lodVerts = vertexNum;
            return vertexNum;
        }
        if (lod) {
            vertexNum = std::min(vertexNum, cache.lodVerts);
            cache.lodVerts = vertexNum;
        }
        float startFrame = found && found->numFrames > 0 ? -1.0f / static_cast<float>(found->numFrames) : 0.0f;
        size_t offset = found ? static_cast<size_t>(std::max(found->startFrame, 0)) * count : 0;
        if (offset + count > verts.size()) {
            offset = 0;
        }
        float alpha = 1.0f - animFrame / cache.frame;
        if (lod && cache.sequence != sequence) {
            cache.tweenIndicator = 0.0f;
        }
        if (cache.sequence != sequence || !(alpha >= 0.0f && alpha <= 1.0f)) {
            cache.sequence = sequence;
            cache.frame = startFrame;
            alpha = 0;
        }
        if (lod) {
            cache.tweenIndicator += (1.0f - cache.tweenIndicator) * alpha;
            if (cache.tweenIndicator > 0.97f) {
                alpha = 0;
                int32_t full = std::min(requested, static_cast<int32_t>(count));
                if (vertexNum < full && offset + count <= verts.size()) {
                    for (size_t i = static_cast<size_t>(vertexNum); i < static_cast<size_t>(full); i++) {
                        cache.verts[i] = verts[offset + i];
                    }
                    vertexNum = full;
                    cache.lodVerts = vertexNum;
                }
            }
        }
        if (alpha > 0.0f && offset + count <= verts.size()) {
            for (size_t i = 0; i < count; i++) {
                cache.verts[i] += (verts[offset + i] - cache.verts[i]) * alpha;
            }
        }
        cache.frame = animFrame;
        return vertexNum;
    }

    int32_t UMesh::animatedPose(const MeshAnimState& state, MeshPoseCaches& caches, std::vector<Vector>& positions, int32_t requested) const
    {
        int32_t vertexNum = tweenFrame(state.sequence, state.frame, caches.main, requested);
        std::vector<Vector> raw = caches.main.verts;
        for (int slot = 0; slot < 4; slot++) {
            Name blend = state.blendSequences[slot];
            if (blend.none() || !findSequence(blend) || verts.size() < raw.size()) {
                continue;
            }
            MeshPoseCache& cache = caches.blends[slot];
            tweenFrame(blend, state.blendFrames[slot], cache, static_cast<int32_t>(raw.size()));
            for (size_t i = 0; i < raw.size(); i++) {
                raw[i] += cache.verts[i] - verts[i];
            }
        }
        transformPose(*this, raw, positions);
        return vertexNum;
    }

    void ULodMesh::serialize(Reader& reader, Linker& source)
    {
        UMesh::serialize(reader, source);
        collapsePointThus = readWords(reader);
        faceLevel = readWords(reader);
        faces.resize(readCount(reader, "mesh"));
        for (MeshFace& face : faces) {
            face = readFace(reader);
        }
        collapseWedgeThus = readWords(reader);
        wedges.resize(readCount(reader, "mesh"));
        for (MeshWedge& wedge : wedges) {
            wedge.vertex = static_cast<uint16_t>(reader.i16());
            wedge.u = reader.u8();
            wedge.v = reader.u8();
        }
        materials.resize(readCount(reader, "mesh"));
        for (MeshMaterial& material : materials) {
            material.polyFlags = reader.u32();
            material.textureIndex = reader.i32();
        }
        specialFaces.resize(readCount(reader, "mesh"));
        for (MeshFace& face : specialFaces) {
            face = readFace(reader);
        }
        modelVerts = reader.u32();
        specialVerts = reader.u32();
        meshScaleMax = reader.f32();
        lodHysteresis = reader.f32();
        lodStrength = reader.f32();
        lodMinVerts = reader.u32();
        lodMorph = reader.f32();
        lodZDisplace = reader.f32();
        remapAnimVerts = readWords(reader);
        oldFrameVerts = reader.u32();
    }

    int32_t ULodMesh::poseVertex(int32_t wedgeVertex) const
    {
        size_t index = static_cast<size_t>(wedgeVertex) + specialVerts;
        if (!remapAnimVerts.empty() && index < remapAnimVerts.size()) {
            return remapAnimVerts[index];
        }
        return static_cast<int32_t>(index);
    }

    std::vector<MeshTriangle> ULodMesh::triangles() const
    {
        std::vector<MeshTriangle> result;
        result.reserve(faces.size());
        for (const MeshFace& face : faces) {
            if (face.material >= materials.size()) {
                continue;
            }
            const MeshMaterial& material = materials[face.material];
            MeshTriangle triangle;
            bool valid = true;
            for (int i = 0; i < 3; i++) {
                if (face.wedge[i] >= wedges.size()) {
                    valid = false;
                    break;
                }
                const MeshWedge& wedge = wedges[face.wedge[i]];
                triangle.vertex[i] = poseVertex(wedge.vertex);
                triangle.u[i] = static_cast<float>(wedge.u);
                triangle.v[i] = static_cast<float>(wedge.v);
            }
            if (!valid) {
                continue;
            }
            triangle.polyFlags = material.polyFlags;
            triangle.textureIndex = material.textureIndex;
            result.push_back(triangle);
        }
        return result;
    }

    void ULodMesh::lodTriangles(int32_t vertexSubset, bool morph, int32_t nonMorph, const std::vector<float>& alphas, std::vector<MeshTriangle>& result) const
    {
        result.clear();
        std::vector<int32_t> pool(wedges.size(), -1);
        std::vector<MeshWedge> resolved(wedges.size());
        for (size_t f = 0; f < faces.size(); f++) {
            if (f < faceLevel.size() && faceLevel[f] > vertexSubset) {
                continue;
            }
            const MeshFace& face = faces[f];
            if (face.material >= materials.size()) {
                continue;
            }
            MeshTriangle triangle;
            bool valid = true;
            for (int w = 0; w < 3 && valid; w++) {
                size_t start = face.wedge[w];
                if (start >= wedges.size()) {
                    valid = false;
                    break;
                }
                if (pool[start] < 0) {
                    size_t current = start;
                    MeshWedge wedge = wedges[current];
                    while (wedge.vertex >= vertexSubset && current < collapseWedgeThus.size()) {
                        current = collapseWedgeThus[current];
                        if (current >= wedges.size()) {
                            valid = false;
                            break;
                        }
                        wedge = wedges[current];
                    }
                    if (!valid) {
                        break;
                    }
                    if (morph && wedge.vertex > nonMorph && wedge.vertex < alphas.size() && alphas[wedge.vertex] != 0.0f && current < collapseWedgeThus.size()) {
                        size_t next = collapseWedgeThus[current];
                        if (next != current && next < wedges.size()) {
                            const MeshWedge& other = wedges[next];
                            if (wedge.u != other.u || wedge.v != other.v) {
                                float alpha = alphas[wedge.vertex];
                                wedge.u = static_cast<uint8_t>(std::nearbyint(static_cast<float>(wedge.u) + static_cast<float>(other.u - wedge.u) * alpha));
                                wedge.v = static_cast<uint8_t>(std::nearbyint(static_cast<float>(wedge.v) + static_cast<float>(other.v - wedge.v) * alpha));
                            }
                        }
                    }
                    resolved[start] = wedge;
                    pool[start] = 1;
                }
                const MeshWedge& wedge = resolved[start];
                triangle.vertex[w] = poseVertex(wedge.vertex);
                triangle.u[w] = static_cast<float>(wedge.u);
                triangle.v[w] = static_cast<float>(wedge.v);
            }
            if (!valid) {
                continue;
            }
            triangle.polyFlags = materials[face.material].polyFlags;
            triangle.textureIndex = materials[face.material].textureIndex;
            result.push_back(triangle);
        }
    }

    void UFireTexture::serialize(Reader& reader, Linker& source)
    {
        UTexture::serialize(reader, source);
        sparks.resize(readCount(reader, "mesh"));
        for (FireSpark& spark : sparks) {
            spark.type = reader.u8();
            spark.heat = reader.u8();
            spark.x = reader.u8();
            spark.y = reader.u8();
            spark.a = reader.u8();
            spark.b = reader.u8();
            spark.c = reader.u8();
            spark.d = reader.u8();
        }
    }

    void UMesh::addReferences(ReferenceVisitor& references)
    {
        UPrimitive::addReferences(references);
        references(textures);
    }

    void registerMeshResources()
    {
        gc::reportsReferences<UMesh, ULodMesh, UFireTexture>();
        objects::registerFactory(Name("Mesh"), [] { return new UMesh(); });
        objects::registerFactory(Name("LodMesh"), [] { return new ULodMesh(); });
        objects::registerFactory(Name("FireTexture"), [] { return new UFireTexture(); });
    }
}
