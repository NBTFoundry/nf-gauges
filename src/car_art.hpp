// SPDX-License-Identifier: MPL-2.0
// The car on the TRANSMISSION page: a sports saloon from above in the manner
// of a BMW M5 (F90), simplified and without badges. Drawn once, anti-aliased,
// into an ARGB8888 image: body with its shadow, bonnet creases and power dome,
// split grille, headlights, mirrors, glasshouse and carbon roof, door seams
// and front wing vents, boot lip spoiler, tail lights, four exhaust tips,
// dark wells where the wheels sit. The wheels and the drivetrain are drawn
// over it by the page (they move).
#ifndef GAUGES_CAR_ART_HPP
#define GAUGES_CAR_ART_HPP

#include <stddef.h>
#include <stdint.h>

namespace gauges {

struct CarGeometry {
    int width, height;     // the image, pixels
    float scale;           // pixels per unit: the car is 1000 units long, 382 wide
    int mid_x, top;        // the image's x of the car's middle line, y of its nose
    int front_axle, rear_axle; // y in the image
    int track;             // a wheel's middle from the middle line, pixels
    int tyre_length, front_tyre_width, rear_tyre_width;
    int gearbox_top, gearbox_bottom; // y of the gearbox (and transfer case)
};

/* The image for a car `length` pixels long (room around it for the mirrors,
 * the exhaust tips and the shadow). */
CarGeometry car_geometry(int length);

/* Draws the car into `argb` (bytes B, G, R, A per pixel, straight alpha),
 * `stride` bytes a row, the image car_geometry gave; clears it first. */
void car_draw(const CarGeometry &geometry, uint8_t *argb, int stride);

} // namespace gauges

#endif
