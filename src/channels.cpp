// SPDX-License-Identifier: MPL-2.0
#include "channels.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace gauges {
namespace {

#define NONE NAN
// key, label, quantity, min, max, warn, danger, origin, fast
const ChannelInfo kBuiltIn[kCorr1] = {
    {"rpm", "RPM", kQRpm, 0, 7500, 6500, 7000, 0, true},
    {"speed", "SPEED", kQSpeed, 0, 260, NONE, NONE, 0, false},
    {"boost", "BOOST", kQPressure, -100, 200, NONE, NONE, 0, true},
    {"boost_target", "BOOST TARGET", kQPressure, -100, 200, NONE, NONE, 0, true},
    {"map", "MANIFOLD", kQPressure, 0, 300, NONE, NONE, 0, true},
    {"baro", "BAROMETRIC", kQPressure, 60, 110, NONE, NONE, 0, false},
    {"lambda", "LAMBDA", kQLambda, 0.7f, 1.3f, NONE, NONE, 1, true},
    {"lambda_target", "LAMBDA TARGET", kQLambda, 0.7f, 1.3f, NONE, NONE, 1, true},
    {"timing", "TIMING", kQAngle, -10, 40, NONE, NONE, 0, true},
    {"rail", "RAIL PRESSURE", kQHighPressure, 0, 25000, NONE, NONE, 0, true},
    {"fuel_low", "FUEL SUPPLY", kQPressure, 0, 800, NONE, NONE, 0, false},
    {"oil_pressure", "OIL PRESSURE", kQPressure, 0, 800, NONE, NONE, 0, false},
    {"coolant", "COOLANT", kQTemperature, 40, 130, 115, 122, 40, false},
    {"oil", "OIL", kQTemperature, 40, 160, 130, 140, 40, false},
    {"intake_air", "INTAKE AIR", kQTemperature, -20, 80, 50, 65, -20, false},
    {"charge_air", "CHARGE AIR", kQTemperature, -20, 100, 60, 75, -20, false},
    {"ambient", "AMBIENT", kQTemperature, -30, 50, NONE, NONE, -30, false},
    {"catalyst", "CATALYST", kQTemperature, 0, 1000, 900, 950, 0, false},
    {"throttle", "THROTTLE", kQPercent, 0, 100, NONE, NONE, 0, true},
    {"pedal", "PEDAL", kQPercent, 0, 100, NONE, NONE, 0, true},
    {"load", "LOAD", kQPercent, 0, 200, NONE, NONE, 0, false},
    {"maf", "AIR MASS", kQFlow, 0, 300, NONE, NONE, 0, false},
    {"trim_short", "SHORT TRIM", kQPercent, -25, 25, NONE, NONE, 0, false},
    {"trim_long", "LONG TRIM", kQPercent, -25, 25, NONE, NONE, 0, false},
    {"voltage", "BATTERY", kQVoltage, 10, 16, NONE, NONE, 10, false},
    {"torque", "TORQUE", kQTorque, 0, 600, NONE, NONE, 0, false},
    {"wheel_fl", "FRONT LEFT", kQSpeed, 0, 260, NONE, NONE, 0, true},
    {"wheel_fr", "FRONT RIGHT", kQSpeed, 0, 260, NONE, NONE, 0, true},
    {"wheel_rl", "REAR LEFT", kQSpeed, 0, 260, NONE, NONE, 0, true},
    {"wheel_rr", "REAR RIGHT", kQSpeed, 0, 260, NONE, NONE, 0, true},
    {"slip_fl", "SLIP FL", kQPercent, -20, 20, 4, 10, 0, false},
    {"slip_fr", "SLIP FR", kQPercent, -20, 20, 4, 10, 0, false},
    {"slip_rl", "SLIP RL", kQPercent, -20, 20, 4, 10, 0, false},
    {"slip_rr", "SLIP RR", kQPercent, -20, 20, 4, 10, 0, false},
    {"steering", "STEERING", kQAngle, -540, 540, NONE, NONE, 0, true},
    {"accel_lat", "LATERAL", kQAccel, -15, 15, NONE, NONE, 0, true},
    {"accel_long", "LONGITUDINAL", kQAccel, -15, 15, NONE, NONE, 0, true},
    {"front_share", "FRONT AXLE", kQPercent, 0, 100, NONE, NONE, 0, true},
    {"xdrive_clutch", "XDRIVE CLUTCH", kQTorque, 0, 1500, NONE, NONE, 0, true},
    {"tyre_fl", "TYRE FL", kQPressure, 0, 400, NONE, NONE, 0, false},
    {"tyre_fr", "TYRE FR", kQPressure, 0, 400, NONE, NONE, 0, false},
    {"tyre_rl", "TYRE RL", kQPressure, 0, 400, NONE, NONE, 0, false},
    {"tyre_rr", "TYRE RR", kQPressure, 0, 400, NONE, NONE, 0, false},
    {"gear", "GEAR", kQCount, -2, 12, NONE, NONE, 0, true},
    {"drive_mode", "DRIVE MODE", kQState, 0, 15, NONE, NONE, 0, false},
    {"gearbox_temp", "GEARBOX OIL", kQTemperature, 20, 150, 120, 135, 20, false},
    {"fuel_type", "FUEL TYPE", kQCount, 0, 30, NONE, NONE, 0, false},
    {"dpf_load", "SOOT LOAD", kQPercent, 0, 120, 85, 100, 0, false},
    {"dpf_regen", "REGENERATION", kQState, 0, 3, NONE, NONE, 0, false},
    {"dpf_delta", "PRESSURE DROP", kQSmallPressure, 0, 40, NONE, NONE, 0, false},
    {"dpf_in", "FILTER INLET", kQTemperature, 0, 800, 650, 720, 0, false},
    {"dpf_out", "FILTER OUTLET", kQTemperature, 0, 800, 700, 760, 0, false},
    {"dpf_avg_distance", "REGEN EVERY", kQDistance, 0, 1500, NONE, NONE, 0, false},
    {"dpf_avg_time", "REGEN EVERY", kQMinutes, 0, 1500, NONE, NONE, 0, false},
    {"dpf_since", "SINCE REGEN", kQDistance, 0, 1500, NONE, NONE, 0, false},
    {"dpf_soot", "SOOT MASS", kQMass, 0, 60, NONE, NONE, 0, false},
    {"dpf_ash", "ASH", kQMass, 0, 200, NONE, NONE, 0, false},
};
#undef NONE

ChannelInfo g_corr[kCylinders];
char g_corr_keys[kCylinders][16];
char g_corr_labels[kCylinders][16];

struct Custom {
    bool used;
    char key[24];
    char label[25];
    char unit[9];
    ChannelInfo info;
};
Custom g_custom[kCustomChannels];
const ChannelInfo kUnused = {"", "", kQRaw, 0, 1, NAN, NAN, 0, false};

void init_corr() {
    if (g_corr[0].key) {
        return;
    }
    for (int i = 0; i < kCylinders; ++i) {
        snprintf(g_corr_keys[i], sizeof(g_corr_keys[i]), "corr%d", i + 1);
        snprintf(g_corr_labels[i], sizeof(g_corr_labels[i]), "CYL %d", i + 1);
        ChannelInfo info = {g_corr_keys[i], g_corr_labels[i], kQAngle, -10, 2, NAN, NAN, 0, true};
        g_corr[i] = info;
    }
}

void copy(char *to, size_t size, const char *from) {
    snprintf(to, size, "%s", from ? from : "");
}

} // namespace

