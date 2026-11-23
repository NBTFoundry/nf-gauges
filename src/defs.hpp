// SPDX-License-Identifier: MPL-2.0
// Definition files: values a DME gives beyond the standard ones (per-cylinder
// ignition corrections, boost target, oil pressure...), described by the
// user in a CSV file sent to the application's files folder. Each line reads
// one value from an ECU's data identifier:
//
//   id,label,unit,ecu,did,pos,type,mul,div,add,min,max
//   corr1,,,12,XXXX,1,s8,0.375,1,0,,
//
// value = raw * mul / div + add, raw being the big-endian integer of `type`
// (u8 s8 u16 s16 u32 s32) at byte `pos` (1 = first) of the identifier's
// record: the scaling of the DME's SGBD table SG_FUNKTIONEN (MUL, DIV, ADD).
// A built-in id (channels.cpp: boost, map, lambda, timing, corr1-corr8,
// oil_pressure, ...) replaces that channel's standard source and must give its
// canonical unit (kPa, degrees Celsius, lambda, degrees); label, unit, min
// and max are then optional. Any other id adds a custom channel (at most 8),
// shown as read with its own label and unit. No BMW data is shipped: the
// identifiers and scalings are the user's (README, "Definition files").
#ifndef GAUGES_DEFS_HPP
#define GAUGES_DEFS_HPP

#include <stddef.h>
#include <stdint.h>

namespace gauges {

struct Definition {
    int channel;
    uint8_t ecu;
    uint16_t did;
    uint8_t pos;  // 1-based
    uint8_t size; // 1, 2 or 4 bytes
    bool is_signed;
    float mul, div, add;
};

struct DefinitionSet {
    enum { kMax = 32, kMaxErrors = 4 };
    Definition defs[kMax];
    int count;
    char errors[kMaxErrors][96];
    int error_count;
    int lines_ignored;
};

void definitions_clear(DefinitionSet *set);
/* Adds the lines of one file (`name` for the error messages); custom
 * channels are registered as they come (channels.hpp set_custom). Returns
 * how many definitions it added. */
int definitions_parse(DefinitionSet *set, const char *name, const char *text, size_t size);
/* The value of `def` in a record (the bytes after the data identifier);
 * false when the record is too short. */
bool definition_value(const Definition &def, const uint8_t *record, size_t size, float *value);

} // namespace gauges

#endif
