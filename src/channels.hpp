// SPDX-License-Identifier: MPL-2.0
// What the gauges show: the channels, their quantities and ranges in
// canonical units (kPa, degrees Celsius, lambda, degrees, km/h, rpm, %, g/s,
// V, Nm, m/s2, km, minutes, grams), and the store the data sources write and
// the screen reads. Display units are the screen's business (units.hpp).
#ifndef GAUGES_CHANNELS_HPP
#define GAUGES_CHANNELS_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace gauges {

enum Quantity {
    kQRaw,          // a custom channel: shown as read, with its own unit text
    kQPressure,     // kPa, shown in bar or psi with two/one decimals
    kQHighPressure, // kPa, shown in bar or psi without decimals (rail)
    kQTemperature,  // degrees Celsius
    kQLambda,       // equivalence ratio, shown as lambda or AFR
    kQAngle,        // degrees
    kQSpeed,        // km/h
    kQRpm,
    kQPercent,
    kQFlow,    // g/s
    kQVoltage, // V
    kQTorque,  // Nm
    kQAccel,         // m/s2, shown in g
    kQSmallPressure, // kPa, shown in mbar or psi (a filter's pressure drop)
    kQDistance,      // km
    kQMinutes,
    kQMass,  // g
    kQState, // a number standing for a state (state_text)
    kQCount, // a plain number
};

enum Channel {
    kRpm,
    kSpeed,
    kBoost,       // relative to the outside air: map - baro
    kBoostTarget, // relative
    kMap,         // absolute
    kBaro,
    kLambda,
    kLambdaTarget,
    kTiming,
    kRail,
    kFuelLow,
    kOilPressure,
    kCoolant,
    kOil,
    kIntakeAir,
    kChargeAir,
    kAmbient,
    kCatalyst,
    kThrottle,
    kPedal,
    kLoad,
    kMaf,
    kTrimShort,
    kTrimLong,
    kVoltage,
    kTorque,
    // xDrive and the wheels. Wheel speeds and the transfer case from
    // definition files (DSC, LMV); slip derived from the wheel speeds
    // (derive_slip); steering and acceleration also from iDrive's car data.
    kWheelFL,
    kWheelFR,
    kWheelRL,
    kWheelRR,
    kSlipFL, // % against the vehicle's speed over ground (derive_slip)
    kSlipFR,
    kSlipRL,
    kSlipRR,
    kSteering,    // steering wheel, degrees, left positive
    kAccelLat,    // m/s2, left positive
    kAccelLong,   // m/s2, forward positive
    kFrontShare,  // % of the drive torque to the front axle
    kXdriveClutch, // Nm the transfer case's clutch passes to the front
    kTyreFL,      // tyre pressures, kPa
    kTyreFR,
    kTyreRL,
    kTyreRR,
    kGear,        // 0 N, -1 R, -2 P, 1 and up the gear engaged
    kDriveMode,   // iDrive's driving experience control (state_text)
    kGearboxTemp, // the gearbox's oil, degrees Celsius
    // Diesel particulate filter: SAE J1979 PIDs 0x7A, 0x7C, 0x8B, or
    // definition files; the fuel type (PID 0x51) tells a diesel.
    kFuelType,
    kDpfLoad,     // % of the load that triggers a regeneration
    kDpfRegen,    // kRegenOff, kRegenOn, kRegenPassive, kRegenActive
    kDpfDelta,    // pressure drop across the filter, kPa
    kDpfInTemp,
    kDpfOutTemp,
    kDpfAvgDistance, // between regenerations, km
    kDpfAvgTime,     // between regenerations, minutes
    kDpfSince,       // km since the last regeneration
    kDpfSoot,        // g
    kDpfAsh,         // g
    kCorr1, // per-cylinder ignition corrections (degrees, negative = retard)
    kCorr8 = kCorr1 + 7,
    kCustom1, // channels a definition file adds
    kCustomLast = kCustom1 + 7,
    kChannelCount
};

enum { kCylinders = 8, kCustomChannels = kCustomLast - kCustom1 + 1 };

/* kDpfRegen's values. */
enum { kRegenOff = 0, kRegenOn = 1, kRegenPassive = 2, kRegenActive = 3 };
/* A state channel's text ("OFF", "ACTIVE", ...). */
const char *state_text(int channel, float value);
/* The gear as shown: N, R, P, or the gear with D (S in a sport mode); `mode`
 * kDriveMode's value, or negative when unknown (the gear alone). */
void gear_text(char *out, size_t size, float gear, float mode);
/* iDrive's car data codes (driving.gear: 1 N, 2 R, 3 P, 5 and up the first
 * gear and up) to kGear's values; false for none. */
bool car_data_gear(int code, float *gear);
/* Whether a channel is computed from others (slip): not in definition files. */
bool derived_channel(int channel);
/* Whether PID 0x51's fuel type is a diesel (4, 19, 23). */
bool diesel_fuel(int fuel_type);

struct ChannelInfo {
    const char *key;   // the definition files' id
    const char *label; // upper case, short
    Quantity quantity;
    float min, max;   // the gauge's range
    float warn;       // above this the value is shown in amber (NaN: never)
    float danger;     // above this in red (NaN: never)
    float origin;     // where a gauge's arc starts (0, or 1 for lambda)
    bool fast;        // polled every cycle (else round robin)
};

/* The built-in table; custom channels get their label from set_custom(). */
const ChannelInfo &channel_info(int channel);
/* The channel whose key is `key` (built-in or custom), or -1. */
int channel_by_key(const char *key);
/* A custom channel's label and unit text (from a definition file). */
void set_custom(int channel, const char *key, const char *label, const char *unit, float min,
                float max);
const char *custom_unit(int channel);
bool custom_used(int channel);
void clear_custom();

/* Where a value came from, in rising priority: a fresher value from a
 * higher source wins; a lower one fills in when the higher is stale. */
enum Source { kNoSource = 0, kDemo = 1, kCarData = 2, kDme = 3 };

struct Sample {
    float value;
    float min, max; // since the last reset_peaks()
    uint64_t at_ms; // when it was set
    uint8_t source;
    bool valid;
};

class Store {
public:
    enum { kStaleMs = 2000 };
    Store();
    ~Store();
    /* Sets a channel; ignored when a higher source set it less than
     * kStaleMs ago. */
    void set(int channel, float value, Source source, uint64_t now_ms);
    /* One channel. */
    Sample get(int channel) const;
    /* A copy of every channel, consistent. */
    void snapshot(Sample *out) const;
    void reset_peaks();
    /* Forgets every value of `source` (a source that went away). */
    void drop(Source source);

private:
    Store(const Store &);
    Store &operator=(const Store &);
    mutable pthread_mutex_t mutex_;
    Sample samples_[kChannelCount];
};

/* Whether a sample is current: valid and set less than `max_age_ms` ago. */
bool fresh(const Sample &sample, uint64_t now_ms, uint64_t max_age_ms = Store::kStaleMs);

/* Each wheel's slip from the four wheel speeds, against the other wheel on
 * its side: the slower one (a spinning wheel shows +), under braking (kAccelLong
 * below -1.5 m/s2) the faster one (a locking wheel shows -). Comparing a side
 * with itself leaves out a bend's outer wheels running faster. 0 below 5 km/h.
 * Sets kSlipFL..kSlipRR as `source` when the four speeds are current; returns
 * whether it did. */
bool derive_slip(Store *store, Source source, uint64_t now_ms);
/* The same on four speeds (FL, FR, RL, RR): `slip` in %. */
void slip_of(const float *speeds, bool braking, float *slip);

} // namespace gauges

#endif
