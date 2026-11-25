#pragma once

namespace dx {
    class UObject;

    // How fast flicker and strobe lights change under Steady frame timing (docs/differences.md, "Frame-rate dependence").
    // The original changes them every frame, which is a harsh strobe at modern frame rates, and
    // their ambient sounds follow the light. Original frame timing keeps that.
    //
    // Flicker: how many times a second each LT_Flicker light draws a new random brightness.
    // About half of the draws switch it between dark and lit, so 5 gives about 2.5 switches, or
    // 1.25 flashes, a second.
    inline constexpr double SteadyFlickerRate = 5.0;
    // Strobe: how many times a second LT_Strobe lights switch between lit and dark, so 5 gives
    // 2.5 flashes a second. Photosensitivity guidelines allow up to three.
    inline constexpr double SteadyStrobeRate = 5.0;
    // The effect clock: how many times a second the effects below change under Steady timing,
    // so they look as at 30 fps whatever the frame rate. The original changes them every tick
    // or every frame, faster the higher the frame rate. The actors' script Tick runs on this
    // clock, given whole periods of time (World::effectStep, deusex::onEffectClock).
    //  - ElectricityEmitter: its script draws a new random arc every tick and checks its
    //    flicker at most once a tick, switching the beam, its hum and its light (LT_Steady or
    //    LT_None, so not a flicker light).
    //  - Every laser beam's drawing (LaserIterator's CurrentItem): an electric beam's random
    //    bends, and the random point along any beam where it draws one segment twice, are
    //    drawn again only when the clock steps, where the original draws new ones every frame.
    //  - LaserTrigger and BeamTrigger while confused by EMP: each tick turns the beam on with a
    //    chance of 1 in 20 and off otherwise.
    inline constexpr double SteadyEffectRate = 30.0;

    // Whether a strobe light is in its dark half. The original has one strobe routine
    // with one toggle for the whole process, flipped whenever the
    // light's level time differs from the last call. The renderer uses it for every light, and
    // the audio subsystem reaches the same routine through URender::GlobalLighting
    // for the volume of an ambient sound, so both share the toggle.
    // `time` is used when the light has no level. Steady frame timing instead switches
    // SteadyStrobeRate times a second of level time, and holds while the level is paused
    // (LevelInfo.Pauser set, as behind a menu), where the original goes on switching.
    bool strobeDark(UObject* light, float time);
}
