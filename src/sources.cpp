// SPDX-License-Identifier: MPL-2.0
#include "sources.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nbt/app/car_data.hpp"

namespace gauges {

// manager/docs/car-data-properties.tsv: the names and iDrive's JSON formats.
const char *const kCarDataNames[] = {
    "engine.RPMSpeed",           // {"RPMSpeed":%hu}
    "engine.temperature",        // {"temperature":{"engine":%d,"oil":%d}}
    "engine.torque",             // {"torque":%hd}
    "driving.acceleratorPedal",  // {"acceleratorPedal":{"position":%hu,"ecoPosition":%hu}}
    "driving.speedActual",       // {"speedActual":%hu}
    "sensors.temperatureExterior", // {"temperatureExterior":%1.1f}
    "driving.acceleration",      // {"acceleration":{"lateral":%1.2f,"longitudinal":%1.2f}}
    "driving.steeringWheel",     // {"steeringWheel":{"angle":%1.1f,"speed":%1.1f,"error":%d}}
    "driving.gear",              // {"gear":%d}
    "driving.mode",              // {"mode":%d}
};
const size_t kCarDataNameCount = sizeof(kCarDataNames) / sizeof(kCarDataNames[0]);

bool json_number(const char *text, const char *key, double *out) {
    const size_t k = strlen(key);
    for (const char *p = strchr(text, '"'); p; p = strchr(p + 1, '"')) {
        if (strncmp(p + 1, key, k) != 0 || p[1 + k] != '"') {
            continue;
        }
        const char *v = p + 2 + k;
        while (*v == ' ') {
            ++v;
        }
        if (*v != ':') {
            continue;
        }
        ++v;
        char *end = NULL;
        const double d = strtod(v, &end);
        if (end == v || isnan(d) || isinf(d)) {
            return false;
        }
        *out = d;
        return true;
    }
    return false;
}

bool car_data_value(int channel, double raw, float *out) {
    switch (channel) {
    case kGear:
        return car_data_gear(static_cast<int>(raw), out);
    case kAccelLat:
    case kAccelLong:
        // 65535 when the car has none.
        if (raw > 650.0 || raw < -650.0) {
            return false;
        }
        break;
    default:
        break;
    }
    *out = static_cast<float>(raw);
    return true;
}

int poll_car_data(const nbt::app::CarData &car, Store *store, uint64_t now_ms) {
    struct Map {
        const char *name, *key;
        int channel;
    };
    // The temperatures as iDrive gives them (degrees Celsius, unconfirmed in
    // the car: README, "Car test").
    static const Map kMap[] = {
        {"engine.RPMSpeed", "RPMSpeed", kRpm},
        {"engine.temperature", "engine", kCoolant},
        {"engine.temperature", "oil", kOil},
        {"engine.torque", "torque", kTorque},
        {"driving.acceleratorPedal", "position", kPedal},
        {"driving.speedActual", "speedActual", kSpeed},
        {"sensors.temperatureExterior", "temperatureExterior", kAmbient},
        // m/s2 and degrees as the car's bus carries them (unconfirmed in the
        // car: README, "Car test").
        {"driving.acceleration", "lateral", kAccelLat},
        {"driving.acceleration", "longitudinal", kAccelLong},
        {"driving.steeringWheel", "angle", kSteering},
        // N, R, P, then the gears; the driving experience control's mode.
        {"driving.gear", "gear", kGear},
        {"driving.mode", "mode", kDriveMode},
    };
    static uint64_t sequences[sizeof(kMap) / sizeof(kMap[0])];
    int fresh_values = 0;
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
        char value[256];
        uint64_t sequence = 0;
        double number = 0;
        if (!car.read(kMap[i].name, value, sizeof(value), &sequence) ||
            !json_number(value, kMap[i].key, &number)) {
            continue;
        }
        // iDrive writes a value when it changes: an unchanged one is still
        // current, so it is set again (fresh) each time.
        float v = 0;
        if (!car_data_value(kMap[i].channel, number, &v)) {
            continue;
        }
        store->set(kMap[i].channel, v, kCarData, now_ms);
        if (sequence != sequences[i]) {
            sequences[i] = sequence;
            ++fresh_values;
        }
    }
    return fresh_values;
}

