/* SPDX-License-Identifier: MPL-2.0 */
/* LVGL's configuration for the gauges: only what differs from LVGL's own
 * defaults (lv_conf_internal.h fills in the rest). Software rendering into a
 * 32-bit buffer that src/display_gl.cpp shows through OpenGL ES 2; text from
 * Montserrat Medium (LVGL's copy) rasterised at any size by Tiny TTF. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888

/* The C library's allocator and printf; LVGL's own string functions (the
 * car's C library has no strnlen). */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

/* Rounded corners, arcs and anti-aliased lines. */
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE

/* LVGL's warnings reach the application's log (lv_log_register_print_cb). */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

/* Text: built-in Montserrat 14 as the fallback, Tiny TTF for the rest. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 96

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/* Nothing of LVGL's own demos and examples. */
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
