// SPDX-License-Identifier: MPL-2.0
#include "ui.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "car_art.hpp"
#include "lvgl.h"
#include "nbt/app/log.hpp"

namespace gauges {
namespace {

// Colours: near-black background, slate panels, one cyan accent; amber and
// red only for warnings.
const uint32_t kBackground = 0x07090C;
const uint32_t kPanel = 0x0F141B;
const uint32_t kPanelBorder = 0x1A212B;
const uint32_t kTrack = 0x1C232D;
const uint32_t kText = 0xF2F5F8;
const uint32_t kTextSecondary = 0x8B96A5;
const uint32_t kTextDim = 0x4F5967;
const uint32_t kAccent = 0x2EC5FF;
const uint32_t kWarn = 0xFFB020;
const uint32_t kDanger = 0xFF4D4F;
const uint32_t kGood = 0x3DDC84;

const int kSweep = 270;    // a gauge's arc, degrees
const int kRotation = 135; // where it starts (LVGL: 0 = 3 o'clock, clockwise)

const char *const kPageNames[Ui::kPageCount] = {"ENGINE",       "FUEL & AIR", "TEMPERATURES",
                                                "IGNITION",     "TRANSMISSION", "MORE",
                                                "CONNECTION"};

struct Fonts {
    const lv_font_t *huge, *big, *large, *mid, *small, *tiny;
};

struct Gauge {
    lv_obj_t *box, *track, *arc, *target, *scale, *value, *unit, *name, *peak;
    int channel, target_channel;
    float lo, hi, origin; // canonical range shown and the arc's start
    float shown;          // smoothed, canonical
    bool has_shown;
    int a0, a1, t0;       // last angles drawn
    char text[16], peak_text[48], unit_text[12];
    uint32_t colour;
    char ticks[12][12];
    const char *tick_ptrs[13];
};

struct Tile {
    lv_obj_t *panel, *name, *value, *unit, *peak, *bar;
    int channel;
    char text[16], peak_text[32], unit_text[12];
    uint32_t colour;
    int bar_value;
    bool show_peak; // not in MORE's narrow tiles
};

struct CylBar {
    lv_obj_t *col, *value, *bar, *name;
    char text[16];
    int bar_value;
    uint32_t colour;
};

enum { kMoreTiles = 8, kDpfTiles = 6, kGauges = 5, kMaxTiles = 6 + 6 + kMoreTiles + kDpfTiles };

// TRANSMISSION: one wheel, drawn on the car, and its numbers beside it.
struct WheelView {
    lv_obj_t *tyre, *rim, *block, *name, *speed, *unit, *slip, *pressure;
    bool right; // on the car's right: the numbers to the right of it
    int angle;  // tenths of a degree drawn (front wheels steer)
    uint32_t colour, slip_colour;
    char speed_text[16], unit_text[12], slip_text[24], pressure_text[24];
};

// Torque and power against engine speed: 1000 to 7500 rpm, every 100.
enum { kDynoFrom = 1000, kDynoStep = 100, kDynoBins = 66 };

struct TransmissionView {
    CarGeometry geo;
    lv_draw_buf_t *car_buf;
    lv_obj_t *car;
    int car_x, car_y; // the car's image on the page
    WheelView wheels[4]; // FL, FR, RL, RR
    lv_obj_t *gearbox, *transfer, *rear_shaft, *rear_axle, *rear_diff, *front_shaft, *front_axle,
        *front_diff;
    lv_point_precise_t tm_points[4][2]; // lines' ends (LVGL keeps the pointers)
    int front_opa, rear_opa;
    Drivetrain drivetrain; // the setting
    int xdrive;            // drawn with the front axle driven (1) or not (0); -1 not yet
    bool xdrive_seen;      // the split or the transfer case answered
    float steer_shown;
    bool has_steer;
    // The torque and power curves.
    lv_obj_t *chart, *torque_legend, *power_legend, *torque_labels[3], *power_labels[3];
    lv_chart_series_t *torque_series, *power_series;
    lv_obj_t *cursor, *torque_dot, *power_dot, *now_text, *peak_text;
    int chart_x, chart_y, chart_w, chart_h;
    float torque_max[kDynoBins]; // Nm, -1 for none yet
    float torque_top, power_top; // the axes' tops, display units
    bool dyno_dirty;
    char now_buf[48], peak_buf[80];
    // The gear, the drive, the gearbox, the steering.
    lv_obj_t *gear, *mode, *drive_title, *split_bar, *front_label, *rear_label, *rwd, *front_value,
        *rear_value, *clutch, *gearbox_temp, *steering, *note;
    int split_value;
    char gear_buf[8], mode_buf[16], front_buf[16], rear_buf[16], clutch_buf[40], gearbox_buf[40],
        steering_buf[40];
};

struct State {
    int width, height, bar_h, pad;
    Fonts fonts;
    Units units;
    float boost_max;
    lv_obj_t *screen, *title, *dots[Ui::kPageCount], *status_dot, *status;
    lv_obj_t *pages[Ui::kPageCount];
    Gauge gauges[kGauges]; // engine: boost, lambda, timing; ignition: timing
    Tile tiles[kMaxTiles];
    int tile_count;
    int more_first; // index of the first MORE tile
    CylBar cyl[kCylinders];
    lv_obj_t *cyl_box, *cyl_note;
    lv_obj_t *ign_box, *dpf_box; // the IGNITION page's two forms
    int dpf_first;               // index of the first DPF tile
    bool diesel;
    TransmissionView tm;
    lv_obj_t *info;
    char status_text[48];
    uint32_t status_colour;
    char info_text[2048];
};

State g;

// --- helpers ---

lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

// Angles carry their degree sign on the number ("12.5\xc2\xb0"), not under it.
bool unit_inline(int channel) {
    return channel_info(channel).quantity == kQAngle;
}

void value_text(char *out, size_t size, int channel, float value, bool ok) {
    format_value(out, size, channel, value, ok, g.units);
    if (ok && unit_inline(channel)) {
        const size_t n = strlen(out);
        snprintf(out + n, size - n, "%s", unit_text(channel, g.units));
    }
}

const char *shown_unit(int channel) {
    return unit_inline(channel) ? "" : unit_text(channel, g.units);
}

void set_text(lv_obj_t *l, char *cache, size_t size, const char *text) {
    if (strcmp(cache, text) != 0) {
        snprintf(cache, size, "%s", text);
        lv_label_set_text(l, cache);
    }
}

void set_colour(lv_obj_t *o, uint32_t *cache, uint32_t colour, lv_style_selector_t sel,
                bool text) {
    if (*cache != colour) {
        *cache = colour;
        if (text) {
            lv_obj_set_style_text_color(o, lv_color_hex(colour), sel);
        } else if (sel == LV_PART_INDICATOR) {
            lv_obj_set_style_arc_color(o, lv_color_hex(colour), sel);
            lv_obj_set_style_bg_color(o, lv_color_hex(colour), sel);
        }
    }
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// The colour a value is shown in: amber above the warning, red above danger.
uint32_t level_colour(int channel, float value, uint32_t normal) {
    const ChannelInfo &info = channel_info(channel);
    if (!isnan(info.danger) && value >= info.danger) {
        return kDanger;
    }
    if (!isnan(info.warn) && value >= info.warn) {
        return kWarn;
    }
    return normal;
}

// A canonical range in display units, with round ticks.
struct Ticks {
    float lo, hi, step; // display units
    int intervals;
    int places;
};

Ticks nice_ticks(float lo, float hi) {
    Ticks t;
    const float span = hi - lo > 0 ? hi - lo : 1.0f;
    const float raw = span / 5.0f;
    const float mag = powf(10.0f, floorf(log10f(raw)));
    const float steps[] = {1.0f, 2.0f, 2.5f, 5.0f, 10.0f};
    t.step = 10.0f * mag;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (steps[i] * mag >= raw * 0.999f) {
            t.step = steps[i] * mag;
            break;
        }
    }
    t.lo = floorf(lo / t.step + 0.001f) * t.step;
    t.hi = ceilf(hi / t.step - 0.001f) * t.step;
    t.intervals = static_cast<int>(roundf((t.hi - t.lo) / t.step));
    if (t.intervals < 1) {
        t.intervals = 1;
    }
    if (t.intervals > 11) {
        t.intervals = 11;
    }
    t.places = t.step >= 1.0f ? 0 : t.step * 10.0f >= 1.0f - 0.001f && fmodf(t.step * 10.0f, 1.0f) < 0.01f ? 1 : 2;
    return t;
}

// Display value back to canonical (inverse of to_display, for ranges).
float from_display(int channel, float shown, const Units &units) {
    const float probe = to_display(channel, 1.0f, units) - to_display(channel, 0.0f, units);
    const float offset = to_display(channel, 0.0f, units);
    return probe != 0 ? (shown - offset) / probe : shown;
}

int angle_of(const Gauge &gg, float v) {
    const float f = (clampf(v, gg.lo, gg.hi) - gg.lo) / (gg.hi - gg.lo);
    return static_cast<int>(roundf(f * kSweep));
}

// --- the top bar ---

void build_bar() {
    lv_obj_t *bar = plain(g.screen);
    lv_obj_set_size(bar, g.width, g.bar_h);
    lv_obj_set_pos(bar, 0, 0);
    g.title = label(bar, g.fonts.small, kTextSecondary, "");
    lv_obj_set_style_text_letter_space(g.title, 2, 0);
    lv_obj_align(g.title, LV_ALIGN_LEFT_MID, g.pad, 0);
    const int dot = g.bar_h / 6 > 6 ? g.bar_h / 6 : 6;
    const int spacing = dot * 3;
    const int x0 = (g.width - (Ui::kPageCount - 1) * spacing - dot) / 2;
    for (int i = 0; i < Ui::kPageCount; ++i) {
        lv_obj_t *d = plain(bar);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(kTrack), 0);
        lv_obj_set_pos(d, x0 + i * spacing, (g.bar_h - dot) / 2);
        g.dots[i] = d;
    }
    g.status = label(bar, g.fonts.small, kTextSecondary, "");
    lv_obj_set_style_text_letter_space(g.status, 1, 0);
    lv_obj_align(g.status, LV_ALIGN_RIGHT_MID, -g.pad, 0);
    g.status_dot = plain(bar);
    lv_obj_set_size(g.status_dot, dot + 2, dot + 2);
    lv_obj_set_style_radius(g.status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g.status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.status_dot, lv_color_hex(kTextDim), 0);
    // A thin line under the bar.
    lv_obj_t *rule = plain(g.screen);
    lv_obj_set_size(rule, g.width - 2 * g.pad, 1);
    lv_obj_set_pos(rule, g.pad, g.bar_h - 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(kPanelBorder), 0);
}

// --- round gauges ---

void style_arc(lv_obj_t *arc, int width, uint32_t colour, lv_part_t part) {
    lv_obj_set_style_arc_width(arc, width, part);
    lv_obj_set_style_arc_color(arc, lv_color_hex(colour), part);
    lv_obj_set_style_arc_rounded(arc, true, part);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, part);
}

