// SPDX-License-Identifier: MPL-2.0
#include "defs.hpp"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channels.hpp"

namespace gauges {
namespace {

enum { kFields = 12, kFieldSize = 32 };

void add_error(DefinitionSet *set, const char *name, int line, const char *what) {
    if (set->error_count < DefinitionSet::kMaxErrors) {
        snprintf(set->errors[set->error_count], sizeof(set->errors[0]), "%s line %d: %s", name,
                 line, what);
    }
    ++set->error_count;
}

// Splits one line at commas into trimmed fields; false when a field is too
// long or there are too many.
bool split(const char *line, size_t len, char fields[kFields][kFieldSize], int *count) {
    *count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && line[i] != ',') {
            continue;
        }
        if (*count == kFields) {
            return false;
        }
        size_t a = start, b = i;
        while (a < b && isspace(static_cast<unsigned char>(line[a]))) {
            ++a;
        }
        while (b > a && isspace(static_cast<unsigned char>(line[b - 1]))) {
            --b;
        }
        if (b - a >= kFieldSize) {
            return false;
        }
        memcpy(fields[*count], line + a, b - a);
        fields[*count][b - a] = '\0';
        ++*count;
        start = i + 1;
    }
    return true;
}

bool parse_hex(const char *text, unsigned max, unsigned *out) {
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (!*text) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(text, &end, 16);
    if (*end || v > max) {
        return false;
    }
    *out = static_cast<unsigned>(v);
    return true;
}

bool parse_float(const char *text, float fallback, float *out) {
    if (!*text) {
        *out = fallback;
        return true;
    }
    char *end = NULL;
    const double v = strtod(text, &end);
    if (*end || isnan(v) || isinf(v)) {
        return false;
    }
    *out = static_cast<float>(v);
    return true;
}

bool valid_key(const char *key) {
    if (!*key || strlen(key) > 23) {
        return false;
    }
    for (const char *p = key; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

bool printable(const char *text) {
    for (const char *p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

int next_custom() {
    for (int c = kCustom1; c <= kCustomLast; ++c) {
        if (!custom_used(c)) {
            return c;
        }
    }
    return -1;
}

} // namespace

void definitions_clear(DefinitionSet *set) {
    memset(set, 0, sizeof(*set));
    clear_custom();
}

int definitions_parse(DefinitionSet *set, const char *name, const char *text, size_t size) {
    int added = 0, line_no = 0;
    size_t pos = 0;
    while (pos < size) {
        size_t end = pos;
        while (end < size && text[end] != '\n') {
            ++end;
        }
        const char *line = text + pos;
        size_t len = end - pos;
        pos = end + 1;
        ++line_no;
        if (len && line[len - 1] == '\r') {
            --len;
        }
        // Blank lines, comments, a header line and a UTF-8 byte order mark.
        if (line_no == 1 && len >= 3 && !memcmp(line, "\xEF\xBB\xBF", 3)) {
            line += 3;
            len -= 3;
        }
        size_t first = 0;
        while (first < len && isspace(static_cast<unsigned char>(line[first]))) {
            ++first;
        }
        if (first == len || line[first] == '#' || (len - first >= 3 && !memcmp(line + first, "id,", 3))) {
            continue;
        }
        char f[kFields][kFieldSize];
        int count = 0;
        if (!split(line, len, f, &count) || count < 8) {
            add_error(set, name, line_no, "expected id,label,unit,ecu,did,pos,type,mul[,div,add,min,max]");
            continue;
        }
        for (int i = count; i < kFields; ++i) {
            f[i][0] = '\0';
        }
        if (set->count == DefinitionSet::kMax) {
            add_error(set, name, line_no, "too many definitions (32)");
            break;
        }
        Definition d;
        memset(&d, 0, sizeof(d));
        unsigned ecu = 0, did = 0;
        float min = 0, max = 0;
        if (!valid_key(f[0])) {
            add_error(set, name, line_no, "id: lower-case letters, digits and _");
            continue;
        }
        if (!printable(f[1]) || !printable(f[2])) {
            add_error(set, name, line_no, "label or unit: printable characters only");
            continue;
        }
        if (!parse_hex(f[3], 0xFF, &ecu) || !parse_hex(f[4], 0xFFFF, &did)) {
            add_error(set, name, line_no, "ecu and did: hexadecimal (12, 0x4205)");
            continue;
        }
        char *pend = NULL;
        const long position = f[5][0] ? strtol(f[5], &pend, 10) : 1;
        if ((f[5][0] && *pend) || position < 1 || position > 250) {
            add_error(set, name, line_no, "pos: 1 for the first byte");
            continue;
        }
        if (!strcmp(f[6], "u8") || !strcmp(f[6], "s8")) {
            d.size = 1;
        } else if (!strcmp(f[6], "u16") || !strcmp(f[6], "s16")) {
            d.size = 2;
        } else if (!strcmp(f[6], "u32") || !strcmp(f[6], "s32")) {
            d.size = 4;
        } else {
            add_error(set, name, line_no, "type: u8, s8, u16, s16, u32 or s32");
            continue;
        }
        d.is_signed = f[6][0] == 's';
        if (!parse_float(f[7], 1.0f, &d.mul) || !parse_float(f[8], 1.0f, &d.div) ||
            !parse_float(f[9], 0.0f, &d.add) || d.div == 0.0f) {
            add_error(set, name, line_no, "mul, div, add: numbers, div not 0");
            continue;
        }
        int channel = channel_by_key(f[0]);
        if (derived_channel(channel)) {
            add_error(set, name, line_no, "slip comes from the wheel speeds (wheel_fl...)");
            continue;
        }
        bool twice = channel >= kCustom1;
        for (int i = 0; i < set->count && channel >= 0; ++i) {
            twice = twice || set->defs[i].channel == channel;
        }
        if (twice) {
            add_error(set, name, line_no, "id defined twice");
            continue;
        }
        const bool built_in = channel >= 0;
        if (!parse_float(f[10], 0.0f, &min) || !parse_float(f[11], 100.0f, &max) ||
            (!built_in && max <= min)) {
            add_error(set, name, line_no, "min, max: numbers, max above min");
            continue;
        }
        if (!built_in) {
            channel = next_custom();
            if (channel < 0) {
                add_error(set, name, line_no, "too many custom values (8)");
                continue;
            }
            set_custom(channel, f[0], f[1][0] ? f[1] : f[0], f[2], min, max);
        }
        d.channel = channel;
        d.ecu = static_cast<uint8_t>(ecu);
        d.did = static_cast<uint16_t>(did);
        d.pos = static_cast<uint8_t>(position);
        set->defs[set->count++] = d;
        ++added;
    }
    return added;
}

bool definition_value(const Definition &def, const uint8_t *record, size_t size, float *value) {
    const size_t start = def.pos - 1u;
    if (def.pos < 1 || start + def.size > size) {
        return false;
    }
    uint32_t raw = 0;
    for (unsigned i = 0; i < def.size; ++i) {
        raw = raw << 8 | record[start + i];
    }
    double v;
    if (def.is_signed) {
        const unsigned bits = def.size * 8u;
        const int64_t sign = static_cast<int64_t>(1) << (bits - 1);
        int64_t s = static_cast<int64_t>(raw);
        if (s & sign) {
            s -= sign << 1;
        }
        v = static_cast<double>(s);
    } else {
        v = static_cast<double>(raw);
    }
    *value = static_cast<float>(v * def.mul / def.div + def.add);
    return true;
}

} // namespace gauges
