// SPDX-License-Identifier: MPL-2.0
#include "units.hpp"

#include <math.h>
#include <stdio.h>

namespace gauges {
namespace {

const float kKpaPerPsi = 6.894757f;
const float kAfrPetrol = 14.7f;

} // namespace

float to_display(int channel, float value, const Units &units) {
    switch (channel_info(channel).quantity) {
    case kQPressure:
    case kQHighPressure:
        return units.imperial ? value / kKpaPerPsi : value / 100.0f;
    case kQTemperature:
        return units.imperial ? value * 9.0f / 5.0f + 32.0f : value;
    case kQLambda:
        return units.afr ? value * kAfrPetrol : value;
    case kQSpeed:
        return units.imperial ? value / 1.609344f : value;
    case kQTorque:
        return units.imperial ? value * 0.737562f : value;
    case kQAccel:
        return value / 9.80665f;
    case kQSmallPressure:
        return units.imperial ? value / kKpaPerPsi : value * 10.0f;
    case kQDistance:
        return units.imperial ? value / 1.609344f : value;
    default:
        return value;
    }
}

const char *unit_text(int channel, const Units &units) {
    switch (channel_info(channel).quantity) {
    case kQPressure:
    case kQHighPressure:
        return units.imperial ? "psi" : "bar";
    case kQTemperature:
        return units.imperial ? "\xc2\xb0" "F" : "\xc2\xb0" "C";
    case kQLambda:
        return units.afr ? "AFR" : "";
    case kQAngle:
        return "\xc2\xb0";
    case kQSpeed:
        return units.imperial ? "mph" : "km/h";
    case kQRpm:
        return "rpm";
    case kQPercent:
        return "%";
    case kQFlow:
        return "g/s";
    case kQVoltage:
        return "V";
    case kQTorque:
        return units.imperial ? "lb-ft" : "Nm";
    case kQAccel:
        return "g";
    case kQSmallPressure:
        return units.imperial ? "psi" : "mbar";
    case kQDistance:
        return units.imperial ? "mi" : "km";
    case kQMinutes:
        return "min";
    case kQMass:
        return "g";
    case kQState:
    case kQCount:
        return "";
    case kQRaw:
        return custom_unit(channel);
    }
    return "";
}

int decimals(int channel, const Units &units) {
    switch (channel_info(channel).quantity) {
    case kQPressure:
        return units.imperial ? 1 : 2;
    case kQLambda:
        return units.afr ? 1 : 2;
    case kQAngle:
    case kQVoltage:
    case kQMass:
        return 1;
    case kQAccel:
        return 2;
    case kQSmallPressure:
        return units.imperial ? 2 : 0;
    case kQRaw: {
        const ChannelInfo &info = channel_info(channel);
        const float span = fabsf(info.max - info.min);
        return span <= 10.0f ? 2 : span <= 100.0f ? 1 : 0;
    }
    default:
        return 0;
    }
}

void format_value(char *out, size_t size, int channel, float value, bool valid,
                  const Units &units) {
    if (!valid) {
        snprintf(out, size, "--");
        return;
    }
    if (channel_info(channel).quantity == kQState) {
        snprintf(out, size, "%s", state_text(channel, value));
        return;
    }
    const float shown = to_display(channel, value, units);
    const int places = decimals(channel, units);
    // No "-0.0": a value that rounds to zero is shown as zero.
    const float scale = places == 0 ? 1.0f : places == 1 ? 10.0f : 100.0f;
    const float rounded = roundf(shown * scale) / scale;
    snprintf(out, size, "%.*f", places, rounded == 0.0f ? 0.0 : static_cast<double>(rounded));
}

} // namespace gauges
