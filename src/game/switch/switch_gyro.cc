#include "game/switch_gyro.h"

#include "core/math.h"

#include <algorithm>

#include <switch.h>

namespace dx::game::switchgyro {
    namespace {
        struct Sensor {
            HidSixAxisSensorHandle handle {};
            u64 lastSample = 0;
            bool started = false;
        };

        // Handheld, a Pro Controller (full key) and a Joy-Con pair's right half, the one under
        // the aiming thumb.
        Sensor handheld;
        Sensor fullKey;
        Sensor joyDual;

        // Starts a sensor not yet started; one that failed is tried again next time.
        void begin(Sensor& sensor, HidNpadIdType id, HidNpadStyleTag style, int index)
        {
            if (sensor.started) {
                return;
            }
            HidSixAxisSensorHandle handles[2] {};
            if (R_SUCCEEDED(hidGetSixAxisSensorHandles(handles, index + 1, id, style)) && R_SUCCEEDED(hidStartSixAxisSensor(handles[index]))) {
                sensor.handle = handles[index];
                sensor.started = true;
            }
        }

        void end(Sensor& sensor)
        {
            if (sensor.started) {
                hidStopSixAxisSensor(sensor.handle);
                sensor = {};
            }
        }
    }

    void start()
    {
        begin(handheld, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 0);
        begin(fullKey, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 0);
        begin(joyDual, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 1);
    }

    void stop()
    {
        end(handheld);
        end(fullKey);
        end(joyDual);
    }

    std::optional<Sample> poll()
    {
        Sensor* sensor = nullptr;
        if (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) {
            sensor = &handheld;
        } else if (u32 style = hidGetNpadStyleSet(HidNpadIdType_No1); style & HidNpadStyleTag_NpadFullKey) {
            sensor = &fullKey;
        } else if (style & HidNpadStyleTag_NpadJoyDual) {
            sensor = &joyDual;
        }
        if (!sensor || !sensor->started) {
            return std::nullopt;
        }
        HidSixAxisSensorState states[16] {};
        size_t count = hidGetSixAxisSensorStates(sensor->handle, states, 16);
        gyro::Turn sum;
        Vector accelerometer;
        int used = 0;
        u64 newest = sensor->lastSample;
        for (size_t i = 0; i < count; i++) {
            if (states[i].sampling_number <= sensor->lastSample) {
                continue;
            }
            newest = std::max(newest, states[i].sampling_number);
            // The sensor's axes are x right, y away from the player and z up, SDL's x, -z and
            // y. Rates are in revolutions a second, counter-clockwise positive as in SDL's
            // convention, and acceleration in g. To be checked on the console.
            sum.pitch += states[i].angular_velocity.x;
            sum.yaw += states[i].angular_velocity.z;
            sum.roll -= states[i].angular_velocity.y;
            accelerometer += Vector { states[i].acceleration.x, states[i].acceleration.z, -states[i].acceleration.y };
            used++;
        }
        sensor->lastSample = newest;
        if (used == 0) {
            return std::nullopt;
        }
        float scale = 2.0f * Pi / static_cast<float>(used);
        return Sample { { sum.pitch * scale, sum.yaw * scale, sum.roll * scale }, accelerometer * (1.0f / static_cast<float>(used)) };
    }
}
