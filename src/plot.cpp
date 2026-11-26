// SPDX-License-Identifier: MPL-2.0
#include "plot.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace gauges {

namespace {

float clamp01(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// What the stroke from a to b covers of each pixel near it: the most of this
// and what the line's other strokes left, so that joints are not drawn twice.
void cover_stroke(uint8_t *cover, int width, int height, float ax, float ay, float bx, float by, float half) {
    const float reach = half + 1.0f;
    int x0 = static_cast<int>(floorf((ax < bx ? ax : bx) - reach)), x1 = static_cast<int>(ceilf((ax < bx ? bx : ax) + reach));
    int y0 = static_cast<int>(floorf((ay < by ? ay : by) - reach)), y1 = static_cast<int>(ceilf((ay < by ? by : ay) + reach));
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= width ? width - 1 : x1;
    y1 = y1 >= height ? height - 1 : y1;
    const float dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) - ax, py = static_cast<float>(y) - ay;
            const float along = length2 > 0 ? clamp01((px * dx + py * dy) / length2) : 0;
            const float ox = px - along * dx, oy = py - along * dy;
            const float alpha = clamp01(half + 0.5f - sqrtf(ox * ox + oy * oy));
            const uint8_t value = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
            uint8_t *c = cover + static_cast<size_t>(y) * static_cast<size_t>(width) + x;
            if (value > *c) {
                *c = value;
            }
        }
    }
}

// The slope at point i of a curve that keeps between its points (Fritsch and
// Carlson's monotone cubic).
float slope_at(const float *x, const float *y, int n, int i) {
    if (i == 0) {
        return (y[1] - y[0]) / (x[1] - x[0]);
    }
    if (i == n - 1) {
        return (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]);
    }
    const float before = (y[i] - y[i - 1]) / (x[i] - x[i - 1]), after = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
    if (before * after <= 0) {
        return 0;
    }
    const float mean = (before + after) / 2, least = fabsf(before) < fabsf(after) ? fabsf(before) : fabsf(after);
    return fabsf(mean) > 3 * least ? (mean < 0 ? -3 * least : 3 * least) : mean;
}

} // namespace

void plot_clear(uint8_t *argb, int height, int stride) {
    memset(argb, 0, static_cast<size_t>(height) * static_cast<size_t>(stride));
}

bool plot_line(uint8_t *argb, int width, int height, int stride, const float *x, const float *y, int n,
               float line_width, uint32_t rgb) {
    if (n < 1 || width < 1 || height < 1) {
        return true;
    }
    uint8_t *cover = static_cast<uint8_t *>(calloc(static_cast<size_t>(width), static_cast<size_t>(height)));
    if (!cover) {
        return false;
    }
    const float half = line_width / 2;
    if (n == 1) {
        cover_stroke(cover, width, height, x[0], y[0], x[0], y[0], half);
    }
    for (int i = 0; i + 1 < n; ++i) {
        // One stroke a pixel of x at most.
        const float h = x[i + 1] - x[i], m0 = slope_at(x, y, n, i), m1 = slope_at(x, y, n, i + 1);
        const int steps = h > 1 ? static_cast<int>(ceilf(h)) : 1;
        float ax = x[i], ay = y[i];
        for (int s = 1; s <= steps; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(steps), u2 = u * u, u3 = u2 * u;
            const float bx = x[i] + u * h;
            const float by = (2 * u3 - 3 * u2 + 1) * y[i] + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * y[i + 1] +
                             (u3 - u2) * h * m1;
            cover_stroke(cover, width, height, ax, ay, bx, by, half);
            ax = bx;
            ay = by;
        }
    }
    const float colour[3] = {static_cast<float>(rgb & 0xFF), static_cast<float>((rgb >> 8) & 0xFF),
                             static_cast<float>((rgb >> 16) & 0xFF)}; // B, G, R
    for (int py = 0; py < height; ++py) {
        uint8_t *row = argb + static_cast<size_t>(py) * static_cast<size_t>(stride);
        const uint8_t *c = cover + static_cast<size_t>(py) * static_cast<size_t>(width);
        for (int px = 0; px < width; ++px) {
            if (!c[px]) {
                continue;
            }
            uint8_t *p = row + px * 4;
            const float sa = static_cast<float>(c[px]) / 255.0f, da = static_cast<float>(p[3]) / 255.0f;
            const float out = sa + da * (1 - sa);
            for (int k = 0; k < 3; ++k) {
                p[k] = static_cast<uint8_t>((colour[k] * sa + static_cast<float>(p[k]) * da * (1 - sa)) / out + 0.5f);
            }
            p[3] = static_cast<uint8_t>(out * 255.0f + 0.5f);
        }
    }
    free(cover);
    return true;
}

} // namespace gauges