const ChannelInfo &channel_info(int channel) {
    if (channel >= 0 && channel < kCorr1) {
        return kBuiltIn[channel];
    }
    if (channel >= kCorr1 && channel <= kCorr8) {
        init_corr();
        return g_corr[channel - kCorr1];
    }
    if (channel >= kCustom1 && channel <= kCustomLast && g_custom[channel - kCustom1].used) {
        return g_custom[channel - kCustom1].info;
    }
    return kUnused;
}

int channel_by_key(const char *key) {
    if (!key || !*key) {
        return -1;
    }
    for (int c = 0; c < kChannelCount; ++c) {
        if (channel_info(c).key && !strcmp(channel_info(c).key, key)) {
            return c;
        }
    }
    return -1;
}

void set_custom(int channel, const char *key, const char *label, const char *unit, float min,
                float max) {
    if (channel < kCustom1 || channel > kCustomLast) {
        return;
    }
    Custom &c = g_custom[channel - kCustom1];
    c.used = true;
    copy(c.key, sizeof(c.key), key);
    copy(c.label, sizeof(c.label), label);
    copy(c.unit, sizeof(c.unit), unit);
    for (char *p = c.label; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = static_cast<char>(*p - 'a' + 'A');
        }
    }
    ChannelInfo info = {c.key, c.label, kQRaw, min, max, NAN, NAN, min, true};
    c.info = info;
}

const char *custom_unit(int channel) {
    if (channel < kCustom1 || channel > kCustomLast) {
        return "";
    }
    return g_custom[channel - kCustom1].unit;
}

bool custom_used(int channel) {
    return channel >= kCustom1 && channel <= kCustomLast && g_custom[channel - kCustom1].used;
}

void clear_custom() {
    memset(g_custom, 0, sizeof(g_custom));
}

Store::Store() {
    pthread_mutex_init(&mutex_, NULL);
    memset(samples_, 0, sizeof(samples_));
}

Store::~Store() {
    pthread_mutex_destroy(&mutex_);
}

