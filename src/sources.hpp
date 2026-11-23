// SPDX-License-Identifier: MPL-2.0
// The other data sources: iDrive's own car data (capability car-data: engine
// speed, coolant and oil temperature, torque, pedal, speed, outside
// temperature, acceleration, steering angle), and a simulated drive for the desktop and for showing the
// screens without a car.
#ifndef GAUGES_SOURCES_HPP
#define GAUGES_SOURCES_HPP

#include <stddef.h>
#include <stdint.h>

#include "channels.hpp"

namespace nbt {
namespace app {
class CarData;
}
} // namespace nbt

namespace gauges {

/* The car-data names the package declares (release/package.args). */
extern const char *const kCarDataNames[];
extern const size_t kCarDataNameCount;

/* The number after "key": in iDrive's JSON (the first such key); false when
 * absent. */
bool json_number(const char *text, const char *key, double *out);

/* One car data number as the channel's value (gear codes converted, the
 * "none" of acceleration left out); false when there is none. */
bool car_data_value(int channel, double raw, float *out);

/* Reads the car data granted to the application into the store; returns how
 * many values were new. */
int poll_car_data(const nbt::app::CarData &car, Store *store, uint64_t now_ms);

/* A drive of 24 s, over and over: idle, a pull through two gears (with some
 * knock on two cylinders and wheelspin), lifting off, cruising through a bend.
 * `t_ms`: since the start. `diesel`: also a particulate filter that starts a
 * regeneration. */
void demo_values(uint64_t t_ms, Store *store, uint64_t now_ms, bool diesel = false);

} // namespace gauges

#endif
