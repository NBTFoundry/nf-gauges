// SPDX-License-Identifier: MPL-2.0
#include "car_art.hpp"

#include <math.h>
#include <string.h>

namespace gauges {
namespace {

// The car in units: 1000 long (y, nose 0, tail 1000), 382 wide (x, 0 the
// middle line, the right side positive).
const float kHalfWidth = 191.0f;
const float kFrontAxle = 187.0f, kRearAxle = 788.0f;
const float kTrack = 162.0f;
// The image's margins, in units: mirrors, shadow, exhaust tips.
const float kSideRoom = 232.0f, kTopRoom = 22.0f, kLength = 1045.0f;

struct Pt {
    float x, y;
};

struct Rgb {
    float r, g, b;
};

Rgb hex(uint32_t c) {
    Rgb v = {static_cast<float>((c >> 16) & 255) / 255.0f, static_cast<float>((c >> 8) & 255) / 255.0f,
             static_cast<float>(c & 255) / 255.0f};
    return v;
}

Rgb mix(Rgb a, Rgb b, float t) {
    Rgb v = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    return v;
}

enum PaintKind { kSolid, kPaintBody, kCarbon };

struct Paint {
    PaintKind kind;
    Rgb a, b; // solid: a; body: a in the middle to b at the sides; carbon: two shades
    float alpha;
};

Paint solid(uint32_t c, float alpha = 1.0f) {
    Paint p = {kSolid, hex(c), hex(c), alpha};
    return p;
}

struct Canvas {
    uint8_t *px;
    int w, h, stride;
    float s, cx, top; // units to pixels
};

enum { kMaxPoints = 2048, kMaxCrossings = 128, kMaxWidth = 4096 };

Pt to_px(const Canvas &c, Pt u) {
    Pt p = {c.cx + u.x * c.s, c.top + u.y * c.s};
    return p;
}

Rgb colour_at(const Canvas &c, const Paint &p, int x, int y) {
    switch (p.kind) {
    case kPaintBody: {
        // Lighter along the middle (the roof line's reflection), darker
        // towards the flanks.
        float t = fabsf((static_cast<float>(x) + 0.5f - c.cx) / (kHalfWidth * c.s));
        t = t > 1.0f ? 1.0f : t;
        return mix(p.a, p.b, powf(t, 1.7f));
    }
    case kCarbon: {
        // A weave: two shades in a checker of two pixels.
        return ((x / 2) + (y / 2)) % 2 ? p.a : p.b;
    }
    default:
        return p.a;
    }
}

void blend(Canvas &c, int x, int y, Rgb col, float a) {
    if (a <= 0.0f) {
        return;
    }
    a = a > 1.0f ? 1.0f : a;
    uint8_t *p = c.px + static_cast<size_t>(y) * static_cast<size_t>(c.stride) + static_cast<size_t>(x) * 4u;
    const float da = p[3] / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) {
        return;
    }
    const float k = da * (1.0f - a);
    const float r = (col.r * a + p[2] / 255.0f * k) / oa;
    const float g = (col.g * a + p[1] / 255.0f * k) / oa;
    const float b = (col.b * a + p[0] / 255.0f * k) / oa;
    p[0] = static_cast<uint8_t>(b * 255.0f + 0.5f);
    p[1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
    p[2] = static_cast<uint8_t>(r * 255.0f + 0.5f);
    p[3] = static_cast<uint8_t>(oa * 255.0f + 0.5f);
}

void add_span(float *cover, int w, float xa, float xb, float weight) {
    xa = xa < 0 ? 0 : xa;
    xb = xb > static_cast<float>(w) ? static_cast<float>(w) : xb;
    if (xb <= xa) {
        return;
    }
    const int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
    if (ia == ib) {
        cover[ia] += (xb - xa) * weight;
        return;
    }
    cover[ia] += (static_cast<float>(ia + 1) - xa) * weight;
    for (int i = ia + 1; i < ib; ++i) {
        cover[i] += weight;
    }
    if (ib < w) {
        cover[ib] += (xb - static_cast<float>(ib)) * weight;
    }
}

// A polygon in pixels, even-odd, anti-aliased: four rows a pixel, exact
// coverage along them.
void fill_px(Canvas &c, const Pt *p, int n, const Paint &paint) {
    if (n < 3 || c.w > kMaxWidth) {
        return;
    }
    float miny = p[0].y, maxy = p[0].y, minx = p[0].x, maxx = p[0].x;
    for (int i = 1; i < n; ++i) {
        miny = fminf(miny, p[i].y);
        maxy = fmaxf(maxy, p[i].y);
        minx = fminf(minx, p[i].x);
        maxx = fmaxf(maxx, p[i].x);
    }
    const int y0 = miny < 0 ? 0 : static_cast<int>(miny);
    const int y1 = maxy >= static_cast<float>(c.h) ? c.h - 1 : static_cast<int>(maxy);
    const int x0 = minx < 0 ? 0 : static_cast<int>(minx);
    const int x1 = maxx >= static_cast<float>(c.w) ? c.w - 1 : static_cast<int>(maxx);
    static float cover[kMaxWidth + 1];
    float xs[kMaxCrossings];
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1 + 1 && x <= c.w; ++x) {
            cover[x] = 0;
        }
        for (int sub = 0; sub < 4; ++sub) {
            const float sy = static_cast<float>(y) + (static_cast<float>(sub) + 0.5f) / 4.0f;
            int k = 0;
            for (int i = 0; i < n && k < kMaxCrossings; ++i) {
                const Pt a = p[i], b = p[(i + 1) % n];
                if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                    xs[k++] = a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y);
                }
            }
            for (int i = 1; i < k; ++i) {
                const float v = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > v) {
                    xs[j + 1] = xs[j];
                    --j;
                }
                xs[j + 1] = v;
            }
            for (int i = 0; i + 1 < k; i += 2) {
                add_span(cover, c.w, xs[i], xs[i + 1], 0.25f);
            }
        }
        for (int x = x0; x <= x1; ++x) {
            if (cover[x] > 0.001f) {
                blend(c, x, y, colour_at(c, paint, x, y), cover[x] * paint.alpha);
            }
        }
    }
}