void Store::set(int channel, float value, Source source, uint64_t now_ms) {
    if (channel < 0 || channel >= kChannelCount || isnan(value) || isinf(value)) {
        return;
    }
    pthread_mutex_lock(&mutex_);
    Sample &s = samples_[channel];
    const bool higher_is_current =
        s.valid && s.source > source && now_ms - s.at_ms < static_cast<uint64_t>(kStaleMs);
    if (!higher_is_current) {
        if (!s.valid) {
            s.min = s.max = value;
        } else {
            if (value < s.min) {
                s.min = value;
            }
            if (value > s.max) {
                s.max = value;
            }
        }
        s.value = value;
        s.at_ms = now_ms;
        s.source = static_cast<uint8_t>(source);
        s.valid = true;
    }
    pthread_mutex_unlock(&mutex_);
}

void Store::snapshot(Sample *out) const {
    pthread_mutex_lock(&mutex_);
    memcpy(out, samples_, sizeof(samples_));
    pthread_mutex_unlock(&mutex_);
}

void Store::reset_peaks() {
    pthread_mutex_lock(&mutex_);
    for (int c = 0; c < kChannelCount; ++c) {
        samples_[c].min = samples_[c].max = samples_[c].value;
    }
    pthread_mutex_unlock(&mutex_);
}

void Store::drop(Source source) {
    pthread_mutex_lock(&mutex_);
    for (int c = 0; c < kChannelCount; ++c) {
        if (samples_[c].source == source) {
            samples_[c].valid = false;
        }
    }
    pthread_mutex_unlock(&mutex_);
}

Sample Store::get(int channel) const {
    Sample s;
    memset(&s, 0, sizeof(s));
    if (channel >= 0 && channel < kChannelCount) {
        pthread_mutex_lock(&mutex_);
        s = samples_[channel];
        pthread_mutex_unlock(&mutex_);
    }
    return s;
}

bool fresh(const Sample &sample, uint64_t now_ms, uint64_t max_age_ms) {
    return sample.valid && now_ms - sample.at_ms < max_age_ms;
}

const char *state_text(int channel, float value) {
    if (channel == kDriveMode) {
        // iDrive's codes (BimmerGestalt's notes on its car data).
        switch (static_cast<int>(value + 0.5f)) {
        case 2:
        case 3:
            return "COMFORT";
        case 9:
            return "COMFORT+";
        case 4:
            return "SPORT";
        case 5:
            return "SPORT+";
        case 6:
            return "RACE";
        case 7:
            return "ECO PRO";
        case 8:
            return "ECO PRO+";
        default:
            return "";
        }
    }
    if (channel == kDpfRegen) {
        switch (static_cast<int>(value + 0.5f)) {
        case kRegenOn:
            return "ON";
        case kRegenPassive:
            return "PASSIVE";
        case kRegenActive:
            return "ACTIVE";
        default:
            return "OFF";
        }
    }
    return "";
}

void gear_text(char *out, size_t size, float gear, float mode) {
    const int n = static_cast<int>(roundf(gear));
    const int m = static_cast<int>(roundf(mode));
    if (n == 0 || n == -1 || n == -2) {
        snprintf(out, size, "%s", n == 0 ? "N" : n == -1 ? "R" : "P");
    } else if (mode < 0) {
        snprintf(out, size, "%d", n);
    } else {
        snprintf(out, size, "%c%d", m == 4 || m == 5 || m == 6 ? 'S' : 'D', n);
    }
}

bool car_data_gear(int code, float *gear) {
    if (code >= 1 && code <= 3) {
        *gear = static_cast<float>(1 - code); // N 0, R -1, P -2
        return true;
    }
    if (code >= 5 && code <= 16) {
        *gear = static_cast<float>(code - 4);
        return true;
    }
    return false;
}

bool derived_channel(int channel) {
    return channel >= kSlipFL && channel <= kSlipRR;
}

bool diesel_fuel(int fuel_type) {
    return fuel_type == 4 || fuel_type == 19 || fuel_type == 23;
}

void slip_of(const float *speeds, bool braking, float *slip) {
    for (int i = 0; i < 4; ++i) {
        const float other = speeds[i < 2 ? i + 2 : i - 2]; // the same side's other axle
        const float ref = braking ? fmaxf(speeds[i], other) : fminf(speeds[i], other);
        slip[i] = ref >= 5.0f ? (speeds[i] - ref) / ref * 100.0f : 0.0f;
    }
}

bool derive_slip(Store *store, Source source, uint64_t now_ms) {
    float speeds[4], slip[4];
    for (int i = 0; i < 4; ++i) {
        const Sample s = store->get(kWheelFL + i);
        if (!fresh(s, now_ms)) {
            return false;
        }
        speeds[i] = s.value;
    }
    const Sample along = store->get(kAccelLong);
    slip_of(speeds, fresh(along, now_ms) && along.value < -1.5f, slip);
    for (int i = 0; i < 4; ++i) {
        store->set(kSlipFL + i, slip[i], source, now_ms);
    }
    return true;
}

} // namespace gauges
