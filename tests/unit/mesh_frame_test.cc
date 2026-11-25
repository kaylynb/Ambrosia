#include "engine/mesh.h"

#include <catch_amalgamated.hpp>

#include <limits>
#include <vector>

namespace {
    // One vertex per frame, at x = the frame's index, so a pose shows which frames it used.
    void fillCountingMesh(dx::UMesh& mesh)
    {
        mesh.frameVerts = 1;
        for (int i = 0; i < 8; i++) {
            mesh.verts.push_back({ static_cast<float>(i), 0, 0 });
        }
        mesh.scale = { 1, 1, 1 };
    }

    float poseX(const dx::UMesh& mesh, const dx::AnimSequence& sequence, float animFrame)
    {
        std::vector<dx::Vector> pose;
        mesh.framePose(&sequence, animFrame, pose);
        REQUIRE(pose.size() == 1);
        return pose[0].x;
    }
}

TEST_CASE("mesh frames follow the original's arithmetic for out-of-range AnimFrame")
{
    dx::UMesh mesh;
    fillCountingMesh(mesh);
    dx::AnimSequence sequence;
    sequence.startFrame = 4;
    sequence.numFrames = 4;
    CHECK(poseX(mesh, sequence, 0.25f) == 5.0f);
    CHECK(poseX(mesh, sequence, 0.375f) == 5.5f);
    // A NaN or negative AnimFrame counts as 0.
    CHECK(poseX(mesh, sequence, std::numeric_limits<float>::quiet_NaN()) == 4.0f);
    CHECK(poseX(mesh, sequence, -3.0f) == 4.0f);
    // Past the int range the frame is 0x80000000: frame0 = 0, and the second frame is
    // (0x80000000 + 1) % 4 = -3, frame 1, with a huge blend toward it.
    CHECK(poseX(mesh, sequence, 1e30f) < -1e30f);
    // A sequence that would start before the mesh, or run past it, falls back to the first frame.
    sequence.startFrame = -2;
    CHECK(poseX(mesh, sequence, 0.0f) == 0.0f);
    sequence.startFrame = 6;
    CHECK(poseX(mesh, sequence, 0.5f) == 0.0f);
}
