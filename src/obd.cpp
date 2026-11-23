// SPDX-License-Identifier: MPL-2.0
#include "obd.hpp"

#include <string.h>

#include "channels.hpp"

namespace gauges {
namespace {

// The gauges' PIDs. Order matters within a channel: obd_plan takes the
// first supported one.
const ObdPid kPids[] = {
    {0x87, 5, "intake manifold absolute pressure (wide)"},
    {0x70, 10, "boost pressure control"},
    {0x0B, 1, "intake manifold absolute pressure"},
    {0x33, 1, "barometric pressure"},
    {0x34, 4, "O2 sensor 1, equivalence ratio and current"},
    {0x24, 4, "O2 sensor 1, equivalence ratio and voltage"},
    {0x44, 2, "commanded equivalence ratio"},
    {0x0E, 1, "timing advance"},
    {0x23, 2, "fuel rail gauge pressure"},
    {0x59, 2, "fuel rail absolute pressure"},
    {0x0A, 1, "fuel pressure"},
    {0x0C, 2, "engine speed"},
    {0x0D, 1, "vehicle speed"},
    {0x05, 1, "coolant temperature"},
    {0x5C, 1, "oil temperature"},
    {0x0F, 1, "intake air temperature"},
    {0x77, 5, "charge air cooler temperature"},
    {0x46, 1, "ambient air temperature"},
    {0x3C, 2, "catalyst temperature bank 1 sensor 1"},
    {0x11, 1, "throttle position"},
    {0x49, 1, "accelerator pedal position D"},
    {0x04, 1, "calculated engine load"},
    {0x10, 2, "mass air flow"},
    {0x06, 1, "short term fuel trim bank 1"},
    {0x07, 1, "long term fuel trim bank 1"},
    {0x42, 2, "control module voltage"},
    {0x62, 1, "actual engine torque"},
    {0x63, 2, "engine reference torque"},
    {0x51, 1, "fuel type"},
    {0x8B, 7, "diesel aftertreatment"},
    {0x7A, 7, "particulate filter bank 1 pressures"},
    {0x7C, 9, "particulate filter temperatures"},
};
const size_t kPidCount = sizeof(kPids) / sizeof(kPids[0]);

// What each PID gives, for obd_plan: a PID is skipped when an earlier one
// already gives its (first) channel.
int primary_channel(uint8_t pid) {
    switch (pid) {
    case 0x87:
    case 0x70:
    case 0x0B:
        return kMap;
    case 0x34:
    case 0x24:
        return kLambda;
    case 0x23:
    case 0x59:
        return kRail;
    default:
        return -1;
    }
}

unsigned u16(const uint8_t *d) {
    return static_cast<unsigned>(d[0]) << 8 | d[1];
}

int s16(const uint8_t *d) {
    const unsigned v = u16(d);
    return v >= 0x8000u ? static_cast<int>(v) - 0x10000 : static_cast<int>(v);
}

} // namespace

size_t obd_pid_count() {
    return kPidCount;
}

const ObdPid &obd_pid(size_t i) {
    return kPids[i < kPidCount ? i : 0];
}

const ObdPid *obd_find(uint8_t pid) {
    for (size_t i = 0; i < kPidCount; ++i) {
        if (kPids[i].pid == pid) {
            return &kPids[i];
        }
    }
    return NULL;
}

int obd_decode(uint8_t pid, const uint8_t *d, size_t size, ObdValue *out, int max) {
    const ObdPid *info = obd_find(pid);
    if (!info || size < info->size || max < 1) {
        return 0;
    }
    int n = 0;
#define PUT(ch, v)                                                                                 \
    do {                                                                                           \
        if (n < max) {                                                                             \
            out[n].channel = (ch);                                                                 \
            out[n].value = static_cast<float>(v);                                                  \
            ++n;                                                                                   \
        }                                                                                          \
    } while (0)
    const float a = d[0];
    switch (pid) {
    case 0x04:
        PUT(kLoad, a * 100.0f / 255.0f);
        break;
    case 0x05:
        PUT(kCoolant, a - 40.0f);
        break;
    case 0x06:
        PUT(kTrimShort, (a - 128.0f) * 100.0f / 128.0f);
        break;
    case 0x07:
        PUT(kTrimLong, (a - 128.0f) * 100.0f / 128.0f);
        break;
    case 0x0A:
        PUT(kFuelLow, a * 3.0f);
        break;
    case 0x0B:
        PUT(kMap, a);
        break;
    case 0x0C:
        PUT(kRpm, u16(d) / 4.0f);
        break;
    case 0x0D:
        PUT(kSpeed, a);
        break;
    case 0x0E:
        PUT(kTiming, a / 2.0f - 64.0f);
        break;
    case 0x0F:
        PUT(kIntakeAir, a - 40.0f);
        break;
    case 0x10:
        PUT(kMaf, u16(d) / 100.0f);
        break;
    case 0x11:
        PUT(kThrottle, a * 100.0f / 255.0f);
        break;
    case 0x23:
        PUT(kRail, u16(d) * 10.0f);
        break;
    case 0x24:
        PUT(kLambda, u16(d) * 2.0f / 65536.0f);
        break;
    case 0x33:
        PUT(kBaro, a);
        break;
    case 0x34:
        PUT(kLambda, u16(d) / 32768.0f);
        break;
    case 0x3C:
        PUT(kCatalyst, u16(d) / 10.0f - 40.0f);
        break;
    case 0x42:
        PUT(kVoltage, u16(d) / 1000.0f);
        break;
    case 0x44:
        PUT(kLambdaTarget, u16(d) * 2.0f / 65536.0f);
        break;
    case 0x46:
        PUT(kAmbient, a - 40.0f);
        break;
    case 0x49:
        PUT(kPedal, a * 100.0f / 255.0f);
        break;
    case 0x59:
        PUT(kRail, u16(d) * 10.0f);
        break;
    case 0x5C:
        PUT(kOil, a - 40.0f);
        break;
    case 0x62:
        // Percent of the reference torque (0x63): the poller multiplies.
        PUT(kTorque, a - 125.0f);
        break;
    case 0x63:
        break; // kept by the poller (obd_reference_torque), not a channel
    case 0x70:
        // A: which of the four values are there; B-C commanded A, D-E actual
        // A, in 1/32 kPa (absolute).
        if (d[0] & 0x01) {
            PUT(kBoostTarget, u16(d + 1) / 32.0f);
        }
        if (d[0] & 0x02) {
            PUT(kMap, u16(d + 3) / 32.0f);
        }
        break;
    case 0x77:
        if (d[0] & 0x01) {
            PUT(kChargeAir, d[1] - 40.0f);
        }
        break;
    case 0x87:
        if (d[0] & 0x01) {
            PUT(kMap, u16(d + 1) / 32.0f);
        }
        break;
    case 0x51:
        PUT(kFuelType, a);
        break;
    case 0x7A:
        // A: which are there (bit 0 the pressure drop); B-C the drop, signed,
        // 1/100 kPa; D-E inlet, F-G outlet pressure.
        if (d[0] & 0x01) {
            PUT(kDpfDelta, s16(d + 1) / 100.0f);
        }
        break;
    case 0x7C:
        // A: which are there (bit 0 bank 1 inlet, bit 1 bank 1 outlet); then
        // temperatures in 1/10 degree, offset -40.
        if (d[0] & 0x01) {
            PUT(kDpfInTemp, u16(d + 1) / 10.0f - 40.0f);
        }
        if (d[0] & 0x02) {
            PUT(kDpfOutTemp, u16(d + 3) / 10.0f - 40.0f);
        }
        break;
    case 0x8B:
        // A: which are there; B bit 0 regenerating, bit 1 active (else
        // passive); C the trigger reached (/255); D-E average minutes, F-G
        // average km between regenerations.
        if (d[0] & 0x01) {
            const bool on = d[1] & 0x01;
            PUT(kDpfRegen, !on               ? kRegenOff
                           : !(d[0] & 0x02)  ? kRegenOn
                           : (d[1] & 0x02) ? kRegenActive
                                             : kRegenPassive);
        }
        if (d[0] & 0x10) {
            PUT(kDpfLoad, d[2] * 100.0f / 255.0f);
        }
        if (d[0] & 0x20) {
            PUT(kDpfAvgTime, u16(d + 3));
        }
        if (d[0] & 0x40) {
            PUT(kDpfAvgDistance, u16(d + 5));
        }
        break;
    default:
        break;
    }
#undef PUT
    return n;
}

void obd_support_set(uint8_t *supported, uint8_t base, const uint8_t *bits) {
    for (int i = 0; i < 32; ++i) {
        // Bit 7 of the first byte is PID base+1.
        if (bits[i / 8] & (0x80 >> (i % 8))) {
            const unsigned pid = base + 1u + static_cast<unsigned>(i);
            if (pid < 256) {
                supported[pid / 8] |= static_cast<uint8_t>(1u << (pid % 8));
            }
        }
    }
}

bool obd_supported(const uint8_t *supported, uint8_t pid) {
    return (supported[pid / 8] >> (pid % 8)) & 1u;
}

int obd_plan(const uint8_t *supported, uint8_t *pids, int max) {
    bool taken[kChannelCount];
    memset(taken, 0, sizeof(taken));
    int n = 0;
    for (size_t i = 0; i < kPidCount && n < max; ++i) {
        const uint8_t pid = kPids[i].pid;
        if (!obd_supported(supported, pid)) {
            continue;
        }
        const int channel = primary_channel(pid);
        if (channel >= 0) {
            if (taken[channel]) {
                continue;
            }
            taken[channel] = true;
        }
        pids[n++] = pid;
    }
    return n;
}

} // namespace gauges
