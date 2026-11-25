#pragma once

#include "audio/acoustic_baker.h"
#include "audio/acoustics.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// The Ambrosia raytraced sound mode's controls and status, for the debug overlay, ucc and tests.
// Whether it is on is audio::raytracedActive, in audio/audio.h with the other modes.
namespace dx::audio {
    // The raytraced mode's view of the level, as last built, or null.
    std::shared_ptr<const acoustics::Geometry> acousticGeometry();

    struct AcousticStatus {
        // Whether this build has Steam Audio at all (AMBROSIA_STEAM_AUDIO).
        bool built = false;
        bool active = false;
        bool unavailable = false;
        // Exporting the level's geometry on the game thread, at the last level change.
        double exportMilliseconds = 0;
        acoustics::Stats simulation;
    };

    AcousticStatus acousticStatus();
    // The raytraced mode's material table: the one in acoustic_scene.cc until the debug overlay
    // changes it, which lasts for this run and reaches the scene at once. The table keeps its
    // entries; only their values change.
    const std::vector<acoustics::Material>& acousticMaterials();
    void setAcousticMaterials(const std::vector<acoustics::Material>& materials);
    // Waits for the raytraced mode's worker to take in everything handed to it, for offline
    // renders and tests.
    void settleAcoustics();

    // The raytraced mode's reverb: its level over the reverb send (AmbrosiaReverbLevel, read with
    // the other settings), whether the zone's authored reverb plays instead for comparison, the
    // reflection simulation's rays and bounces, and the decay times the mixer last used. Only
    // the level is a setting; the rest last for this run.
    struct ReverbControls {
        float level = 1;
        bool authored = false;
        acoustics::ReflectionSettings reflections;
        std::array<float, 3> times {};
    };

    ReverbControls reverbControls();
    void setReverbControls(const ReverbControls& controls);
    // For tests: a scene of their own, kept while update is given no world.
    void setAcousticGeometry(std::shared_ptr<const acoustics::Geometry> geometry);

    // Baked acoustics for the level played: where its data stands (found, baking, in use), and a
    // bake of it again whatever the cache holds.
    acoustics::BakeReport bakeReport();
    void rebakeAcoustics();
    // Which baked data is used (AmbrosiaBakedReverb and AmbrosiaPathing, read with the other
    // settings); the overlay's switches change it for this run.
    acoustics::BakedUse bakedUse();
    void setBakedUse(const acoustics::BakedUse& use);
    // The voice whose baked path is kept for drawing, or 0, and its legs.
    void setTracedVoice(int32_t id);
    std::vector<acoustics::PathSegment> tracedPath();
    // For tests: baked data of their own for the scene setAcousticGeometry gave.
    void setAcousticBaked(std::shared_ptr<const acoustics::BakedData> data);
    // Where the level's movers last were, as handed to the simulation; tests set them.
    const std::vector<acoustics::MoverPose>& acousticMoverPoses();
    void setAcousticMoverPoses(std::vector<acoustics::MoverPose> poses);
}