// A smooth closed (or open) curve through control points: Catmull-Rom.
int spline(const Pt *ctrl, int n, bool closed, Pt *out, int max) {
    const int steps = 8;
    int m = 0;
    const int segments = closed ? n : n - 1;
    for (int i = 0; i < segments; ++i) {
        const Pt p0 = ctrl[closed ? (i - 1 + n) % n : (i > 0 ? i - 1 : 0)];
        const Pt p1 = ctrl[i];
        const Pt p2 = ctrl[(i + 1) % n];
        const Pt p3 = ctrl[closed ? (i + 2) % n : (i + 2 < n ? i + 2 : n - 1)];
        for (int k = 0; k < steps && m < max; ++k) {
            const float t = static_cast<float>(k) / steps, t2 = t * t, t3 = t2 * t;
            Pt q;
            q.x = 0.5f * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                          (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
            q.y = 0.5f * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                          (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
            out[m++] = q;
        }
    }
    if (!closed && m < max) {
        out[m++] = ctrl[n - 1];
    }
    return m;
}

// A shape symmetric about the middle line from its right half, nose to tail
// (first and last points on the middle line).
int mirror(const Pt *half, int n, Pt *out) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        out[m++] = half[i];
    }
    for (int i = n - 2; i >= 1; --i) {
        Pt p = {-half[i].x, half[i].y};
        out[m++] = p;
    }
    return m;
}

