#include "core/objects.h"
#include "test_paths.h"
#include "deusex/startup.h"
#include "engine/actor.h"
#include "engine/csg.h"
#include "engine/mesh.h"
#include "engine/props_engine.h"
#include "engine/world.h"

#include <catch_amalgamated.hpp>

namespace {
    void setupMesh(dx::UMesh& mesh)
    {
        mesh.frameVerts = 1;
        mesh.animFrames = 4;
        mesh.scale = { 1, 1, 1 };
        mesh.verts = { { 0, 0, 0 }, { 10, 0, 0 }, { 100, 0, 0 }, { 200, 0, 0 } };
        dx::AnimSequence first;
        first.name = dx::Name("TweenTestA");
        first.startFrame = 0;
        first.numFrames = 2;
        first.rate = 10;
        dx::AnimSequence second;
        second.name = dx::Name("TweenTestB");
        second.startFrame = 2;
        second.numFrames = 2;
        second.rate = 10;
        mesh.sequences = { first, second };
    }
}

TEST_CASE("tweening starts from the last drawn pose and moves toward the new sequence")
{
    dx::UMesh mesh;
    setupMesh(mesh);
    dx::MeshPoseCache cache;
    mesh.tweenFrame(dx::Name("TweenTestA"), 0.25f, cache, 1);
    REQUIRE(cache.verts.size() == 1);
    CHECK(cache.verts[0].x == 5.0f);
    mesh.tweenFrame(dx::Name("TweenTestB"), -0.5f, cache, 1);
    CHECK(cache.verts[0].x == 5.0f);
    mesh.tweenFrame(dx::Name("TweenTestB"), -0.25f, cache, 1);
    CHECK(cache.verts[0].x == 52.5f);
    mesh.tweenFrame(dx::Name("TweenTestB"), -0.125f, cache, 1);
    CHECK(cache.verts[0].x == 76.25f);
    mesh.tweenFrame(dx::Name("TweenTestB"), 0.0f, cache, 1);
    CHECK(cache.verts[0].x == 100.0f);
}

TEST_CASE("blend channels add their offset from the first mesh frame")
{
    dx::UMesh mesh;
    setupMesh(mesh);
    dx::MeshAnimState state;
    state.sequence = dx::Name("TweenTestA");
    state.frame = 0.5f;
    state.blendSequences[0] = dx::Name("TweenTestB");
    state.blendFrames[0] = 0.0f;
    dx::MeshPoseCaches caches;
    std::vector<dx::Vector> pose;
    mesh.animatedPose(state, caches, pose, 1);
    REQUIRE(pose.size() == 1);
    CHECK(pose[0].x == 110.0f);
    CHECK(pose[0].y == 0.0f);
    CHECK(pose[0].z == 0.0f);
}

TEST_CASE("LOD mesh tweening stops once the tween indicator passes 0.97")
{
    dx::ULodMesh mesh;
    setupMesh(mesh);
    dx::MeshPoseCache cache;
    mesh.tweenFrame(dx::Name("TweenTestA"), 0.25f, cache, 1);
    mesh.tweenFrame(dx::Name("TweenTestB"), -0.5f, cache, 1);
    const float frames[] = { -0.25f, -0.125f, -0.0625f, -0.03125f, -0.015625f };
    for (float frame : frames) {
        mesh.tweenFrame(dx::Name("TweenTestB"), frame, cache, 1);
    }
    CHECK(cache.verts[0].x == 97.03125f);
    mesh.tweenFrame(dx::Name("TweenTestB"), -0.0078125f, cache, 1);
    CHECK(cache.verts[0].x == 97.03125f);
    CHECK(cache.tweenIndicator > 0.97f);
}

TEST_CASE("blend animations advance in the main animation's unused passes and stop with it", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UObject* package = dx::objects::packageObject(dx::Name("BlendAnimTest"));
    dx::UObject* meshObject = dx::objects::newObject(dx::csg::nativeClass("Mesh"), package, dx::Name("BlendTestMesh"), 0);
    meshObject->loaded = true;
    dx::UMesh* mesh = dx::cast<dx::UMesh>(meshObject);
    REQUIRE(mesh != nullptr);
    setupMesh(*mesh);
    dx::UObject* a = dx::objects::newObject(dx::objects::findClass("Engine.Actor"), package, dx::Name(), 0);
    a->loaded = true;
    dx::actor::Mesh(a) = mesh;
    dx::actor::AnimSequence(a) = dx::Name("TweenTestA");
    dx::actor::AnimFrame(a) = 0.0f;
    dx::actor::AnimRate(a) = 1.0f;
    dx::actor::AnimLast(a) = 0.5f;
    dx::actor::bAnimLoop(a) = 1;
    dx::actor::BlendAnimSequence(a, 1) = dx::Name("TweenTestB");
    dx::actor::BlendAnimFrame(a, 1) = 0.0f;
    dx::actor::BlendAnimRate(a, 1) = 1.0f;
    dx::actor::BlendAnimLast(a, 1) = 0.5f;

    // The main animation takes one pass; the blend slot gets the other three, each
    // with the whole tick's time.
    dx::updateAnimation(a, 0.01f);
    CHECK(dx::actor::AnimFrame(a) == Catch::Approx(0.01f));
    CHECK(dx::actor::BlendAnimFrame(a, 1) == Catch::Approx(0.03f));

    dx::actor::AnimRate(a) = 0.0f;
    dx::updateAnimation(a, 0.01f);
    CHECK(dx::actor::BlendAnimFrame(a, 1) == Catch::Approx(0.03f));
    dx::objects::destroyObject(a);
}

TEST_CASE("LOD mesh tweening cannot return more vertices than the cached pose had")
{
    dx::ULodMesh mesh;
    setupMesh(mesh);
    dx::MeshPoseCache cache;
    CHECK(mesh.tweenFrame(dx::Name("TweenTestA"), 0.25f, cache, 0) == 0);
    CHECK(mesh.tweenFrame(dx::Name("TweenTestB"), -0.5f, cache, 1) == 0);
    CHECK(mesh.tweenFrame(dx::Name("TweenTestA"), 0.5f, cache, 1) == 1);
}
