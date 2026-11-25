#pragma once

// Building Steam Audio meshes from the exported level, shared by the live simulation and the
// bake, and handles that own the scenes and meshes. Only for code built with DX_STEAM_AUDIO.

#if DX_STEAM_AUDIO
#include "audio/acoustic_scene.h"
#include "audio/steam_api.h"
#include "core/log.h"

#include <array>
#include <memory>
#include <type_traits>
#include <vector>

namespace dx::audio::acoustics::steamscene {
    inline IPLMaterial material(const Material& material)
    {
        IPLMaterial out {};
        for (size_t band = 0; band < 3; band++) {
            out.absorption[band] = material.absorption[band];
            out.transmission[band] = material.transmission[band];
        }
        out.scattering = material.scattering;
        return out;
    }

    inline IPLVector3 point(const Vector& unreal)
    {
        std::array<float, 3> p = toSteam(unreal);
        return { p[0], p[1], p[2] };
    }

    // A point in Steam Audio's space back in Unreal units and axes.
    inline Vector unreal(const IPLVector3& steam)
    {
        return { steam.x * UnitsPerMetre, steam.z * UnitsPerMetre, steam.y * UnitsPerMetre };
    }

    // Handles that free what they hold when they go. Adding a mesh to a scene or taking it out
    // only takes effect when the scene is committed, and until then the scene keeps the mesh and
    // the mesh keeps the scene. A mesh's handle takes it out of its scene and releases it; the
    // scene's handle commits, which frees what was taken out, and releases the scene. So a scene's
    // handle must go after the handles of what is in it: declare it before them.
    struct SceneRelease {
        const steam::Api* api = nullptr;

        void operator()(IPLScene scene) const
        {
            api->iplSceneCommit(scene);
            api->iplSceneRelease(&scene);
        }
    };

    struct StaticMeshRelease {
        const steam::Api* api = nullptr;
        IPLScene scene = nullptr;

        void operator()(IPLStaticMesh mesh) const
        {
            api->iplStaticMeshRemove(mesh, scene);
            api->iplStaticMeshRelease(&mesh);
        }
    };

    // Outside InstancedMesh, which GCC would not default-construct with it inside.
    struct InstancedMeshRelease {
        const steam::Api* api = nullptr;
        IPLScene scene = nullptr;
        bool added = false;

        void operator()(IPLInstancedMesh mesh) const
        {
            if (added) {
                api->iplInstancedMeshRemove(mesh, scene);
            }
            api->iplInstancedMeshRelease(&mesh);
        }
    };

    using Scene = std::unique_ptr<std::remove_pointer_t<IPLScene>, SceneRelease>;
    // A static mesh in the scene it was made for.
    using StaticMesh = std::unique_ptr<std::remove_pointer_t<IPLStaticMesh>, StaticMeshRelease>;

    // An empty scene, or none when Steam Audio refuses it.
    inline Scene makeScene(const steam::Api* api, const IPLSceneSettings& settings)
    {
        IPLSceneSettings copy = settings;
        IPLScene created = nullptr;
        if (api->iplSceneCreate(api->context, &copy, &created) != IPL_STATUS_SUCCESS) {
            return Scene(nullptr, { api });
        }
        return Scene(created, { api });
    }

    // Another scene placed in a scene, which it is in only while added.
    class InstancedMesh {
    public:
        InstancedMesh() = default;

        // An instance of sub in target, not yet added, or none when Steam Audio refuses it.
        static InstancedMesh make(const steam::Api* api, IPLScene target, IPLScene sub, const IPLMatrix4x4& transform)
        {
            IPLInstancedMeshSettings settings {};
            settings.subScene = sub;
            settings.transform = transform;
            IPLInstancedMesh created = nullptr;
            InstancedMesh made;
            if (api->iplInstancedMeshCreate(target, &settings, &created) == IPL_STATUS_SUCCESS) {
                made.instance = Handle(created, { api, target, false });
            }
            return made;
        }

        explicit operator bool() const
        {
            return instance != nullptr;
        }

        bool added() const
        {
            return instance.get_deleter().added;
        }

        void add()
        {
            Release& release = instance.get_deleter();
            release.api->iplInstancedMeshAdd(instance.get(), release.scene);
            release.added = true;
        }

        void remove()
        {
            Release& release = instance.get_deleter();
            release.api->iplInstancedMeshRemove(instance.get(), release.scene);
            release.added = false;
        }

        void place(const IPLMatrix4x4& transform)
        {
            const Release& release = instance.get_deleter();
            release.api->iplInstancedMeshUpdateTransform(instance.get(), release.scene, transform);
        }

    private:
        using Release = InstancedMeshRelease;
        using Handle = std::unique_ptr<std::remove_pointer_t<IPLInstancedMesh>, Release>;
        Handle instance;
    };

    // Makes geometry's triangles a static mesh of target, with the material table, and adds it.
    // None when Steam Audio refuses it.
    inline StaticMesh makeMesh(const steam::Api* api, IPLScene target, const Geometry& geometry, const std::vector<Material>& table)
    {
        std::vector<IPLVector3> vertices(geometry.vertices.size());
        for (size_t i = 0; i < vertices.size(); i++) {
            vertices[i] = point(geometry.vertices[i]);
        }
        std::vector<IPLTriangle> triangles(geometry.triangles.size());
        for (size_t i = 0; i < triangles.size(); i++) {
            triangles[i] = { { geometry.triangles[i][0], geometry.triangles[i][1], geometry.triangles[i][2] } };
        }
        std::vector<IPLMaterial> materials;
        for (const Material& entry : table) {
            materials.push_back(material(entry));
        }
        std::vector<IPLint32> materialIndices(geometry.triangleMaterial.begin(), geometry.triangleMaterial.end());
        IPLStaticMeshSettings mesh {};
        mesh.numVertices = static_cast<IPLint32>(vertices.size());
        mesh.numTriangles = static_cast<IPLint32>(triangles.size());
        mesh.numMaterials = static_cast<IPLint32>(materials.size());
        mesh.vertices = vertices.data();
        mesh.triangles = triangles.data();
        mesh.materialIndices = materialIndices.data();
        mesh.materials = materials.data();
        IPLStaticMesh created = nullptr;
        if (api->iplStaticMeshCreate(target, &mesh, &created) != IPL_STATUS_SUCCESS) {
            log::error("DevAudio", "Steam Audio: a static mesh could not be created");
            return StaticMesh(nullptr, { api, target });
        }
        api->iplStaticMeshAdd(created, target);
        return StaticMesh(created, { api, target });
    }
}
#endif