// Fills a shape given in units; `smooth` passes it through the spline.
void fill(Canvas &c, const Pt *units, int n, bool smooth, const Paint &paint, bool flip = false) {
    static Pt curve[kMaxPoints];
    Pt tmp[256];
    const Pt *src = units;
    if (flip) {
        for (int i = 0; i < n && i < 256; ++i) {
            tmp[i].x = -units[i].x;
            tmp[i].y = units[i].y;
        }
        src = tmp;
    }
    int m = 0;
    if (smooth) {
        m = spline(src, n, true, curve, kMaxPoints);
    } else {
        for (int i = 0; i < n && i < kMaxPoints; ++i) {
            curve[m++] = src[i];
        }
    }
    for (int i = 0; i < m; ++i) {
        curve[i] = to_px(c, curve[i]);
    }
    fill_px(c, curve, m, paint);
}

// Both sides of a right-hand shape.
void fill_both(Canvas &c, const Pt *units, int n, bool smooth, const Paint &paint) {
    fill(c, units, n, smooth, paint, false);
    fill(c, units, n, smooth, paint, true);
}

void fill_symmetric(Canvas &c, const Pt *half, int n, const Paint &paint, float sx = 1.0f,
                    float sy = 1.0f) {
    Pt full[256];
    const int m = mirror(half, n, full);
    for (int i = 0; i < m; ++i) {
        full[i].x *= sx;
        full[i].y = 500.0f + (full[i].y - 500.0f) * sy;
    }
    fill(c, full, m, true, paint);
}

void ellipse(Canvas &c, float x, float y, float rx, float ry, const Paint &paint) {
    Pt p[48];
    for (int i = 0; i < 48; ++i) {
        const float a = static_cast<float>(i) * 6.2831853f / 48.0f;
        p[i].x = x + rx * cosf(a);
        p[i].y = y + ry * sinf(a);
    }
    fill(c, p, 48, false, paint);
}

// A line through points (units), `width` units wide, round ends.
void stroke(Canvas &c, const Pt *units, int n, float width, const Paint &paint, bool smooth = true,
            bool flip = false) {
    static Pt curve[kMaxPoints];
    Pt src[64];
    for (int i = 0; i < n && i < 64; ++i) {
        src[i].x = flip ? -units[i].x : units[i].x;
        src[i].y = units[i].y;
    }
    const int m = smooth && n > 2 ? spline(src, n, false, curve, kMaxPoints) : n;
    if (!(smooth && n > 2)) {
        for (int i = 0; i < n; ++i) {
            curve[i] = src[i];
        }
    }
    const float r = width / 2.0f;
    for (int i = 0; i + 1 < m; ++i) {
        const Pt a = curve[i], b = curve[i + 1];
        const float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) {
            continue;
        }
        // Each piece a little longer, so that the joins have no seams.
        const float nx = -dy / len * r, ny = dx / len * r, ex = dx / len * r * 0.5f, ey = dy / len * r * 0.5f;
        Pt q[4] = {{a.x + nx - ex, a.y + ny - ey}, {b.x + nx + ex, b.y + ny + ey},
                   {b.x - nx + ex, b.y - ny + ey}, {a.x - nx - ex, a.y - ny - ey}};
        for (int k = 0; k < 4; ++k) {
            q[k] = to_px(c, q[k]);
        }
        fill_px(c, q, 4, paint);
    }
}

void stroke_both(Canvas &c, const Pt *units, int n, float width, const Paint &paint, bool smooth = true) {
    stroke(c, units, n, width, paint, smooth, false);
    stroke(c, units, n, width, paint, smooth, true);
}

// --- the car ---

