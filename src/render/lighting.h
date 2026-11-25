#pragma once

#include "core/math.h"
#include "core/object.h"

#include <cstdint>

namespace dx {
    Vector fGetHSV(uint8_t hue, uint8_t saturation, uint8_t value);
    float lightDistanceFalloff(float distanceSquaredNormalized);
    float lightRadius(UObject* light);
    Vector lightColor(UObject* light, float time);
    Vector lightColor(UObject* light, float time, float* brightnessOut);

    int32_t truncateToInt(double value);
    int32_t appFloor(float value);
    float sinTab(int32_t angle);
    float cosTab(int32_t angle);
    float sqrtApprox(float value);
    // Once a frame. paused: the level is paused, as behind a menu, where Steady frame timing
    // holds the flicker lights.
    void tickLightRandoms(float timeSeconds, bool paused = false);
    float lightRandomBase(int32_t index);
    float lightRandom(int32_t index);
    bool lightEffectSpatialDynamic(uint8_t effect);
    bool lightEffectMergeDynamic(uint8_t effect);
    bool lightBakedOnSurfaces(UObject* light);
    bool lightMovesOnSurfaces(UObject* light);
    bool lightOnSurfaces(UObject* light);
    // Whether a light can ever reach surfaces, whatever its type and brightness now: a
    // TriggerLight or lamp that is off when the lightmaps are made can be turned on later.
    bool lightMayLightSurfaces(UObject* light);
    float autoPanTexels(float timeSeconds, float speed);
    float smallWavyPanU(float timeSeconds);
    float smallWavyPanV(float timeSeconds);

    struct PreparedLight {
        Vector location;
        float radius = 0;
        float radiusSquared = 0;
        uint8_t effect = 0;
        uint8_t period = 0;
        uint8_t phase = 0;
        Vector spotDirection;
        float outer = 0;
    };

    PreparedLight prepareLight(UObject* light, const Vector& lightLocation);
    float lightIllumination(const PreparedLight& light, const Vector& point, const Vector& normal, float time);
    float lightIllumination(UObject* light, const Vector& lightLocation, const Vector& point, const Vector& normal, float time);
    int32_t lightIntensity(const PreparedLight& light, const Vector& point, const Vector& normal, float time, int32_t source);
    bool isLightSource(UObject* actor);

    struct VolumetricLight {
        Vector location;
        float radius = 0;
        float radiusSquared = 0;
        float reciprocalRadius = 0;
        float reciprocalRadiusSquared = 0;
        float locationSizeSquared = 0;
        float brightness = 0;
        bool inside = false;
    };

    VolumetricLight prepareVolumetric(const Vector& relativeLocation, uint8_t volumeRadius, float brightness, uint8_t volumeBrightness);
    float volumetricFog(const VolumetricLight& light, const Vector& vertex);
    uint8_t fogMerge(uint8_t dest, uint8_t add);
    Vector zoneAmbient(UObject* zone);
}
