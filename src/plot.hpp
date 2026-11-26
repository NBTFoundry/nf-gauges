// SPDX-License-Identifier: MPL-2.0
// Curves for the torque and power chart: a smooth line through a few points,
// drawn anti-aliased at fractions of a pixel into an ARGB8888 image. LVGL's
// own lines join whole pixels, which shows as teeth on a slow slope.
#ifndef GAUGES_PLOT_HPP
#define GAUGES_PLOT_HPP

#include <stdint.h>

namespace gauges {

/* Clears the image (transparent). */
void plot_clear(uint8_t *argb, int height, int stride);

/* Draws a line `line_width` pixels wide through the `n` points over what the
 * image holds (bytes B, G, R, A per pixel, straight alpha). The points are in
 * pixels, (0, 0) the middle of the first one, their x rising; between them
 * the curve never passes over or under its two neighbours. One point is a
 * dot. False when memory is short (nothing is drawn). */
bool plot_line(uint8_t *argb, int width, int height, int stride, const float *x, const float *y, int n,
               float line_width, uint32_t rgb);

} // namespace gauges

#endif