// The body's right half, nose to tail: a slightly pointed nose, the front
// wings flared over the wheels, a waist along the doors, broad rear haunches.
const Pt kBody[] = {
    {0, 0},     {48, 2},    {96, 8},    {136, 21},  {164, 43},  {180, 76},  {188, 116}, {191, 176},
    {190, 238}, {185, 290}, {181, 340}, {180, 450}, {181, 560}, {185, 640}, {191, 720}, {192, 790},
    {190, 852}, {184, 906}, {170, 946}, {144, 975}, {100, 993}, {50, 999},  {0, 1000},
};
// Windscreen, side windows and rear window as one.
const Pt kGlass[] = {
    {0, 318},   {70, 321},  {122, 330}, {148, 346}, {157, 374}, {160, 430}, {161, 520},
    {160, 610}, {156, 670}, {146, 720}, {124, 760}, {84, 783},  {40, 792},  {0, 794},
};
const Pt kRoof[] = {
    {0, 446},   {80, 448},  {121, 455}, {136, 474}, {140, 530},
    {140, 620}, {136, 668}, {121, 690}, {80, 700},  {0, 702},
};
const Pt kMirror[] = {{176, 352}, {204, 357}, {222, 372}, {221, 388}, {200, 387}, {178, 380}};
const Pt kKidney[] = {{7, 3}, {34, 1}, {60, 4}, {68, 11}, {62, 21}, {34, 24}, {9, 22}};
const Pt kHeadlight[] = {{64, 13},  {110, 16}, {148, 29}, {172, 55}, {178, 76},
                         {168, 74}, {144, 50}, {104, 33}, {68, 27}};
const Pt kDrl[] = {{84, 22}, {120, 27}, {150, 41}, {166, 62}};
const Pt kTailLight[] = {{102, 976}, {148, 962}, {176, 934}, {186, 904}, {184, 932},
                         {164, 966}, {124, 987}, {100, 990}};
const Pt kTailLine[] = {{112, 982}, {150, 968}, {174, 940}};
const Pt kSpoiler[] = {{0, 950},   {78, 946},  {138, 934}, {168, 912}, {170, 924},
                       {142, 950}, {80, 961},  {0, 964}};
const Pt kFin[] = {{0, 684}, {6, 690}, {7, 704}, {0, 713}, {-7, 704}, {-6, 690}};
const Pt kGill[] = {{177, 258}, {186, 260}, {187, 296}, {178, 294}};

