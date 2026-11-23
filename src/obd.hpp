// SPDX-License-Identifier: MPL-2.0
// The standard OBD-II values an engine computer gives over UDS: data
// identifiers 0xF400-0xF4FF carry the SAE J1979 parameters (0xF4 then the
// PID), with J1979's formulas. Which ones a DME has comes from the support
// bitmaps 0xF400, 0xF420, 0xF440, ... (each says which of the next 32 exist).
#ifndef GAUGES_OBD_HPP
#define GAUGES_OBD_HPP

#include <stddef.h>
#include <stdint.h>

namespace gauges {

struct ObdValue {
    int channel;
    float value;
};

struct ObdPid {
    uint8_t pid;
    uint8_t size; // data bytes after the DID
    const char *name;
};

/* The PIDs the gauges read, best first where several give one channel. */
size_t obd_pid_count();
const ObdPid &obd_pid(size_t i);
const ObdPid *obd_find(uint8_t pid);

enum { kObdMaxValues = 4 }; // the most one PID gives (0x8B)

/* Decodes PID `pid`'s data (`size` bytes); fills up to `max` values and
 * returns how many. Absolute pressures are kPa; boost targets are given
 * absolute (kBoostTarget then holds an absolute value: the caller subtracts
 * the barometric pressure, see derive_boost). */
int obd_decode(uint8_t pid, const uint8_t *data, size_t size, ObdValue *out, int max);

/* Support bitmaps: `base` is 0x00, 0x20, ...; `bits` the 4 data bytes. */
void obd_support_set(uint8_t *supported /* 256 bits */, uint8_t base, const uint8_t *bits);
bool obd_supported(const uint8_t *supported, uint8_t pid);

/* Which PIDs to read for the supported set: one per channel, best first
 * (manifold pressure from 0x87, then 0x70, then 0x0B; lambda from 0x34, then
 * 0x24; rail from 0x23, then 0x59). Returns how many were written. */
int obd_plan(const uint8_t *supported, uint8_t *pids, int max);

} // namespace gauges

#endif