namespace {

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// 0..1 smooth step.
float ease(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Full-load torque against engine speed, Nm: an M5's twin-turbo V8 (S63,
// F90) as a dyno pull draws it, its ripple smoothed away. The turbos build to
// 800 Nm by 2200 rpm, 824 Nm at 2900, a slow slope to 5750, then away:
// 466 kW (626 hp) at 5900.
const struct {
    float rpm, nm;
} kFullLoad[] = {
    {900, 250},  {1300, 339}, {1500, 407}, {1750, 536}, {2000, 685}, {2150, 778}, {2300, 802}, {2600, 806},
    {2900, 824}, {3200, 802}, {3600, 794}, {4000, 786}, {4400, 784}, {4800, 778}, {5200, 766}, {5500, 766},
    {5750, 768}, {6000, 743}, {6200, 705}, {6400, 653}, {6600, 603}, {6850, 546}, {7000, 515}, {7300, 460},
};
const int kFullLoadPoints = static_cast<int>(sizeof(kFullLoad) / sizeof(kFullLoad[0]));

// The slope at a point: the mean of the two segments beside it.
float full_load_slope(int i) {
    const int a = i > 0 ? i - 1 : i, b = i < kFullLoadPoints - 1 ? i + 1 : i;
    return (kFullLoad[b].nm - kFullLoad[a].nm) / (kFullLoad[b].rpm - kFullLoad[a].rpm);
}

// A smooth curve through the points (cubic Hermite).
float full_load(float rpm) {
    if (rpm <= kFullLoad[0].rpm) {
        return kFullLoad[0].nm;
    }
    if (rpm >= kFullLoad[kFullLoadPoints - 1].rpm) {
        return kFullLoad[kFullLoadPoints - 1].nm;
    }
    int i = 0;
    while (rpm > kFullLoad[i + 1].rpm) {
        ++i;
    }
    const float h = kFullLoad[i + 1].rpm - kFullLoad[i].rpm, u = (rpm - kFullLoad[i].rpm) / h;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * kFullLoad[i].nm + (u3 - 2 * u2 + u) * h * full_load_slope(i) +
           (-2 * u3 + 3 * u2) * kFullLoad[i + 1].nm + (u3 - u2) * h * full_load_slope(i + 1);
}

} // namespace

void demo_values(uint64_t t_ms, Store *store, uint64_t now_ms, bool diesel) {
    // A 24 s loop: idle 4 s, a pull 2nd to 3rd gear 8 s, lift 3 s, cruise 9 s
    // through a long left-hand bend.
    const float t = static_cast<float>(t_ms % 24000u) / 1000.0f;
    float rpm, load, pedal, speed, along, lat = 0, steer = 0, share;
    if (t < 4.0f) {
        rpm = 780.0f + 15.0f * sinf(t * 6.0f);
        load = 0.18f;
        pedal = 0;
        speed = 0;
        along = 0;
        share = 40.0f; // xDrive's preload for a start
    } else if (t < 12.0f) {
        const float u = t - 4.0f;
        const float gear = u < 4.5f ? u / 4.5f : (u - 4.5f) / 3.5f;
        // From idle to 7050 rpm, twice: the whole curve is drawn.
        rpm = u < 4.5f ? 900.0f + 6150.0f * ease(gear) : 4600.0f + 2450.0f * ease(gear);
        load = clampf(u * 8.0f, 0.0f, 1.0f);
        pedal = 100.0f;
        speed = 135.0f * (1.0f - expf(-u / 4.5f)) / (1.0f - expf(-8.0f / 4.5f));
        along = 4.6f * expf(-u / 6.0f);
        share = 42.0f - 2.0f * u;
        if (u > 4.4f && u < 4.7f) {
            load = 0.3f; // the shift
            along = 0.8f;
            share = 20.0f;
        }
    } else if (t < 15.0f) {
        const float u = (t - 12.0f) / 3.0f;
        rpm = 7050.0f - 4350.0f * ease(u);
        load = 0.0f;
        pedal = 0;
        speed = 135.0f - 50.0f * ease(u);
        along = -2.4f;
        share = 2.0f;
    } else {
        const float u = t - 15.0f;
        rpm = 2200.0f + 60.0f * sinf(u * 0.9f);
        load = 0.32f + 0.05f * sinf(u * 1.7f);
        pedal = 18.0f + 3.0f * sinf(u * 1.7f);
        speed = 85.0f + 2.0f * sinf(u * 0.9f);
        along = 0.3f * sinf(u * 0.9f);
        // The bend: in, held, out (steering 45 degrees, 0.45 g).
        const float bend = sinf(3.14159265f * clampf(u / 9.0f, 0.0f, 1.0f));
        steer = 70.0f * bend + 4.0f * sinf(u * 2.3f) * bend;
        lat = 4.4f * bend;
        share = 14.0f + 8.0f * bend;
    }
    const float boost = load > 0.25f ? -45.0f + 175.0f * ease((load - 0.25f) / 0.75f) *
                                                       ease((rpm - 1500.0f) / 1500.0f)
                                     : -60.0f + 50.0f * load;
    const float lambda = pedal > 90.0f && boost > 80.0f ? 0.80f + 0.02f * sinf(t * 9.0f)
                         : load <= 0.01f                 ? 1.30f
                                                         : 1.0f + 0.01f * sinf(t * 13.0f);
    const float timing =
        load <= 0.01f ? 2.0f : 38.0f - 26.0f * clampf(boost / 130.0f, 0.0f, 1.0f) + 2.0f * sinf(t);
    store->set(kRpm, rpm, kDemo, now_ms);
    store->set(kBoost, boost, kDemo, now_ms);
    store->set(kBoostTarget, boost > 0 ? boost * 1.04f + 3.0f : boost, kDemo, now_ms);
    store->set(kMap, boost + 101.3f, kDemo, now_ms);
    store->set(kBaro, 101.3f, kDemo, now_ms);
    store->set(kLambda, lambda, kDemo, now_ms);
    store->set(kLambdaTarget, pedal > 90.0f && boost > 80.0f ? 0.80f : 1.0f, kDemo, now_ms);
    store->set(kTiming, timing, kDemo, now_ms);
    store->set(kRail, 5000.0f + 13000.0f * clampf(load, 0.0f, 1.0f), kDemo, now_ms);
    store->set(kFuelLow, 520.0f - 30.0f * load, kDemo, now_ms);
    store->set(kOilPressure, 180.0f + rpm * 0.06f, kDemo, now_ms);
    store->set(kCoolant, 98.0f + 4.0f * load, kDemo, now_ms);
    store->set(kOil, 104.0f + 0.0004f * static_cast<float>(t_ms % 600000u) / 100.0f, kDemo, now_ms);
    store->set(kIntakeAir, 31.0f + 6.0f * load, kDemo, now_ms);
    store->set(kChargeAir, 33.0f + 14.0f * load, kDemo, now_ms);
    store->set(kAmbient, 18.5f, kDemo, now_ms);
    store->set(kCatalyst, 520.0f + 300.0f * load, kDemo, now_ms);
    store->set(kThrottle, pedal > 0 ? clampf(pedal * 0.9f, 0.0f, 100.0f) : 3.0f, kDemo, now_ms);
    store->set(kPedal, pedal, kDemo, now_ms);
    store->set(kLoad, 100.0f * load, kDemo, now_ms);
    store->set(kMaf, 4.0f + 0.045f * rpm * load, kDemo, now_ms);
    store->set(kTrimShort, 1.5f * sinf(t * 2.0f), kDemo, now_ms);
    store->set(kTrimLong, -2.3f, kDemo, now_ms);
    store->set(kVoltage, 14.2f + 0.1f * sinf(t * 0.5f), kDemo, now_ms);
    const float full = full_load(rpm);
    const float torque = 15.0f + (full - 15.0f) * load;
    store->set(kTorque, torque, kDemo, now_ms);
    store->set(kSpeed, speed, kDemo, now_ms);
    // Corrections: a little knock on cylinders 2 and 5 during the pull.
    for (int c = 0; c < 6; ++c) {
        float corr = 0;
        if (t > 7.0f && t < 11.0f && (c == 1 || c == 4)) {
            corr = -1.5f * ease((t - 7.0f) / 0.5f) * (1.0f - ease((t - 9.5f) / 1.5f));
        } else if (t > 7.0f && t < 11.0f) {
            corr = -0.4f * (c % 2);
        }
        store->set(kCorr1 + c, corr, kDemo, now_ms);
    }
    // The wheels: the rear ones spin at the start of the pull and a little
    // after the shift; in the bend the outer (right) wheels run faster.
    float rear = 0;
    if (t > 4.0f && t < 6.5f) {
        rear = 9.0f * ease((t - 4.0f) / 0.3f) * (1.0f - ease((t - 4.6f) / 1.9f));
    } else if (t > 8.5f && t < 9.6f) {
        rear = 3.5f * ease((t - 8.5f) / 0.2f) * (1.0f - ease((t - 8.8f) / 0.8f));
    }
    const float front = rear * 0.15f;
    const float bend = lat / 4.4f * 0.006f; // half the track over the bend's radius
    const float ground = speed > 0.5f ? speed : 0.0f;
    store->set(kWheelFL, ground * (1.0f + front / 100.0f - bend), kDemo, now_ms);
    store->set(kWheelFR, ground * (1.0f + front / 100.0f + bend), kDemo, now_ms);
    store->set(kWheelRL, ground * (1.0f + rear / 100.0f - bend * 0.9f), kDemo, now_ms);
    store->set(kWheelRR, ground * (1.0f + rear / 100.0f + bend * 0.9f), kDemo, now_ms);
    derive_slip(store, kDemo, now_ms);
    store->set(kSteering, steer, kDemo, now_ms);
    store->set(kAccelLat, lat, kDemo, now_ms);
    store->set(kAccelLong, along, kDemo, now_ms);
    store->set(kFrontShare, share, kDemo, now_ms);
    store->set(kXdriveClutch, torque * 3.2f * share / 100.0f, kDemo, now_ms);
    store->set(kTyreFL, 240.0f + 0.4f * t, kDemo, now_ms);
    store->set(kTyreFR, 241.0f + 0.4f * t, kDemo, now_ms);
    store->set(kTyreRL, 262.0f + 0.5f * t, kDemo, now_ms);
    store->set(kTyreRR, 261.0f + 0.5f * t, kDemo, now_ms);
    // In Sport+: first at the start, second then third through the pull,
    // fifth for the cruise.
    store->set(kGear, t < 4.0f ? 1 : t < 8.5f ? 2 : t < 15.0f ? 3 : 5, kDemo, now_ms);
    store->set(kDriveMode, 5, kDemo, now_ms);
    store->set(kGearboxTemp, 84.0f + 6.0f * load, kDemo, now_ms);
    if (diesel) {
        // A filter near its regeneration: it starts in the cruise.
        const bool regen = t >= 15.0f;
        store->set(kFuelType, 4, kDemo, now_ms);
        store->set(kDpfLoad, regen ? 96.0f - 3.0f * (t - 15.0f) : 88.0f + 0.7f * t, kDemo, now_ms);
        store->set(kDpfRegen, regen ? kRegenActive : kRegenOff, kDemo, now_ms);
        store->set(kDpfDelta, 1.2f + 0.0035f * (4.0f + 0.045f * rpm * load) * 10.0f, kDemo, now_ms);
        const float in = regen ? 560.0f + 60.0f * ease((t - 15.0f) / 3.0f) : 230.0f + 320.0f * load;
        store->set(kDpfInTemp, in, kDemo, now_ms);
        store->set(kDpfOutTemp, regen ? in + 45.0f : in - 15.0f, kDemo, now_ms);
        store->set(kDpfAvgDistance, 412, kDemo, now_ms);
        store->set(kDpfAvgTime, 395, kDemo, now_ms);
        store->set(kDpfSince, 398.0f + t * 0.03f, kDemo, now_ms);
        store->set(kDpfSoot, regen ? 27.5f - 0.8f * (t - 15.0f) : 26.0f + 0.06f * t, kDemo, now_ms);
        store->set(kDpfAsh, 41.0f, kDemo, now_ms);
    }
}

} // namespace gauges