void draw_car(Canvas &c) {
    const uint32_t rim = 0x5A6B82, paint_mid = 0x31485F, paint_side = 0x111923;
    Paint body = {kPaintBody, hex(paint_mid), hex(paint_side), 1.0f};
    // The shadow on the road.
    fill_symmetric(c, kBody, sizeof(kBody) / sizeof(kBody[0]), solid(0x000000, 0.10f), 1.10f, 1.035f);
    fill_symmetric(c, kBody, sizeof(kBody) / sizeof(kBody[0]), solid(0x000000, 0.18f), 1.06f, 1.02f);
    fill_symmetric(c, kBody, sizeof(kBody) / sizeof(kBody[0]), solid(0x000000, 0.30f), 1.03f, 1.01f);
    // Four exhaust tips, peeking out under the rear bumper.
    const float tips[2] = {60, 90};
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < 2; ++i) {
            ellipse(c, side * tips[i], 1003, 12, 7, solid(0x9AA4B1));
            ellipse(c, side * tips[i], 1004, 8.5f, 4.5f, solid(0x050608));
        }
    }
    // Mirrors, under the body's edge.
    fill_both(c, kMirror, sizeof(kMirror) / sizeof(kMirror[0]), true, solid(rim));
    // The body: a light rim, then the paint inset in it.
    fill_symmetric(c, kBody, sizeof(kBody) / sizeof(kBody[0]), solid(rim));
    fill_symmetric(c, kBody, sizeof(kBody) / sizeof(kBody[0]), body, 1.0f - 3.0f / kHalfWidth,
                   1.0f - 3.0f / 500.0f);
    Pt mirror_inner[6];
    for (int i = 0; i < 6; ++i) {
        mirror_inner[i].x = kMirror[i].x + (i < 3 ? -1.0f : 1.0f);
        mirror_inner[i].y = kMirror[i].y + (i == 0 || i == 5 ? 0.0f : (i < 3 ? 2.0f : -2.0f));
    }
    fill_both(c, mirror_inner, 6, true, solid(0x24364A));
    // Dark wells where the wheels sit.
    for (int side = -1; side <= 1; side += 2) {
        ellipse(c, side * kTrack, kFrontAxle, 31, 68, solid(0x050608, 0.92f));
        ellipse(c, side * kTrack, kRearAxle, 34, 68, solid(0x050608, 0.92f));
    }
    // The bonnet: the power dome's two ridges from the grille, the outer
    // creases from the headlights, each a light edge with a shadow beside it.
    const Pt dome[] = {{30, 24}, {46, 120}, {60, 220}, {70, 312}};
    const Pt dome_shadow[] = {{35, 24}, {51, 120}, {65, 220}, {75, 312}};
    const Pt crease[] = {{116, 40}, {133, 140}, {144, 240}, {149, 330}};
    const Pt crease_shadow[] = {{121, 42}, {138, 140}, {149, 240}, {154, 330}};
    stroke_both(c, dome_shadow, 4, 3.0f, solid(0x0E151E));
    stroke_both(c, dome, 4, 2.5f, solid(0x5B6D85));
    stroke_both(c, crease_shadow, 4, 3.0f, solid(0x0E151E));
    stroke_both(c, crease, 4, 2.5f, solid(0x4F6077));
    // A split grille at the nose, its chrome surround.
    fill_both(c, kKidney, sizeof(kKidney) / sizeof(kKidney[0]), true, solid(0x8E99A8));
    Pt kidney_inner[7];
    for (int i = 0; i < 7; ++i) {
        kidney_inner[i].x = 36 + (kKidney[i].x - 36) * 0.8f;
        kidney_inner[i].y = 12 + (kKidney[i].y - 12) * 0.62f;
    }
    fill_both(c, kidney_inner, 7, true, solid(0x07090C));
    // Headlights: dark glass, a light strip.
    fill_both(c, kHeadlight, sizeof(kHeadlight) / sizeof(kHeadlight[0]), true, solid(0x3A4555));
    Pt light_inner[9];
    for (int i = 0; i < 9; ++i) {
        light_inner[i].x = 122 + (kHeadlight[i].x - 122) * 0.9f;
        light_inner[i].y = 44 + (kHeadlight[i].y - 44) * 0.75f;
    }
    fill_both(c, light_inner, 9, true, solid(0x0D1218));
    stroke_both(c, kDrl, 4, 4.0f, solid(0xDCEBFF));
    // The glasshouse, a reflection across the windscreen, the carbon roof.
    fill_symmetric(c, kGlass, sizeof(kGlass) / sizeof(kGlass[0]), solid(0x2A3644));
    fill_symmetric(c, kGlass, sizeof(kGlass) / sizeof(kGlass[0]), solid(0x090D12), 1.0f - 3.0f / 160.0f,
                   1.0f - 2.0f / 500.0f);
    const Pt glare[] = {{-70, 336}, {-10, 330}, {-70, 440}, {-120, 440}};
    fill(c, glare, 4, false, solid(0xFFFFFF, 0.05f));
    Paint carbon = {kCarbon, hex(0x1B2129), hex(0x13171D), 1.0f};
    fill_symmetric(c, kRoof, sizeof(kRoof) / sizeof(kRoof[0]), solid(0x2F3A48));
    fill_symmetric(c, kRoof, sizeof(kRoof) / sizeof(kRoof[0]), carbon, 1.0f - 2.0f / 140.0f,
                   1.0f - 2.0f / 500.0f);
    // Pillars in the paint: A, B, C.
    const Pt a_pillar[] = {{150, 350}, {138, 472}};
    const Pt b_pillar[] = {{140, 578}, {160, 580}};
    const Pt c_pillar[] = {{137, 670}, {150, 716}};
    stroke_both(c, a_pillar, 2, 9.0f, solid(0x1E2B3A), false);
    stroke_both(c, b_pillar, 2, 7.0f, solid(0x1A2633), false);
    stroke_both(c, c_pillar, 2, 11.0f, solid(0x1E2B3A), false);
    // Wipers at the windscreen's foot.
    const Pt wiper_l[] = {{-118, 338}, {-26, 327}};
    const Pt wiper_r[] = {{6, 335}, {96, 330}};
    stroke(c, wiper_l, 2, 2.5f, solid(0x040506), false);
    stroke(c, wiper_r, 2, 2.5f, solid(0x040506), false);
    // The aerial's fin at the roof's end.
    fill(c, kFin, sizeof(kFin) / sizeof(kFin[0]), true, solid(0x0B0E12));
    // Door seams, handles, the front wings' vents.
    const Pt seam_b[] = {{161, 580}, {183, 582}};
    const Pt seam_c[] = {{158, 702}, {170, 712}, {186, 716}};
    const Pt seam_a[] = {{160, 384}, {181, 388}};
    stroke_both(c, seam_a, 2, 1.8f, solid(0x0B1016), false);
    stroke_both(c, seam_b, 2, 1.8f, solid(0x0B1016), false);
    stroke_both(c, seam_c, 3, 1.8f, solid(0x0B1016));
    const Pt handle_f[] = {{171, 470}, {180, 470}, {180, 486}, {171, 486}};
    const Pt handle_r[] = {{171, 650}, {181, 650}, {181, 666}, {171, 666}};
    fill_both(c, handle_f, 4, true, solid(0x52637A));
    fill_both(c, handle_r, 4, true, solid(0x52637A));
    fill_both(c, kGill, sizeof(kGill) / sizeof(kGill[0]), false, solid(0x07090C));
    const Pt gill_edge[] = {{177, 258}, {178, 294}};
    stroke_both(c, gill_edge, 2, 1.5f, solid(0x8E99A8), false);
    // The boot: its lid's edge, the lip spoiler at its end.
    const Pt boot[] = {{-150, 804}, {-110, 808}, {0, 812}, {110, 808}, {150, 804}};
    stroke(c, boot, 5, 2.0f, solid(0x0B1016));
    fill_symmetric(c, kSpoiler, sizeof(kSpoiler) / sizeof(kSpoiler[0]), solid(0x0E141B));
    const Pt lip[] = {{-138, 935}, {-78, 947}, {0, 951}, {78, 947}, {138, 935}};
    stroke(c, lip, 5, 1.8f, solid(0x4F6077));
    // Tail lights wrapping the corners.
    fill_both(c, kTailLight, sizeof(kTailLight) / sizeof(kTailLight[0]), true, solid(0x8E1E26));
    stroke_both(c, kTailLine, 3, 3.0f, solid(0xFF5A57));
}

} // namespace