lv_obj_t *make_arc(lv_obj_t *parent, int size) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, kRotation);
    lv_arc_set_bg_angles(arc, 0, kSweep);
    lv_arc_set_angles(arc, 0, 0);
    return arc;
}

void build_gauge(Gauge &gg, lv_obj_t *parent, int x, int y, int size, int channel,
                 int target_channel) {
    memset(&gg, 0, sizeof(gg));
    gg.channel = channel;
    gg.target_channel = target_channel;
    gg.a0 = gg.a1 = gg.t0 = -1;
    gg.box = plain(parent);
    lv_obj_set_size(gg.box, size, size);
    lv_obj_set_pos(gg.box, x, y);
    const int track = size / 16;
    // Track: the background arc; value: a second arc's indicator.
    gg.track = make_arc(gg.box, size);
    style_arc(gg.track, track, kTrack, LV_PART_MAIN);
    lv_obj_set_style_arc_width(gg.track, 0, LV_PART_INDICATOR);
    gg.arc = make_arc(gg.box, size);
    lv_obj_set_style_arc_width(gg.arc, 0, LV_PART_MAIN);
    style_arc(gg.arc, track, kAccent, LV_PART_INDICATOR);
    gg.colour = kAccent;
    // Target: a short white mark across the track.
    gg.target = make_arc(gg.box, size);
    lv_obj_set_style_arc_width(gg.target, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(gg.target, track, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gg.target, lv_color_hex(kText), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(gg.target, false, LV_PART_INDICATOR);
    lv_obj_set_hidden(gg.target, true);
    // Ticks and their labels inside the track.
    const int inner = size - 2 * track - size / 20;
    gg.scale = lv_scale_create(gg.box);
    lv_obj_remove_style_all(gg.scale);
    lv_obj_set_size(gg.scale, inner, inner);
    lv_obj_center(gg.scale);
    lv_scale_set_mode(gg.scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(gg.scale, kRotation);
    lv_scale_set_angle_range(gg.scale, kSweep);
    lv_scale_set_label_show(gg.scale, true);
    lv_obj_set_style_arc_width(gg.scale, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(gg.scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(gg.scale, lv_color_hex(kTextSecondary), LV_PART_INDICATOR);
    lv_obj_set_style_length(gg.scale, size / 28, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(gg.scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(gg.scale, lv_color_hex(kTextDim), LV_PART_ITEMS);
    lv_obj_set_style_length(gg.scale, size / 60, LV_PART_ITEMS);
    lv_obj_set_style_text_font(gg.scale, g.fonts.tiny, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(gg.scale, lv_color_hex(kTextSecondary), LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(gg.scale, size / 70, LV_PART_INDICATOR);
    // The value, its unit, the peak, and the name in the gap at the bottom.
    gg.value = label(gg.box, g.fonts.huge, kText, "--");
    lv_obj_align(gg.value, LV_ALIGN_CENTER, 0, -size / 30);
    gg.unit = label(gg.box, g.fonts.mid, kTextSecondary, "");
    lv_obj_align(gg.unit, LV_ALIGN_CENTER, 0, size / 7);
    gg.peak = label(gg.box, g.fonts.tiny, kTextDim, "");
    lv_obj_align(gg.peak, LV_ALIGN_CENTER, 0, size / 4);
    gg.name = label(gg.box, g.fonts.small, kTextSecondary, channel_info(channel).label);
    lv_obj_set_style_text_letter_space(gg.name, 2, 0);
    lv_obj_align(gg.name, LV_ALIGN_BOTTOM_MID, 0, -size / 40);
}

// The gauge's range in display units, its ticks and labels.
void configure_gauge(Gauge &gg) {
    const ChannelInfo &info = channel_info(gg.channel);
    float hi = info.max;
    if (gg.channel == kBoost) {
        hi = g.boost_max;
    }
    float a = to_display(gg.channel, info.min, g.units);
    float b = to_display(gg.channel, hi, g.units);
    if (a > b) {
        const float t = a;
        a = b;
        b = t;
    }
    const Ticks t = nice_ticks(a, b);
    gg.lo = from_display(gg.channel, t.lo, g.units);
    gg.hi = from_display(gg.channel, t.hi, g.units);
    gg.origin = clampf(info.origin, gg.lo, gg.hi);
    for (int i = 0; i <= t.intervals && i < 12; ++i) {
        const float v = t.lo + t.step * static_cast<float>(i);
        snprintf(gg.ticks[i], sizeof(gg.ticks[i]), "%.*f", t.places, fabsf(v) < 1e-4f ? 0.0 : static_cast<double>(v));
        gg.tick_ptrs[i] = gg.ticks[i];
    }
    gg.tick_ptrs[t.intervals + 1 < 13 ? t.intervals + 1 : 12] = NULL;
    const int minor = 5;
    lv_scale_set_range(gg.scale, 0, t.intervals * minor);
    lv_scale_set_total_tick_count(gg.scale, static_cast<uint32_t>(t.intervals * minor + 1));
    lv_scale_set_major_tick_every(gg.scale, minor);
    lv_scale_set_text_src(gg.scale, gg.tick_ptrs);
    gg.a0 = gg.a1 = gg.t0 = -1;
    gg.text[0] = gg.peak_text[0] = gg.unit_text[0] = '\0';
    lv_label_set_text(gg.unit, "");
}

void update_gauge(Gauge &gg, const UiInput &in) {
    const Sample &s = in.samples[gg.channel];
    const bool ok = fresh(s, in.now_ms);
    // The arc follows the value smoothly; the number shows it as read.
    if (ok) {
        if (!gg.has_shown) {
            gg.shown = s.value;
            gg.has_shown = true;
        }
        gg.shown += (s.value - gg.shown) * 0.35f;
    } else {
        gg.has_shown = false;
        gg.shown = gg.origin;
    }
    const int o = angle_of(gg, gg.origin), v = angle_of(gg, gg.shown);
    const int a0 = v < o ? v : o, a1 = v < o ? o : v;
    if (a0 != gg.a0 || a1 != gg.a1) {
        gg.a0 = a0;
        gg.a1 = a1;
        lv_arc_set_angles(gg.arc, a0, a1);
    }
    set_colour(gg.arc, &gg.colour, ok ? level_colour(gg.channel, s.value, kAccent) : kTrack,
               LV_PART_INDICATOR, false);
    char text[48];
    value_text(text, sizeof(text), gg.channel, s.value, ok);
    set_text(gg.value, gg.text, sizeof(gg.text), text);
    set_text(gg.unit, gg.unit_text, sizeof(gg.unit_text), shown_unit(gg.channel));
    if (ok) {
        char m[16];
        // The peak that matters: the highest, or for timing and lambda the
        // range seen.
        format_value(m, sizeof(m), gg.channel, s.max, true, g.units);
        if (gg.channel == kBoost) {
            snprintf(text, sizeof(text), "PEAK %s", m);
        } else {
            char lo[16];
            format_value(lo, sizeof(lo), gg.channel, s.min, true, g.units);
            snprintf(text, sizeof(text), "%s  -  %s", lo, m);
        }
    } else {
        text[0] = '\0';
    }
    set_text(gg.peak, gg.peak_text, sizeof(gg.peak_text), text);
    // The target mark.
    int t = -1;
    if (gg.target_channel >= 0 && fresh(in.samples[gg.target_channel], in.now_ms)) {
        t = angle_of(gg, in.samples[gg.target_channel].value);
    }
    if (t != gg.t0) {
        gg.t0 = t;
        if (t < 0) {
            lv_obj_set_hidden(gg.target, true);
        } else {
            lv_obj_set_hidden(gg.target, false);
            lv_arc_set_angles(gg.target, t > 1 ? t - 1 : 0, t + 1 < kSweep ? t + 1 : kSweep);
        }
    }
}

// --- tiles ---

void build_tile(Tile &t, lv_obj_t *parent, int x, int y, int w, int h, int channel) {
    memset(&t, 0, sizeof(t));
    t.channel = channel;
    t.bar_value = -1;
    t.show_peak = true;
    t.panel = plain(parent);
    lv_obj_set_size(t.panel, w, h);
    lv_obj_set_pos(t.panel, x, y);
    lv_obj_set_style_radius(t.panel, h / 10, 0);
    lv_obj_set_style_bg_opa(t.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t.panel, lv_color_hex(kPanel), 0);
    lv_obj_set_style_border_width(t.panel, 1, 0);
    lv_obj_set_style_border_color(t.panel, lv_color_hex(kPanelBorder), 0);
    const int inset = h / 9;
    t.name = label(t.panel, g.fonts.small, kTextSecondary, channel >= 0 ? channel_info(channel).label : "");
    lv_obj_set_style_text_letter_space(t.name, 2, 0);
    lv_obj_align(t.name, LV_ALIGN_TOP_LEFT, inset, inset);
    t.peak = label(t.panel, g.fonts.tiny, kTextDim, "");
    lv_obj_align(t.peak, LV_ALIGN_TOP_RIGHT, -inset, inset + 2);
    t.value = label(t.panel, g.fonts.big, kText, "--");
    lv_obj_align(t.value, LV_ALIGN_LEFT_MID, inset - 2, h / 12);
    t.unit = label(t.panel, g.fonts.mid, kTextSecondary, "");
    t.bar = lv_bar_create(t.panel);
    lv_obj_remove_style_all(t.bar);
    lv_obj_set_size(t.bar, w - 2 * inset, h / 28 > 3 ? h / 28 : 3);
    lv_obj_align(t.bar, LV_ALIGN_BOTTOM_MID, 0, -inset);
    lv_obj_set_style_bg_opa(t.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(t.bar, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_radius(t.bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(t.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(t.bar, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(t.bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(t.bar, 0, 1000);
    t.colour = kAccent;
}

void set_tile_channel(Tile &t, int channel) {
    if (t.channel == channel) {
        return;
    }
    t.channel = channel;
    t.text[0] = t.peak_text[0] = t.unit_text[0] = '\0';
    t.bar_value = -1;
    if (channel < 0) {
        lv_obj_set_hidden(t.panel, true);
        return;
    }
    lv_obj_set_hidden(t.panel, false);
    lv_label_set_text(t.name, channel_info(channel).label);
}

void update_tile(Tile &t, const UiInput &in) {
    if (t.channel < 0) {
        return;
    }
    const Sample &s = in.samples[t.channel];
    const bool ok = fresh(s, in.now_ms);
    char text[32];
    value_text(text, sizeof(text), t.channel, s.value, ok);
    const bool moved = strcmp(text, t.text) != 0;
    set_text(t.value, t.text, sizeof(t.text), text);
    const bool unit_moved = strcmp(t.unit_text, shown_unit(t.channel)) != 0;
    set_text(t.unit, t.unit_text, sizeof(t.unit_text), shown_unit(t.channel));
    if (moved || unit_moved) {
        lv_obj_align_to(t.unit, t.value, LV_ALIGN_OUT_RIGHT_BOTTOM, lv_obj_get_height(t.value) / 8,
                        -lv_obj_get_height(t.value) / 8);
    }
    const bool state = channel_info(t.channel).quantity == kQState;
    const uint32_t normal = state && s.value >= 0.5f ? kWarn : level_colour(t.channel, s.value, kText);
    set_colour(t.value, &t.colour, ok ? normal : kTextSecondary, 0, true);
    if (ok && t.show_peak && !state) {
        char m[16];
        value_text(m, sizeof(m), t.channel, s.max, true);
        snprintf(text, sizeof(text), "MAX %s", m);
    } else {
        text[0] = '\0';
    }
    set_text(t.peak, t.peak_text, sizeof(t.peak_text), text);
    const ChannelInfo &info = channel_info(t.channel);
    if (lv_obj_is_hidden(t.bar) != state) {
        lv_obj_set_hidden(t.bar, state);
    }
    const int bar = ok ? static_cast<int>(1000.0f * clampf((s.value - info.min) / (info.max - info.min), 0, 1)) : 0;
    if (bar != t.bar_value) {
        t.bar_value = bar;
        lv_bar_set_value(t.bar, bar, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(t.bar, lv_color_hex(ok ? level_colour(t.channel, s.value, kAccent) : kTrack),
                                  LV_PART_INDICATOR);
    }
}

// Tiles in `cols` x `rows` between x0 and x0 + width.
void build_grid_at(lv_obj_t *page, int x0, int width, int cols, int rows, const int *channels,
                   int count) {
    const int top = g.pad, h_all = g.height - g.bar_h - 2 * g.pad;
    const int w = (width - (cols - 1) * g.pad) / cols;
    const int h = (h_all - (rows - 1) * g.pad) / rows;
    for (int i = 0; i < count && g.tile_count < kMaxTiles; ++i) {
        const int c = i % cols, r = i / cols;
        build_tile(g.tiles[g.tile_count++], page, x0 + c * (w + g.pad), top + r * (h + g.pad), w,
                   h, channels[i]);
    }
}

void build_grid(lv_obj_t *page, int cols, int rows, const int *channels, int count) {
    build_grid_at(page, g.pad, g.width - 2 * g.pad, cols, rows, channels, count);
}

// --- the ignition page's cylinder bars ---

void build_cylinders(lv_obj_t *page, int x, int w) {
    const int top = g.pad, h = g.height - g.bar_h - 2 * g.pad;
    g.cyl_box = plain(page);
    lv_obj_set_pos(g.cyl_box, x, top);
    lv_obj_set_size(g.cyl_box, w, h);
    lv_obj_t *title = label(g.cyl_box, g.fonts.small, kTextSecondary, "CORRECTIONS");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    const int col_w = w / kCylinders;
    const int bar_h = h * 58 / 100;
    for (int i = 0; i < kCylinders; ++i) {
        CylBar &c = g.cyl[i];
        c.col = plain(g.cyl_box);
        lv_obj_set_size(c.col, col_w, h - h / 10);
        lv_obj_set_pos(c.col, i * col_w, h / 10);
        c.value = label(c.col, g.fonts.mid, kText, "--");
        lv_obj_align(c.value, LV_ALIGN_TOP_MID, 0, 0);
        c.bar = lv_bar_create(c.col);
        lv_obj_remove_style_all(c.bar);
        lv_obj_set_size(c.bar, col_w / 3, bar_h);
        lv_obj_align(c.bar, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_opa(c.bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kPanel), LV_PART_MAIN);
        lv_obj_set_style_border_width(c.bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(c.bar, lv_color_hex(kPanelBorder), LV_PART_MAIN);
        lv_obj_set_style_radius(c.bar, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c.bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kAccent), LV_PART_INDICATOR);
        lv_obj_set_style_radius(c.bar, 6, LV_PART_INDICATOR);
        lv_bar_set_mode(c.bar, LV_BAR_MODE_SYMMETRICAL);
        lv_bar_set_range(c.bar, -60, 10); // tenths of a degree: retard down
        lv_bar_set_value(c.bar, 0, LV_ANIM_OFF);
        c.name = label(c.col, g.fonts.small, kTextSecondary, channel_info(kCorr1 + i).label);
        lv_obj_align(c.name, LV_ALIGN_BOTTOM_MID, 0, 0);
        c.bar_value = 0;
        c.colour = kAccent;
    }
    g.cyl_note = label(page, g.fonts.mid, kTextSecondary,
                       "Per-cylinder corrections come\nfrom a definition file:\nsee the README.");
    lv_obj_set_style_text_line_space(g.cyl_note, 6, 0);
    lv_obj_set_pos(g.cyl_note, x, top + h / 3);
    lv_obj_set_hidden(g.cyl_note, true);
}

void update_cylinders(const UiInput &in) {
    int count = 0;
    for (int i = 0; i < kCylinders; ++i) {
        if (in.samples[kCorr1 + i].valid) {
            count = i + 1;
        }
    }
    if (!count) {
        lv_obj_set_hidden(g.cyl_box, true);
        lv_obj_set_hidden(g.cyl_note, false);
        return;
    }
    lv_obj_set_hidden(g.cyl_box, false);
    lv_obj_set_hidden(g.cyl_note, true);
    const int w = lv_obj_get_width(g.cyl_box);
    const int col_w = w / count;
    for (int i = 0; i < kCylinders; ++i) {
        CylBar &c = g.cyl[i];
        if (i >= count) {
            lv_obj_set_hidden(c.col, true);
            continue;
        }
        lv_obj_set_hidden(c.col, false);
        if (lv_obj_get_width(c.col) != col_w) {
            lv_obj_set_width(c.col, col_w);
            lv_obj_set_x(c.col, i * col_w);
        }
        const Sample &s = in.samples[kCorr1 + i];
        const bool ok = fresh(s, in.now_ms);
        char text[16];
        value_text(text, sizeof(text), kCorr1 + i, s.value, ok);
        set_text(c.value, c.text, sizeof(c.text), text);
        const int v = ok ? static_cast<int>(roundf(clampf(s.value, -6.0f, 1.0f) * 10.0f)) : 0;
        if (v != c.bar_value) {
            c.bar_value = v;
            lv_bar_set_value(c.bar, v, LV_ANIM_OFF);
        }
        const uint32_t colour = !ok ? kTextDim : s.value <= -3.0f ? kDanger : s.value <= -1.0f ? kWarn : kAccent;
        if (colour != c.colour) {
            c.colour = colour;
            lv_obj_set_style_bg_color(c.bar, lv_color_hex(colour), LV_PART_INDICATOR);
        }
    }
}

// --- MORE: custom values first, then whatever else answers ---

const int kMoreOrder[] = {kSpeed,     kTorque,    kVoltage,   kOilPressure, kFuelLow,
                          kLambdaTarget, kMap,    kBaro,      kTrimShort,   kTrimLong,
                          kPedal,     kCatalyst};

void update_more(const UiInput &in) {
    int chosen[kMoreTiles], n = 0;
    for (int c = kCustom1; c <= kCustomLast && n < kMoreTiles; ++c) {
        if (custom_used(c)) {
            chosen[n++] = c;
        }
    }
    for (size_t i = 0; i < sizeof(kMoreOrder) / sizeof(kMoreOrder[0]) && n < kMoreTiles; ++i) {
        if (in.samples[kMoreOrder[i]].valid) {
            chosen[n++] = kMoreOrder[i];
        }
    }
    for (int i = 0; i < kMoreTiles; ++i) {
        set_tile_channel(g.tiles[g.more_first + i], i < n ? chosen[i] : -1);
    }
}

// --- the IGNITION page on a diesel: DPF ---

void build_dpf(lv_obj_t *box, int gap, int y, int d) {
    build_gauge(g.gauges[4], box, gap, y, d, kDpfLoad, -1);
    g.dpf_first = g.tile_count;
    const int tiles[kDpfTiles] = {kDpfRegen, kDpfDelta, kDpfInTemp, kDpfOutTemp, kDpfAvgDistance, kDpfSince};
    build_grid_at(box, 2 * gap + d, g.width - 2 * gap - d - g.pad, 3, 2, tiles, kDpfTiles);
    for (int i = 0; i < kDpfTiles; ++i) {
        g.tiles[g.dpf_first + i].show_peak = false; // long names
    }
    // Regeneration: a word ("PASSIVE"), smaller than the numbers.
    lv_obj_set_style_text_font(g.tiles[g.dpf_first].value, g.fonts.large, 0);
}

void update_dpf(const UiInput &in) {
    update_gauge(g.gauges[4], in);
    // The last tile: the distance since the last regeneration, else the soot
    // mass, else the average time between regenerations.
    const int last = in.samples[kDpfSince].valid ? kDpfSince
                     : in.samples[kDpfSoot].valid ? kDpfSoot
                                                  : kDpfAvgTime;
    set_tile_channel(g.tiles[g.dpf_first + kDpfTiles - 1], last);
    for (int i = 0; i < kDpfTiles; ++i) {
        update_tile(g.tiles[g.dpf_first + i], in);
    }
}

bool diesel_seen(const UiInput &in) {
    const Sample &fuel = in.samples[kFuelType];
    if (fuel.valid && diesel_fuel(static_cast<int>(fuel.value + 0.5f))) {
        return true;
    }
    for (int c = kDpfLoad; c <= kDpfAsh; ++c) {
        if (in.samples[c].valid) {
            return true;
        }
    }
    return false;
}

const char *page_title(int page) {
    return page == Ui::kIgnition && g.diesel ? "DPF" : kPageNames[page];
}

// --- TRANSMISSION: the car from above, the gear, torque and power ---

const uint32_t kTyre = 0x0B0E12;
const uint32_t kTyreEdge = 0x2E3946;
const uint32_t kTread = 0x2A3644;
const uint32_t kSplitRear = 0x16607C; // the accent, darker: the rear's part
const uint32_t kPower = 0xA78BFA;     // the power curve

lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t fill,
                uint32_t border, int border_width) {
    lv_obj_t *o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    if (border_width) {
        lv_obj_set_style_border_width(o, border_width, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    }
    return o;
}

lv_obj_t *line(lv_obj_t *parent, lv_point_precise_t *ends, int x0, int y0, int x1, int y1,
               int width, uint32_t colour, lv_opa_t opa) {
    ends[0].x = x0;
    ends[0].y = y0;
    ends[1].x = x1;
    ends[1].y = y1;
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_clickable(l, false);
    lv_line_set_points(l, ends, 2);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

lv_obj_t *small_title(lv_obj_t *page, int x, int y, const char *text) {
    lv_obj_t *t = label(page, g.fonts.tiny, kTextSecondary, text);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_set_pos(t, x, y);
    return t;
}

lv_obj_t *rule(lv_obj_t *page, int x, int y, int w) {
    lv_obj_t *r = plain(page);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(kPanelBorder), 0);
    return r;
}

// The car: its image (drawn once), the wheels over it, the drivetrain over
// both (each axle brighter with more of the torque).
void build_car(lv_obj_t *page, int length) {
    TransmissionView &t = g.tm;
    t.geo = car_geometry(length);
    const CarGeometry &geo = t.geo;
    const int content = g.height - g.bar_h;
    t.car_x = g.width / 2 - geo.width / 2;
    t.car_y = (content - geo.height) / 2;
    t.car_buf = lv_draw_buf_create(static_cast<uint32_t>(geo.width), static_cast<uint32_t>(geo.height),
                                   LV_COLOR_FORMAT_ARGB8888, 0);
    if (t.car_buf) {
        car_draw(geo, t.car_buf->data, static_cast<int>(t.car_buf->header.stride));
        t.car = lv_canvas_create(page);
        lv_canvas_set_draw_buf(t.car, t.car_buf);
        lv_obj_set_pos(t.car, t.car_x, t.car_y);
    }
    const int mid = t.car_x + geo.mid_x;
    const int yf = t.car_y + geo.front_axle, yr = t.car_y + geo.rear_axle;
    const int lw = length / 110 > 2 ? length / 110 : 2;
    // The drivetrain: gearbox, the shaft to the rear differential, the rear
    // half shafts; with xDrive the transfer case's shaft to the front
    // differential beside the engine, and the front half shafts.
    const int gb_w = geo.front_tyre_width;
    t.gearbox = shape(page, mid - gb_w / 2, t.car_y + geo.gearbox_top, gb_w,
                      geo.gearbox_bottom - geo.gearbox_top, gb_w / 3, kBackground, kAccent, 2);
    lv_obj_set_style_bg_opa(t.gearbox, 150, 0);
    const int half = geo.track - geo.rear_tyre_width / 2;
    const int dd = lw * 4;
    t.rear_shaft = line(page, t.tm_points[0], mid, t.car_y + geo.gearbox_bottom, mid, yr, lw, kAccent, 200);
    t.rear_axle = line(page, t.tm_points[1], mid - half, yr, mid + half, yr, lw, kAccent, 200);
    t.rear_diff = shape(page, mid - dd / 2, yr - dd / 2, dd, dd, dd / 4, kAccent, 0, 0);
    const int fx = mid + gb_w / 2 + lw; // the front shaft runs beside the engine
    const int fhalf = geo.track - geo.front_tyre_width / 2;
    t.front_shaft = line(page, t.tm_points[2], fx, t.car_y + geo.gearbox_bottom - lw * 2, fx, yf, lw, kAccent, 200);
    t.front_axle = line(page, t.tm_points[3], mid - fhalf, yf, mid + fhalf, yf, lw, kAccent, 200);
    t.front_diff = shape(page, fx - dd / 2, yf - dd / 2, dd, dd, dd / 4, kAccent, 0, 0);
    t.transfer = shape(page, mid - gb_w / 2, t.car_y + geo.gearbox_bottom - lw * 4, gb_w + lw * 2, lw * 4,
                       lw, kBackground, kAccent, 2);
    t.front_opa = t.rear_opa = -1;
    t.xdrive = -1;
    // The wheels, the front ones steering; tread across each.
    for (int i = 0; i < 4; ++i) {
        WheelView &wv = t.wheels[i];
        wv.right = i % 2 == 1;
        const int ww = i < 2 ? geo.front_tyre_width : geo.rear_tyre_width, wh = geo.tyre_length;
        const int wx = mid + (wv.right ? geo.track : -geo.track) - ww / 2;
        const int wy = (i < 2 ? yf : yr) - wh / 2;
        wv.tyre = shape(page, wx, wy, ww, wh, ww / 4, kTyre, kTyreEdge, 1);
        const int inset = ww / 6 > 2 ? ww / 6 : 2;
        wv.rim = shape(wv.tyre, inset - 1, inset - 1, ww - 2 * inset, wh - 2 * inset, ww / 6, kTread, 0, 0);
        for (int k = 1; k * 6 < wh - 2 * inset; ++k) {
            lv_obj_t *groove = shape(wv.rim, 0, k * 6, ww - 2 * inset, 1, 0, kTyre, 0, 0);
            lv_obj_set_style_bg_opa(groove, 170, 0);
        }
        lv_obj_set_style_transform_pivot_x(wv.tyre, ww / 2, 0);
        lv_obj_set_style_transform_pivot_y(wv.tyre, wh / 2, 0);
        wv.angle = 0;
        wv.colour = kTread;
    }
}

// A wheel's numbers: name, speed, slip, tyre pressure; beside the car.
void build_wheel_block(WheelView &wv, lv_obj_t *page, int x, int y_mid, int w, const char *name) {
    const int h = g.height * 30 / 100;
    wv.block = plain(page);
    lv_obj_set_size(wv.block, w, h);
    lv_obj_set_pos(wv.block, x, y_mid - h / 2);
    const lv_align_t side = wv.right ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT;
    wv.name = label(wv.block, g.fonts.tiny, kTextSecondary, name);
    lv_obj_set_style_text_letter_space(wv.name, 2, 0);
    lv_obj_align(wv.name, side, 0, 0);
    wv.speed = label(wv.block, g.fonts.large, kText, "--");
    wv.unit = label(wv.block, g.fonts.small, kTextSecondary, "");
    wv.slip = label(wv.block, g.fonts.small, kTextSecondary, "");
    wv.pressure = label(wv.block, g.fonts.tiny, kTextDim, "");
    lv_obj_align(wv.speed, side, 0, h * 14 / 100);
    lv_obj_align(wv.slip, side, 0, h * 55 / 100);
    lv_obj_align(wv.pressure, side, 0, h * 74 / 100);
    wv.slip_colour = kTextSecondary;
    wv.speed_text[0] = wv.unit_text[0] = wv.slip_text[0] = wv.pressure_text[0] = '\0';
}

// Torque and power against engine speed: the most seen every 100 rpm (a
// dyno sheet that fills as the engine runs through its range), and where the
// engine is now.
void build_dyno(lv_obj_t *page, int x, int w) {
    TransmissionView &t = g.tm;
    const int content = g.height - g.bar_h;
    small_title(page, x, g.pad, "TORQUE & POWER");
    const int ly = g.pad + g.height * 55 / 1000;
    shape(page, x, ly + 4, 10, 10, 2, kAccent, 0, 0);
    t.torque_legend = label(page, g.fonts.tiny, kTextSecondary, "");
    lv_obj_set_pos(t.torque_legend, x + 16, ly);
    shape(page, x + w / 2, ly + 4, 10, 10, 2, kPower, 0, 0);
    t.power_legend = label(page, g.fonts.tiny, kTextSecondary, "");
    lv_obj_set_pos(t.power_legend, x + w / 2 + 16, ly);
    const int margin = g.width * 3 / 100;
    t.chart_x = x + margin;
    t.chart_y = ly + g.height * 7 / 100;
    t.chart_w = w - 2 * margin;
    t.chart_h = content - t.chart_y - g.height * 21 / 100;
    t.chart = lv_chart_create(page);
    lv_obj_remove_style_all(t.chart);
    lv_obj_set_clickable(t.chart, false);
    lv_obj_set_pos(t.chart, t.chart_x, t.chart_y);
    lv_obj_set_size(t.chart, t.chart_w, t.chart_h);
    lv_chart_set_type(t.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(t.chart, kDynoBins);
    lv_chart_set_div_line_count(t.chart, 3, 7);
    lv_obj_set_style_line_color(t.chart, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_line_width(t.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(t.chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_line_width(t.chart, g.height / 150 > 2 ? g.height / 150 : 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(t.chart, true, LV_PART_ITEMS);
    lv_obj_set_style_width(t.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(t.chart, 0, LV_PART_INDICATOR);
    t.torque_series = lv_chart_add_series(t.chart, lv_color_hex(kAccent), LV_CHART_AXIS_PRIMARY_Y);
    t.power_series = lv_chart_add_series(t.chart, lv_color_hex(kPower), LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_values(t.chart, t.torque_series, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(t.chart, t.power_series, LV_CHART_POINT_NONE);
    // The axes' labels: torque on the left, power on the right, 0 to the top.
    for (int i = 0; i < 3; ++i) {
        const int y = t.chart_y + t.chart_h - (t.chart_h * i) / 2 - g.height * 17 / 1000;
        t.torque_labels[i] = label(page, g.fonts.tiny, kTextDim, "");
        lv_obj_set_width(t.torque_labels[i], margin - 6);
        lv_obj_set_style_text_align(t.torque_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(t.torque_labels[i], x, y);
        t.power_labels[i] = label(page, g.fonts.tiny, kTextDim, "");
        lv_obj_set_pos(t.power_labels[i], t.chart_x + t.chart_w + 6, y);
    }
    // Engine speed under the chart: thousands of rpm.
    for (int k = 1; k <= 7; ++k) {
        char text[12];
        snprintf(text, sizeof(text), "%d", k);
        lv_obj_t *l = label(page, g.fonts.tiny, kTextDim, "");
        lv_label_set_text(l, text);
        const int px = t.chart_x + (k * 1000 - kDynoFrom) * (t.chart_w - 1) / ((kDynoBins - 1) * kDynoStep);
        lv_obj_set_pos(l, px - 4, t.chart_y + t.chart_h + 4);
    }
    lv_obj_t *rpm = label(page, g.fonts.tiny, kTextDim, "x1000 RPM");
    lv_obj_set_pos(rpm, t.chart_x + t.chart_w - g.width * 7 / 100, t.chart_y + t.chart_h + g.height * 45 / 1000);
    // Where the engine is now: a line at its speed, a dot on it for each value.
    t.cursor = shape(page, 0, t.chart_y, g.height >= 720 ? 2 : 1, t.chart_h, 0, kTextDim, 0, 0);
    lv_obj_set_hidden(t.cursor, true);
    const int dot = g.height * 3 / 100;
    t.torque_dot = shape(page, 0, 0, dot, dot, LV_RADIUS_CIRCLE, kAccent, kBackground, 2);
    t.power_dot = shape(page, 0, 0, dot, dot, LV_RADIUS_CIRCLE, kPower, kBackground, 2);
    lv_obj_set_hidden(t.torque_dot, true);
    lv_obj_set_hidden(t.power_dot, true);
    t.now_text = label(page, g.fonts.small, kText, "");
    lv_obj_set_pos(t.now_text, x, content - g.pad - g.height * 95 / 1000);
    t.peak_text = label(page, g.fonts.tiny, kTextDim, "");
    lv_obj_set_pos(t.peak_text, x, content - g.pad - g.height * 4 / 100);
    for (int i = 0; i < kDynoBins; ++i) {
        t.torque_max[i] = -1.0f;
    }
    t.torque_top = t.power_top = 0;
    t.dyno_dirty = true;
    t.now_buf[0] = t.peak_buf[0] = '\0';
}

void build_gear_panel(lv_obj_t *page, int x, int w) {
    TransmissionView &t = g.tm;
    const int content = g.height - g.bar_h;
    small_title(page, x, g.pad, "GEAR");
    t.gear = label(page, g.fonts.huge, kText, "--");
    lv_obj_set_pos(t.gear, x - 4, g.pad + g.height * 3 / 100);
    t.mode = label(page, g.fonts.small, kAccent, "");
    lv_obj_set_width(t.mode, w);
    lv_obj_set_style_text_align(t.mode, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(t.mode, 2, 0);
    lv_obj_set_pos(t.mode, x, g.pad + g.height * 12 / 100);
    int y = g.pad + g.height * 25 / 100;
    rule(page, x, y, w);
    y += g.height * 25 / 1000;
    t.drive_title = small_title(page, x, y, "DRIVE");
    y += g.height * 55 / 1000;
    t.split_bar = lv_bar_create(page);
    lv_obj_remove_style_all(t.split_bar);
    lv_obj_set_size(t.split_bar, w, g.height / 40 > 8 ? g.height / 40 : 8);
    lv_obj_set_pos(t.split_bar, x, y);
    lv_obj_set_style_bg_opa(t.split_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(t.split_bar, lv_color_hex(kSplitRear), LV_PART_MAIN);
    lv_obj_set_style_radius(t.split_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(t.split_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(t.split_bar, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(t.split_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(t.split_bar, 0, 1000);
    t.split_value = -1;
    y += g.height * 35 / 1000;
    t.front_label = small_title(page, x, y, "FRONT");
    t.rear_label = small_title(page, x, y, "REAR");
    lv_obj_set_width(t.rear_label, w);
    lv_obj_set_style_text_align(t.rear_label, LV_TEXT_ALIGN_RIGHT, 0);
    t.rwd = label(page, g.fonts.small, kText, "REAR-WHEEL DRIVE");
    lv_obj_set_pos(t.rwd, x, y + g.height * 4 / 100);
    y += g.height * 4 / 100;
    t.front_value = label(page, g.fonts.large, kText, "--");
    lv_obj_set_pos(t.front_value, x, y);
    t.rear_value = label(page, g.fonts.large, kText, "--");
    lv_obj_set_width(t.rear_value, w);
    lv_obj_set_style_text_align(t.rear_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(t.rear_value, x, y);
    y += g.height * 105 / 1000;
    t.clutch = label(page, g.fonts.small, kTextSecondary, "");
    lv_obj_set_pos(t.clutch, x, y);
    y += g.height * 7 / 100;
    rule(page, x, y, w);
    y += g.height * 25 / 1000;
    t.gearbox_temp = label(page, g.fonts.small, kTextSecondary, "");
    lv_obj_set_pos(t.gearbox_temp, x, y);
    t.steering = label(page, g.fonts.small, kTextSecondary, "");
    lv_obj_set_pos(t.steering, x, y + g.height * 55 / 1000);
    lv_obj_set_hidden(t.rwd, true);
    t.note = label(page, g.fonts.tiny, kTextDim,
                   "Wheel speeds (DSC), the split (transfer\ncase) and the gearbox: from a definition\nfile, see the README.");
    lv_obj_set_style_text_line_space(t.note, 4, 0);
    lv_obj_set_pos(t.note, x, content - g.pad - g.height * 13 / 100);
    lv_obj_set_hidden(t.note, true);
    t.gear_buf[0] = t.mode_buf[0] = t.front_buf[0] = t.rear_buf[0] = t.clutch_buf[0] = '\0';
    t.gearbox_buf[0] = t.steering_buf[0] = '\0';
}

void build_transmission(lv_obj_t *page) {
    TransmissionView &t = g.tm;
    const int content = g.height - g.bar_h;
    build_car(page, content - 2 * g.pad);
    const int car_half = t.geo.width / 2;
    const int gap = g.pad * 3 / 4;
    const int block_w = g.width * 14 / 100;
    const int panel_w = (g.width - 2 * car_half - 2 * block_w - 4 * gap - 2 * g.pad) / 2;
    const int yf = t.car_y + t.geo.front_axle, yr = t.car_y + t.geo.rear_axle;
    const char *const names[4] = {"FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT"};
    for (int i = 0; i < 4; ++i) {
        WheelView &wv = t.wheels[i];
        const int x = wv.right ? g.width / 2 + car_half + gap : g.width / 2 - car_half - gap - block_w;
        build_wheel_block(wv, page, x, i < 2 ? yf : yr, block_w, names[i]);
    }
    build_dyno(page, g.pad, panel_w);
    build_gear_panel(page, g.width - g.pad - panel_w, panel_w);
    t.drivetrain = kDriveAuto;
    t.xdrive_seen = false;
}

uint32_t slip_colour(float slip) {
    const float a = fabsf(slip);
    return a >= channel_info(kSlipFL).danger ? kDanger : a >= channel_info(kSlipFL).warn ? kWarn : 0;
}

void update_wheels(const UiInput &in) {
    TransmissionView &t = g.tm;
    // The front wheels turn with the steering, smoothly: a tenth of the
    // steering wheel's angle (the car's ratio is about 15:1; a little more
    // shows better).
    const Sample &st = in.samples[kSteering];
    if (fresh(st, in.now_ms)) {
        if (!t.has_steer) {
            t.steer_shown = st.value;
            t.has_steer = true;
        }
        t.steer_shown += (st.value - t.steer_shown) * 0.35f;
    } else {
        t.has_steer = false;
        t.steer_shown = 0;
    }
    const int angle = static_cast<int>(roundf(clampf(-t.steer_shown / 10.0f, -35.0f, 35.0f) * 10.0f));
    for (int i = 0; i < 4; ++i) {
        WheelView &wv = t.wheels[i];
        if (i < 2 && abs(angle - wv.angle) >= 5) {
            wv.angle = angle;
            lv_obj_set_style_transform_rotation(wv.tyre, angle, 0);
        }
        const Sample &sp = in.samples[kWheelFL + i];
        const Sample &sl = in.samples[kSlipFL + i];
        const Sample &ty = in.samples[kTyreFL + i];
        const bool ok = fresh(sp, in.now_ms), slip_ok = fresh(sl, in.now_ms);
        char text[32];
        format_value(text, sizeof(text), kWheelFL + i, sp.value, ok, g.units);
        const bool moved = strcmp(text, wv.speed_text) != 0;
        set_text(wv.speed, wv.speed_text, sizeof(wv.speed_text), text);
        const bool unit_moved = strcmp(wv.unit_text, unit_text(kWheelFL + i, g.units)) != 0;
        set_text(wv.unit, wv.unit_text, sizeof(wv.unit_text), unit_text(kWheelFL + i, g.units));
        if (moved || unit_moved) {
            const int lift = lv_obj_get_height(wv.speed) / 8;
            if (wv.right) {
                lv_obj_align_to(wv.unit, wv.speed, LV_ALIGN_OUT_RIGHT_BOTTOM, lift, -lift);
            } else {
                lv_obj_update_layout(wv.unit);
                lv_obj_align(wv.speed, LV_ALIGN_TOP_RIGHT, -lv_obj_get_width(wv.unit) - lift,
                             lv_obj_get_height(wv.block) * 14 / 100);
                lv_obj_align_to(wv.unit, wv.speed, LV_ALIGN_OUT_RIGHT_BOTTOM, lift, -lift);
            }
        }
        if (slip_ok) {
            const float v = roundf(sl.value * 10.0f) / 10.0f;
            snprintf(text, sizeof(text), "SLIP %+.1f%%", v == 0.0f ? 0.0 : static_cast<double>(v));
        } else {
            snprintf(text, sizeof(text), "%s", ok ? "SLIP --" : "");
        }
        set_text(wv.slip, wv.slip_text, sizeof(wv.slip_text), text);
        const uint32_t level = slip_ok ? slip_colour(sl.value) : 0;
        set_colour(wv.slip, &wv.slip_colour, level ? level : kTextSecondary, 0, true);
        if (fresh(ty, in.now_ms)) {
            char p[16];
            format_value(p, sizeof(p), kTyreFL + i, ty.value, true, g.units);
            snprintf(text, sizeof(text), "TYRE %s %s", p, unit_text(kTyreFL + i, g.units));
        } else {
            text[0] = '\0';
        }
        set_text(wv.pressure, wv.pressure_text, sizeof(wv.pressure_text), text);
        // The tread: grey, amber or red with the wheel's slip.
        const uint32_t fill = level ? level : ok ? 0x3A4757 : kTread;
        if (fill != wv.colour) {
            wv.colour = fill;
            lv_obj_set_style_bg_color(wv.rim, lv_color_hex(fill), 0);
            lv_obj_set_style_border_color(wv.tyre, lv_color_hex(level ? level : kTyreEdge), 0);
        }
    }
}

// Whether the car is drawn with xDrive: the setting, or (automatic) once the
// transfer case or the split was seen.
bool xdrive_now(const UiInput &in) {
    TransmissionView &t = g.tm;
    if (in.samples[kFrontShare].valid || in.samples[kXdriveClutch].valid) {
        t.xdrive_seen = true;
    }
    return t.drivetrain == kDriveXdrive || (t.drivetrain == kDriveAuto && t.xdrive_seen);
}

void show_drivetrain(bool xdrive) {
    TransmissionView &t = g.tm;
    if (t.xdrive == (xdrive ? 1 : 0)) {
        return;
    }
    t.xdrive = xdrive ? 1 : 0;
    lv_obj_t *front[] = {t.front_shaft, t.front_axle, t.front_diff, t.transfer, t.front_label,
                         t.rear_label, t.front_value, t.rear_value, t.clutch};
    for (size_t i = 0; i < sizeof(front) / sizeof(front[0]); ++i) {
        lv_obj_set_hidden(front[i], !xdrive);
    }
    lv_obj_set_hidden(t.rwd, xdrive);
    lv_label_set_text(t.drive_title, xdrive ? "DRIVE  XDRIVE" : "DRIVE");
    t.split_value = -1;
    t.front_opa = t.rear_opa = -1;
}

void update_drive(const UiInput &in) {
    TransmissionView &t = g.tm;
    show_drivetrain(xdrive_now(in));
    const Sample &sh = in.samples[kFrontShare];
    const bool ok = t.xdrive && fresh(sh, in.now_ms);
    const float front = !t.xdrive ? 0.0f : ok ? clampf(sh.value, 0.0f, 100.0f) : 0.0f;
    char text[40];
    if (t.xdrive) {
        snprintf(text, sizeof(text), ok ? "%.0f%%" : "--", static_cast<double>(front));
        set_text(t.front_value, t.front_buf, sizeof(t.front_buf), text);
        snprintf(text, sizeof(text), ok ? "%.0f%%" : "--", static_cast<double>(100.0f - front));
        set_text(t.rear_value, t.rear_buf, sizeof(t.rear_buf), text);
    }
    const int bar = static_cast<int>(front * 10.0f);
    if (bar != t.split_value) {
        t.split_value = bar;
        lv_bar_set_value(t.split_bar, bar, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(t.split_bar, lv_color_hex(!t.xdrive || ok ? kSplitRear : kTrack), LV_PART_MAIN);
    }
    // The axles' brightness: their part of the torque (all of it at the rear
    // without xDrive).
    const float rear = 100.0f - front;
    const int fo = ok ? 60 + static_cast<int>(front * 1.95f) : 60;
    const int ro = !t.xdrive || ok ? 60 + static_cast<int>(rear * 1.95f) : 60;
    if (fo != t.front_opa || ro != t.rear_opa) {
        t.front_opa = fo;
        t.rear_opa = ro;
        lv_obj_set_style_line_opa(t.front_axle, static_cast<lv_opa_t>(fo), 0);
        lv_obj_set_style_line_opa(t.front_shaft, static_cast<lv_opa_t>(fo), 0);
        lv_obj_set_style_bg_opa(t.front_diff, static_cast<lv_opa_t>(fo), 0);
        lv_obj_set_style_line_opa(t.rear_axle, static_cast<lv_opa_t>(ro), 0);
        lv_obj_set_style_line_opa(t.rear_shaft, static_cast<lv_opa_t>(ro), 0);
        lv_obj_set_style_bg_opa(t.rear_diff, static_cast<lv_opa_t>(ro), 0);
    }
    const Sample &cl = in.samples[kXdriveClutch];
    if (t.xdrive && fresh(cl, in.now_ms)) {
        char v[16];
        format_value(v, sizeof(v), kXdriveClutch, cl.value, true, g.units);
        snprintf(text, sizeof(text), "CLUTCH  %s %s", v, unit_text(kXdriveClutch, g.units));
    } else {
        text[0] = '\0';
    }
    set_text(t.clutch, t.clutch_buf, sizeof(t.clutch_buf), text);
}

void update_gear(const UiInput &in) {
    TransmissionView &t = g.tm;
    const Sample &ge = in.samples[kGear];
    const Sample &mo = in.samples[kDriveMode];
    const bool mode_ok = fresh(mo, in.now_ms, 10000);
    char text[40];
    if (fresh(ge, in.now_ms, 10000)) {
        gear_text(text, sizeof(text), ge.value, mode_ok ? mo.value : -1.0f);
    } else {
        snprintf(text, sizeof(text), "--");
    }
    set_text(t.gear, t.gear_buf, sizeof(t.gear_buf), text);
    set_text(t.mode, t.mode_buf, sizeof(t.mode_buf), mode_ok ? state_text(kDriveMode, mo.value) : "");
    const Sample &gt = in.samples[kGearboxTemp];
    if (fresh(gt, in.now_ms)) {
        char v[16];
        format_value(v, sizeof(v), kGearboxTemp, gt.value, true, g.units);
        snprintf(text, sizeof(text), "GEARBOX OIL  %s %s", v, unit_text(kGearboxTemp, g.units));
    } else {
        text[0] = '\0';
    }
    set_text(t.gearbox_temp, t.gearbox_buf, sizeof(t.gearbox_buf), text);
    const Sample &st = in.samples[kSteering];
    if (fresh(st, in.now_ms)) {
        const float a = roundf(st.value);
        snprintf(text, sizeof(text), "STEERING  %.0f\xc2\xb0%s", fabs(static_cast<double>(a)),
                 a > 0 ? " L" : a < 0 ? " R" : "");
    } else {
        text[0] = '\0';
    }
    set_text(t.steering, t.steering_buf, sizeof(t.steering_buf), text);
    const bool nothing = !in.samples[kWheelFL].valid && !in.samples[kFrontShare].valid &&
                         !in.samples[kGearboxTemp].valid;
    if (lv_obj_is_hidden(t.note) != !nothing) {
        lv_obj_set_hidden(t.note, !nothing);
    }
}

// Torque and power in display units.
float torque_shown(float nm) {
    return to_display(kTorque, nm, g.units);
}

float power_shown(float nm, float rpm) {
    const float kw = nm * rpm / 9549.3f;
    return g.units.imperial ? kw * 1.341022f : kw;
}

float nice_top(float v, float step, float least) {
    const float top = ceilf(v * 1.12f / step) * step;
    return top < least ? least : top;
}

// Every frame, whatever the page: the most torque seen at this engine speed.
void dyno_record(const UiInput &in) {
    TransmissionView &t = g.tm;
    const Sample &rpm = in.samples[kRpm];
    const Sample &tq = in.samples[kTorque];
    if (!fresh(rpm, in.now_ms) || !fresh(tq, in.now_ms) || tq.value <= 0) {
        return;
    }
    const int bin = static_cast<int>(roundf((rpm.value - kDynoFrom) / kDynoStep));
    if (bin < 0 || bin >= kDynoBins) {
        return;
    }
    if (tq.value > t.torque_max[bin]) {
        t.torque_max[bin] = tq.value;
        t.dyno_dirty = true;
    }
}

void dyno_reset() {
    TransmissionView &t = g.tm;
    for (int i = 0; i < kDynoBins; ++i) {
        t.torque_max[i] = -1.0f;
    }
    t.dyno_dirty = true;
}

void update_dyno(const UiInput &in) {
    TransmissionView &t = g.tm;
    char text[80];
    if (t.dyno_dirty) {
        t.dyno_dirty = false;
        float tmax = 0, pmax = 0;
        int tbin = -1, pbin = -1;
        for (int i = 0; i < kDynoBins; ++i) {
            if (t.torque_max[i] < 0) {
                continue;
            }
            const float rpm = static_cast<float>(kDynoFrom + i * kDynoStep);
            const float tv = torque_shown(t.torque_max[i]), pv = power_shown(t.torque_max[i], rpm);
            if (tv > tmax) {
                tmax = tv;
                tbin = i;
            }
            if (pv > pmax) {
                pmax = pv;
                pbin = i;
            }
        }
        const float ttop = nice_top(tmax, 100.0f, 200.0f), ptop = nice_top(pmax, 50.0f, 100.0f);
        if (ttop != t.torque_top || ptop != t.power_top) {
            t.torque_top = ttop;
            t.power_top = ptop;
            lv_chart_set_axis_range(t.chart, LV_CHART_AXIS_PRIMARY_Y, 0, static_cast<int32_t>(ttop));
            lv_chart_set_axis_range(t.chart, LV_CHART_AXIS_SECONDARY_Y, 0, static_cast<int32_t>(ptop));
            for (int i = 0; i < 3; ++i) {
                snprintf(text, sizeof(text), "%.0f", static_cast<double>(ttop * static_cast<float>(i) / 2.0f));
                lv_label_set_text(t.torque_labels[i], text);
                snprintf(text, sizeof(text), "%.0f", static_cast<double>(ptop * static_cast<float>(i) / 2.0f));
                lv_label_set_text(t.power_labels[i], text);
            }
            snprintf(text, sizeof(text), "TORQUE  %s", unit_text(kTorque, g.units));
            lv_label_set_text(t.torque_legend, text);
            snprintf(text, sizeof(text), "POWER  %s", g.units.imperial ? "hp" : "kW");
            lv_label_set_text(t.power_legend, text);
        }
        for (int i = 0; i < kDynoBins; ++i) {
            const float rpm = static_cast<float>(kDynoFrom + i * kDynoStep);
            const bool have = t.torque_max[i] >= 0;
            lv_chart_set_series_value_by_id(t.chart, t.torque_series, static_cast<uint32_t>(i),
                                            have ? static_cast<int32_t>(roundf(torque_shown(t.torque_max[i])))
                                                 : LV_CHART_POINT_NONE);
            lv_chart_set_series_value_by_id(t.chart, t.power_series, static_cast<uint32_t>(i),
                                            have ? static_cast<int32_t>(roundf(power_shown(t.torque_max[i], rpm)))
                                                 : LV_CHART_POINT_NONE);
        }
        lv_chart_refresh(t.chart);
        if (tbin >= 0) {
            snprintf(text, sizeof(text), "MAX  %.0f %s at %d   %.0f %s at %d", static_cast<double>(tmax),
                     unit_text(kTorque, g.units), kDynoFrom + tbin * kDynoStep, static_cast<double>(pmax),
                     g.units.imperial ? "hp" : "kW", kDynoFrom + pbin * kDynoStep);
        } else {
            snprintf(text, sizeof(text), "%s", "The curves fill as the engine runs.");
        }
        set_text(t.peak_text, t.peak_buf, sizeof(t.peak_buf), text);
    }
    // Where the engine is now.
    const Sample &rpm = in.samples[kRpm];
    const Sample &tq = in.samples[kTorque];
    const bool ok = fresh(rpm, in.now_ms) && fresh(tq, in.now_ms) && t.torque_top > 0;
    if (ok) {
        const float tv = torque_shown(tq.value), pv = power_shown(tq.value, rpm.value);
        snprintf(text, sizeof(text), "%.0f %s   %.0f %s", static_cast<double>(tv), unit_text(kTorque, g.units),
                 static_cast<double>(pv), g.units.imperial ? "hp" : "kW");
        const float f = clampf((rpm.value - kDynoFrom) / static_cast<float>((kDynoBins - 1) * kDynoStep), 0, 1);
        const int dot = lv_obj_get_width(t.torque_dot);
        const int x = t.chart_x + static_cast<int>(f * static_cast<float>(t.chart_w - 1));
        const int th = static_cast<int>(clampf(tv / t.torque_top, 0, 1) * static_cast<float>(t.chart_h - 1));
        const int ph = static_cast<int>(clampf(pv / t.power_top, 0, 1) * static_cast<float>(t.chart_h - 1));
        const bool inside = rpm.value >= kDynoFrom;
        lv_obj_set_hidden(t.cursor, !inside);
        // A dot lying on the axis (overrun, idling) says nothing and covers the
        // other one: the line alone then.
        lv_obj_set_hidden(t.torque_dot, !inside || th < dot);
        lv_obj_set_hidden(t.power_dot, !inside || ph < dot);
        if (inside) {
            lv_obj_set_x(t.cursor, x);
            lv_obj_set_pos(t.torque_dot, x - dot / 2, t.chart_y + t.chart_h - 1 - th - dot / 2);
            lv_obj_set_pos(t.power_dot, x - dot / 2, t.chart_y + t.chart_h - 1 - ph - dot / 2);
        }
    } else {
        snprintf(text, sizeof(text), "%s", "--");
        lv_obj_set_hidden(t.cursor, true);
        lv_obj_set_hidden(t.torque_dot, true);
        lv_obj_set_hidden(t.power_dot, true);
    }
    set_text(t.now_text, t.now_buf, sizeof(t.now_buf), text);
}

void update_transmission(const UiInput &in) {
    update_wheels(in);
    update_drive(in);
    update_gear(in);
    update_dyno(in);
}

// --- CONNECTION ---

void build_info(lv_obj_t *page) {
    g.info = label(page, g.fonts.small, kText, "");
    lv_obj_set_style_text_line_space(g.info, g.height / 90, 0);
    lv_obj_set_pos(g.info, g.pad * 2, g.pad);
    lv_obj_set_width(g.info, g.width - 4 * g.pad);
    lv_label_set_long_mode(g.info, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_t *hint = label(page, g.fonts.tiny, kTextDim,
                           "TURN: PAGE    PUSH: RESET PEAKS    BACK: CLOSE");
    lv_obj_set_style_text_letter_space(hint, 1, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -g.pad * 2, -g.pad);
}

const char *mode_text(SourceMode mode) {
    switch (mode) {
    case kSourceAuto:
        return "automatic (DME, then iDrive car data)";
    case kSourceDme:
        return "DME only";
    case kSourceCarData:
        return "iDrive car data only";
    case kSourceDemo:
        return "demo";
    }
    return "";
}

void update_info(const UiInput &in) {
    char text[sizeof(g.info_text)];
    const DmeStatus &d = in.dme;
    char vin[32] = "";
    if (d.vin[0]) {
        const size_t n = strlen(d.vin);
        snprintf(vin, sizeof(vin), "   VIN ...%s", n > 7 ? d.vin + n - 7 : d.vin);
    }
    char dme[96];
    if (d.state == DmeStatus::kRunning) {
        snprintf(dme, sizeof(dme), "reading, %.0f per second%s", static_cast<double>(d.rate_hz),
                 d.batched ? ", batched" : "");
    } else {
        snprintf(dme, sizeof(dme), "%s", dme_state_text(d.state));
    }
    snprintf(text, sizeof(text),
             "#8b96a5 SOURCE#   %s\n"
             "#8b96a5 DME#   %s\n"
             "#8b96a5 GATEWAY#   %s%s   tester %02x\n"
             "#8b96a5 DME INDEX#   %s\n"
             "#8b96a5 STANDARD VALUES#   %s\n"
             "#8b96a5 DEFINITION FILE#   %d values, %d answering, %d refused%s%s\n"
             "#8b96a5 REQUESTS#   %u, %u answered, %u timed out   #8b96a5 CONNECTIONS#   %u\n"
             "#8b96a5 LAST PROBLEM#   %s\n"
             "#8b96a5 SEARCHED#   %s\n"
             "#8b96a5 IDRIVE CAR DATA#   %s\n"
             "#8b96a5 VERSION#   %s",
             mode_text(in.mode), dme, d.gateway[0] ? d.gateway : "-", vin, d.tester,
             d.index[0] ? d.index : "-", d.standard[0] ? d.standard : "-", in.defs_count,
             d.defined_ok, d.defined_failed, in.defs_errors ? "   " : "", in.defs_errors ? in.defs_error : "",
             d.requests, d.answers, d.timeouts, d.reconnects, d.error[0] ? d.error : "none",
             d.searched[0] ? d.searched : "-",
             !in.car_data ? "not granted" : in.car_data_fresh ? "receiving" : "granted, nothing yet",
             in.version);
    if (strcmp(text, g.info_text) != 0) {
        snprintf(g.info_text, sizeof(g.info_text), "%s", text);
        lv_label_set_recolor(g.info, true);
        lv_label_set_text(g.info, g.info_text);
    }
}

// --- the status in the top bar ---

void update_status(const UiInput &in) {
    char text[48];
    uint32_t colour = kTextDim;
    const DmeStatus &d = in.dme;
    if (in.mode == kSourceDemo) {
        snprintf(text, sizeof(text), "DEMO");
        colour = kAccent;
    } else if (d.state == DmeStatus::kRunning && in.mode != kSourceCarData) {
        snprintf(text, sizeof(text), "DME  %.0f HZ", static_cast<double>(d.rate_hz));
        colour = kGood;
    } else if (in.car_data && in.car_data_fresh && in.mode != kSourceDme) {
        snprintf(text, sizeof(text), "IDRIVE CAR DATA");
        colour = kWarn;
    } else if (in.mode == kSourceCarData) {
        snprintf(text, sizeof(text), in.car_data ? "WAITING FOR CAR DATA" : "NO CAR DATA");
    } else if (d.state == DmeStatus::kSearching) {
        snprintf(text, sizeof(text), "SEARCHING");
    } else {
        snprintf(text, sizeof(text), "CONNECTING");
        colour = kWarn;
    }
    if (strcmp(text, g.status_text) != 0 || colour != g.status_colour) {
        snprintf(g.status_text, sizeof(g.status_text), "%s", text);
        g.status_colour = colour;
        lv_label_set_text(g.status, g.status_text);
        lv_obj_update_layout(g.status);
        lv_obj_align_to(g.status_dot, g.status, LV_ALIGN_OUT_LEFT_MID, -g.pad / 2, 0);
        lv_obj_set_style_bg_color(g.status_dot, lv_color_hex(colour), 0);
    }
}

void log_print(lv_log_level_t level, const char *buf) {
    (void)level;
    // LVGL's lines end with a newline.
    char line[256];
    snprintf(line, sizeof(line), "%s", buf);
    const size_t n = strlen(line);
    if (n && line[n - 1] == '\n') {
        line[n - 1] = '\0';
    }
    nbt::app::log_info("lvgl: %s", line);
}

} // namespace

void ui_install_log() {
    lv_log_register_print_cb(&log_print);
}

Ui::Ui() : page_(kEngine), built_(false), last_slow_ms_(0) {}

bool Ui::init(int width, int height, const uint8_t *ttf, size_t ttf_size) {
    memset(&g, 0, sizeof(g));
    g.width = width;
    g.height = height;
    g.bar_h = height / 11;
    g.pad = height / 30;
    g.units.imperial = false;
    g.units.afr = false;
    g.boost_max = 200.0f;
    // Text sizes from the panel's height (480 on the 8.8" and 10.25" units).
    const int h = height;
    g.fonts.huge = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 17 / 100, LV_FONT_KERNING_NORMAL, 24);
    g.fonts.big = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 14 / 100, LV_FONT_KERNING_NORMAL, 24);
    g.fonts.large = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 9 / 100, LV_FONT_KERNING_NORMAL, 32);
    g.fonts.mid = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 55 / 1000, LV_FONT_KERNING_NORMAL, 64);
    g.fonts.small = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 42 / 1000, LV_FONT_KERNING_NORMAL, 96);
    g.fonts.tiny = lv_tiny_ttf_create_data_ex(ttf, ttf_size, h * 34 / 1000, LV_FONT_KERNING_NORMAL, 64);
    if (!g.fonts.huge || !g.fonts.big || !g.fonts.large || !g.fonts.mid || !g.fonts.small ||
        !g.fonts.tiny) {
        nbt::app::log_info("ui: no font");
        return false;
    }
    g.screen = lv_screen_active();
    lv_obj_remove_style_all(g.screen);
    lv_obj_set_scrollable(g.screen, false);
    lv_obj_set_style_bg_opa(g.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.screen, lv_color_hex(kBackground), 0);
    build_bar();
    for (int p = 0; p < kPageCount; ++p) {
        g.pages[p] = plain(g.screen);
        lv_obj_set_pos(g.pages[p], 0, g.bar_h);
        lv_obj_set_size(g.pages[p], width, height - g.bar_h);
        lv_obj_set_hidden(g.pages[p], true);
    }
    // ENGINE: three gauges as large as the page allows.
    const int content = height - g.bar_h;
    int d = (width - 4 * g.pad) / 3;
    if (d > content - 2 * g.pad) {
        d = content - 2 * g.pad;
    }
    const int gap = (width - 3 * d) / 4;
    const int y = (content - d) / 2;
    build_gauge(g.gauges[0], g.pages[kEngine], gap, y, d, kBoost, kBoostTarget);
    build_gauge(g.gauges[1], g.pages[kEngine], 2 * gap + d, y, d, kLambda, kLambdaTarget);
    build_gauge(g.gauges[2], g.pages[kEngine], 3 * gap + 2 * d, y, d, kTiming, -1);
    // FUEL & AIR and TEMPERATURES: six tiles each.
    const int fuel[] = {kRpm, kRail, kMaf, kThrottle, kLoad, kBoostTarget};
    build_grid(g.pages[kFuelAir], 3, 2, fuel, 6);
    const int temps[] = {kCoolant, kOil, kIntakeAir, kChargeAir, kAmbient, kCatalyst};
    build_grid(g.pages[kTemperatures], 3, 2, temps, 6);
    // IGNITION: timing, then the cylinders; on a diesel DPF instead.
    g.ign_box = plain(g.pages[kIgnition]);
    lv_obj_set_size(g.ign_box, width, content);
    build_gauge(g.gauges[3], g.ign_box, gap, y, d, kTiming, -1);
    build_cylinders(g.ign_box, 2 * gap + d, width - 3 * gap - d);
    g.dpf_box = plain(g.pages[kIgnition]);
    lv_obj_set_size(g.dpf_box, width, content);
    build_dpf(g.dpf_box, gap, y, d);
    lv_obj_set_hidden(g.dpf_box, true);
    // TRANSMISSION: the car.
    build_transmission(g.pages[kTransmission]);
    // MORE: eight tiles, filled as values come.
    g.more_first = g.tile_count;
    const int none[kMoreTiles] = {-1, -1, -1, -1, -1, -1, -1, -1};
    build_grid(g.pages[kMore], 4, 2, none, kMoreTiles);
    for (int i = 0; i < kMoreTiles; ++i) {
        g.tiles[g.more_first + i].channel = -2;
        g.tiles[g.more_first + i].show_peak = false;
        set_tile_channel(g.tiles[g.more_first + i], -1);
    }
    build_info(g.pages[kConnection]);
    built_ = true;
    configure(g.units, g.boost_max);
    show_page(page_);
    return true;
}

void Ui::configure(const Units &units, float boost_max) {
    g.units = units;
    g.boost_max = boost_max;
    if (!built_) {
        return;
    }
    for (int i = 0; i < kGauges; ++i) {
        configure_gauge(g.gauges[i]);
    }
    for (int i = 0; i < g.tile_count; ++i) {
        g.tiles[i].text[0] = g.tiles[i].unit_text[0] = g.tiles[i].peak_text[0] = '\0';
    }
}

void Ui::show_page(int page) {
    if (page < 0 || page >= kPageCount) {
        page = kEngine;
    }
    page_ = page;
    last_slow_ms_ = 0; // the slow parts of the new page at once
    if (!built_) {
        return;
    }
    for (int p = 0; p < kPageCount; ++p) {
        if (p == page) {
            lv_obj_set_hidden(g.pages[p], false);
        } else {
            lv_obj_set_hidden(g.pages[p], true);
        }
        lv_obj_set_style_bg_color(g.dots[p], lv_color_hex(p == page ? kText : kTrack), 0);
    }
    lv_label_set_text(g.title, page_title(page));
}

bool Ui::diesel() const {
    return g.diesel;
}

void Ui::set_drivetrain(Drivetrain drivetrain) {
    g.tm.drivetrain = drivetrain;
}

void Ui::reset_peaks() {
    if (built_) {
        dyno_reset();
    }
}

void Ui::turn(int detents) {
    if (!detents) {
        return;
    }
    int p = (page_ + detents) % kPageCount;
    if (p < 0) {
        p += kPageCount;
    }
    show_page(p);
}

void Ui::update(const UiInput &in) {
    if (!built_) {
        return;
    }
    update_status(in);
    dyno_record(in);
    if (!g.diesel && diesel_seen(in)) {
        g.diesel = true;
        lv_obj_set_hidden(g.ign_box, true);
        lv_obj_set_hidden(g.dpf_box, false);
        lv_label_set_text(g.title, page_title(page_));
        nbt::app::log_info("ui: a diesel: DPF in place of IGNITION");
    }
    switch (page_) {
    case kEngine:
        for (int i = 0; i < 3; ++i) {
            update_gauge(g.gauges[i], in);
        }
        break;
    case kIgnition:
        if (g.diesel) {
            update_dpf(in);
        } else {
            update_gauge(g.gauges[3], in);
            update_cylinders(in);
        }
        break;
    case kTransmission:
        update_transmission(in);
        break;
    case kMore:
        if (in.now_ms - last_slow_ms_ >= 1000) {
            last_slow_ms_ = in.now_ms;
            update_more(in);
        }
        for (int i = 0; i < kMoreTiles; ++i) {
            update_tile(g.tiles[g.more_first + i], in);
        }
        break;
    case kConnection:
        if (in.now_ms - last_slow_ms_ >= 500) {
            last_slow_ms_ = in.now_ms;
            update_info(in);
        }
        break;
    default: {
        // The tiles of FUEL & AIR (0-5) or TEMPERATURES (6-11).
        const int first = page_ == kFuelAir ? 0 : 6;
        for (int i = first; i < first + 6; ++i) {
            update_tile(g.tiles[i], in);
        }
        break;
    }
    }
}

} // namespace gauges
