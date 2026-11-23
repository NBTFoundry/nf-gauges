// SPDX-License-Identifier: MPL-2.0
// Display units: canonical values (channels.hpp) to the numbers and unit
// texts the screen shows, by the user's settings.
#ifndef GAUGES_UNITS_HPP
#define GAUGES_UNITS_HPP

#include <stddef.h>

#include "channels.hpp"

namespace gauges {

struct Units {
    bool imperial; // psi and degrees Fahrenheit, mph
    bool afr;      // lambda as petrol air-fuel ratio (14.7 x lambda)
};

/* A canonical value in display units. */
float to_display(int channel, float value, const Units &units);
/* The unit text ("bar", "psi", "\xc2\xb0""C", ...); "" for none. */
const char *unit_text(int channel, const Units &units);
/* Decimals the value is shown with. */
int decimals(int channel, const Units &units);
/* The value as text in display units ("--" when invalid). */
void format_value(char *out, size_t size, int channel, float value, bool valid,
                  const Units &units);

} // namespace gauges

#endif