CarGeometry car_geometry(int length) {
    CarGeometry g;
    const float s = static_cast<float>(length) / 1000.0f;
    g.scale = s;
    g.width = static_cast<int>(ceilf(2.0f * kSideRoom * s)) + 2;
    g.height = static_cast<int>(ceilf(kLength * s)) + 2;
    g.mid_x = g.width / 2;
    g.top = static_cast<int>(roundf(kTopRoom * s));
    g.front_axle = g.top + static_cast<int>(roundf(kFrontAxle * s));
    g.rear_axle = g.top + static_cast<int>(roundf(kRearAxle * s));
    g.track = static_cast<int>(roundf(kTrack * s));
    g.tyre_length = static_cast<int>(roundf(128.0f * s));
    g.front_tyre_width = static_cast<int>(roundf(52.0f * s));
    g.rear_tyre_width = static_cast<int>(roundf(58.0f * s));
    g.gearbox_top = g.top + static_cast<int>(roundf(345.0f * s));
    g.gearbox_bottom = g.top + static_cast<int>(roundf(475.0f * s));
    return g;
}

void car_draw(const CarGeometry &g, uint8_t *argb, int stride) {
    for (int y = 0; y < g.height; ++y) {
        memset(argb + static_cast<size_t>(y) * static_cast<size_t>(stride), 0, static_cast<size_t>(g.width) * 4u);
    }
    Canvas c;
    c.px = argb;
    c.w = g.width;
    c.h = g.height;
    c.stride = stride;
    c.s = g.scale;
    c.cx = static_cast<float>(g.width) / 2.0f;
    c.top = static_cast<float>(g.top);
    draw_car(c);
}

} // namespace gauges
