// SPDX-License-Identifier: MPL-2.0
// Desktop tool, without a window: every page with the simulated drive as a
// PNG (make shots: build/shots/<page>.png), or one page as a seamless loop of
// raw frames for the README's animations (make anims: docs/screens/<page>.webp).
//   gauges-shot <dir> [width height] [t_ms] [imperial] [afr]
//   gauges-shot --frames <page> [width height] [fps] > frames.bgr0
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channels.hpp"
#include "display_gl.hpp"
#include "dme.hpp"
#include "lvgl.h"
#include "sources.hpp"
#include "ui.hpp"

extern "C" {
extern const unsigned char gauges_font_ttf[];
extern const unsigned int gauges_font_ttf_size;
}

namespace {

uint32_t g_now = 0;
uint32_t tick() {
    return g_now;
}

uint32_t crc_table[256];

void crc_init() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

void put32(FILE *f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    fwrite(b, 1, 4, f);
}

void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    put32(f, static_cast<uint32_t>(n));
    fwrite(type, 1, 4, f);
    if (n) {
        fwrite(data, 1, n, f);
    }
    uint32_t c = crc(0xFFFFFFFFu, reinterpret_cast<const uint8_t *>(type), 4);
    c = crc(c, data, n);
    put32(f, c ^ 0xFFFFFFFFu);
}

// An RGB PNG with stored (uncompressed) deflate blocks.
bool write_png(const char *path, const uint8_t *bgrx, int w, int h, int stride) {
    const size_t row = 1 + static_cast<size_t>(w) * 3;
    const size_t raw_size = row * static_cast<size_t>(h);
    uint8_t *raw = static_cast<uint8_t *>(malloc(raw_size));
    const size_t blocks = (raw_size + 65534) / 65535;
    uint8_t *z = static_cast<uint8_t *>(malloc(raw_size + blocks * 5 + 6));
    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }
    for (int y = 0; y < h; ++y) {
        uint8_t *o = raw + row * static_cast<size_t>(y);
        const uint8_t *in = bgrx + static_cast<size_t>(y) * static_cast<size_t>(stride);
        *o++ = 0;
        for (int x = 0; x < w; ++x) {
            o[0] = in[2];
            o[1] = in[1];
            o[2] = in[0];
            o += 3;
            in += 4;
        }
    }
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_size; off += 65535) {
        const size_t len = raw_size - off < 65535 ? raw_size - off : 65535;
        z[zn++] = off + len == raw_size ? 1 : 0;
        z[zn++] = static_cast<uint8_t>(len);
        z[zn++] = static_cast<uint8_t>(len >> 8);
        z[zn++] = static_cast<uint8_t>(~len);
        z[zn++] = static_cast<uint8_t>(~len >> 8);
        memcpy(z + zn, raw + off, len);
        zn += len;
    }
    for (size_t i = 0; i < raw_size; ++i) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = b << 16 | a;
    z[zn++] = static_cast<uint8_t>(adler >> 24);
    z[zn++] = static_cast<uint8_t>(adler >> 16);
    z[zn++] = static_cast<uint8_t>(adler >> 8);
    z[zn++] = static_cast<uint8_t>(adler);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw);
        free(z);
        return false;
    }
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(kSignature, 1, 8, f);
    uint8_t ihdr[13] = {0};
    ihdr[0] = static_cast<uint8_t>(w >> 24);
    ihdr[1] = static_cast<uint8_t>(w >> 16);
    ihdr[2] = static_cast<uint8_t>(w >> 8);
    ihdr[3] = static_cast<uint8_t>(w);
    ihdr[4] = static_cast<uint8_t>(h >> 24);
    ihdr[5] = static_cast<uint8_t>(h >> 16);
    ihdr[6] = static_cast<uint8_t>(h >> 8);
    ihdr[7] = static_cast<uint8_t>(h);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // RGB
    chunk(f, "IHDR", ihdr, sizeof(ihdr));
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    const bool ok = fclose(f) == 0;
    free(raw);
    free(z);
    return ok;
}

const char *const kFiles[gauges::Ui::kPageCount] = {"engine",       "fuel-air", "temperatures", "ignition",
                                                    "transmission", "more",     "connection"};
const char kDpfFile[] = "dpf"; // IGNITION on a diesel

const int kSimFps = 30;           // the application's rate: the arcs' smoothing is per frame
const uint64_t kDriveMs = 24000;  // the demo drive's loop
const uint64_t kDriveFrom = 3000; // a loop starts just before the pull
const uint64_t kStartMs = 12000;  // the CONNECTION page's loop: from the search to reading
const uint64_t kClockBase = 100000;

// Everything one picture needs.
struct Scene {
    int width, height;
    gauges::DisplayGl display;
    gauges::Ui ui;
    gauges::Store store;
    gauges::Sample samples[gauges::kChannelCount];
    gauges::UiInput in;
    bool diesel; // the drive's diesel values too: IGNITION is then DPF
};

void copy(char *to, size_t size, const char *text) {
    snprintf(to, size, "%s", text);
}

// The DME read for a while: the top bar of every page, the CONNECTION still.
void dme_reading(gauges::DmeStatus *d) {
    memset(d, 0, sizeof(*d));
    d->state = gauges::DmeStatus::kRunning;
    d->rate_hz = 23.6f;
    d->batched = true;
    d->tester = 0xF4;
    copy(d->gateway, sizeof(d->gateway), "169.254.88.10");
    copy(d->vin, sizeof(d->vin), "WBA0000000F000000");
    copy(d->index, sizeof(d->index), "00 00 2b 01");
    copy(d->standard, sizeof(d->standard), "04 05 0b 0c 0d 0e 0f 11 23 33 34 42 44 46 49 5c");
    d->standard_count = 16;
    d->requests = 18240;
    d->answers = 18236;
    d->timeouts = 4;
    d->reconnects = 1;
    copy(d->searched, sizeof(d->searched), "169.254.255.255 192.168.1.255 255.255.255.255");
}

// The application starting, `ms` in: the search, the gateway, the DME's
// identification, then reading (iDrive's car data meanwhile).
void dme_starting(uint64_t ms, gauges::DmeStatus *d) {
    gauges::DmeStatus reading;
    dme_reading(&reading);
    memset(d, 0, sizeof(*d));
    d->tester = 0xF4;
    d->state = gauges::DmeStatus::kSearching;
    if (ms >= 400) {
        copy(d->searched, sizeof(d->searched), reading.searched);
    }
    if (ms < 2200) {
        return;
    }
    d->state = gauges::DmeStatus::kConnecting;
    copy(d->gateway, sizeof(d->gateway), reading.gateway);
    copy(d->vin, sizeof(d->vin), reading.vin);
    if (ms < 2900) {
        return;
    }
    d->state = gauges::DmeStatus::kIdentifying;
    d->reconnects = 1;
    d->requests = d->answers = static_cast<uint32_t>(1 + (ms - 2900) / 150);
    if (ms >= 3200) {
        copy(d->index, sizeof(d->index), reading.index);
    }
    if (ms >= 3900) {
        copy(d->standard, sizeof(d->standard), reading.standard);
        d->standard_count = reading.standard_count;
    }
    if (ms < 4600) {
        return;
    }
    d->state = gauges::DmeStatus::kRunning;
    d->batched = true;
    d->rate_hz = 23.6f + 0.5f * sinf(static_cast<float>(ms) / 1700.0f);
    d->requests = d->answers = static_cast<uint32_t>(12 + (ms - 4600) * 25 / 1000);
}

Scene *scene_new(int width, int height, const gauges::Units &units) {
    lv_init();
    gauges::ui_install_log();
    lv_tick_set_cb(&tick);
    Scene *s = new Scene;
    s->diesel = false;
    s->width = width;
    s->height = height;
    if (!s->display.init(width, height, false) ||
        !s->ui.init(width, height, gauges_font_ttf, gauges_font_ttf_size)) {
        fprintf(stderr, "no display\n");
        delete s;
        return NULL;
    }
    s->ui.configure(units, 200.0f);
    memset(&s->in, 0, sizeof(s->in));
    s->in.mode = gauges::kSourceAuto;
    dme_reading(&s->in.dme);
    s->in.car_data = true;
    s->in.car_data_fresh = 1;
    s->in.version = "0.0.1"; // not the build's: the pictures stay the same from commit to commit
    s->in.defs_error = "";
    s->in.samples = s->samples;
    return s;
}

// One application frame: the drive at `drive_ms`, the clock at `now`.
void step(Scene *s, uint64_t drive_ms, uint64_t now) {
    g_now = static_cast<uint32_t>(now);
    gauges::demo_values(drive_ms, &s->store, now, s->diesel);
    s->store.snapshot(s->samples);
    s->in.now_ms = now;
    s->ui.update(s->in);
}

bool emit(Scene *s, FILE *out) {
    lv_refr_now(NULL);
    const uint8_t *p = s->display.pixels();
    for (int y = 0; y < s->height; ++y) {
        const uint8_t *row = p + static_cast<size_t>(y) * static_cast<size_t>(s->display.stride());
        if (fwrite(row, 4, static_cast<size_t>(s->width), out) != static_cast<size_t>(s->width)) {
            return false;
        }
    }
    return true;
}

uint64_t frame_ms(uint64_t frame) {
    return frame * 1000u / kSimFps;
}

// One page as a loop of raw frames (bgr0) on stdout: the drive, or for
// CONNECTION the application starting.
int frames(Scene *s, int page, int fps) {
    const uint64_t every = static_cast<uint64_t>(kSimFps / fps);
    s->ui.show_page(page);
    int written = 0;
    if (page == gauges::Ui::kConnection) {
        for (uint64_t f = 0; frame_ms(f) < kStartMs; ++f) {
            dme_starting(frame_ms(f), &s->in.dme);
            step(s, kDriveFrom + frame_ms(f), kClockBase + frame_ms(f));
            if (f % every == 0) {
                if (!emit(s, stdout)) {
                    return 1;
                }
                ++written;
            }
        }
    } else {
        // A loop first, so the arcs and the peaks are where the loop leaves
        // them: the last frame then leads into the first.
        const uint64_t loop = kDriveMs * kSimFps / 1000u;
        for (uint64_t f = 0; f < 2 * loop; ++f) {
            const uint64_t ms = frame_ms(f);
            step(s, kDriveFrom + ms, kClockBase + ms);
            if (f >= loop && f % every == 0) {
                if (!emit(s, stdout)) {
                    return 1;
                }
                ++written;
            }
        }
    }
    fprintf(stderr, "%s: %d frames, %dx%d, %d fps\n", s->diesel ? kDpfFile : kFiles[page], written,
            s->width, s->height, fps);
    return fflush(stdout) == 0 ? 0 : 1;
}

// Every page as a PNG, the drive at `t` ms; then DPF (the diesel stays).
int stills(Scene *s, const char *dir, uint32_t t) {
    crc_init();
    for (int n = 0; n <= gauges::Ui::kPageCount; ++n) {
        const int p = n < gauges::Ui::kPageCount ? n : static_cast<int>(gauges::Ui::kIgnition);
        s->diesel = n == gauges::Ui::kPageCount;
        s->ui.show_page(p);
        // A few frames of the drive so the arcs settle where the values are.
        for (int f = 0; f < 30; ++f) {
            const uint64_t now = t - 30u * 33u + static_cast<uint32_t>(f) * 33u;
            step(s, now, now);
        }
        lv_refr_now(NULL);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.png", dir, s->diesel ? kDpfFile : kFiles[p]);
        if (!write_png(path, s->display.pixels(), s->width, s->height, s->display.stride())) {
            fprintf(stderr, "%s: not written\n", path);
            return 1;
        }
        printf("%s\n", path);
    }
    return 0;
}

int usage(const char *self) {
    fprintf(stderr,
            "usage: %s <dir> [width height] [t_ms] [imperial] [afr] [rwd]\n"
            "       %s --frames <page> [width height] [fps] [imperial] [afr] [rwd] > frames.bgr0\n"
            "pages: engine fuel-air temperatures ignition dpf transmission more connection;\n"
            "fps: 10, 15 or 30\n",
            self, self);
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const bool frame_mode = !strcmp(argv[1], "--frames");
    if (frame_mode && argc < 3) {
        return usage(argv[0]);
    }
    const int first = frame_mode ? 3 : 2; // width, height, then t_ms or fps
    const int width = argc > first + 1 ? atoi(argv[first]) : 1280;
    const int height = argc > first + 1 ? atoi(argv[first + 1]) : 480;
    const int number = argc > first + 2 ? atoi(argv[first + 2]) : frame_mode ? 15 : 9200;
    gauges::Units units;
    units.imperial = false;
    units.afr = false;
    bool rwd = false;
    for (int i = first + 3; i < argc; ++i) {
        units.imperial = units.imperial || !strcmp(argv[i], "imperial");
        units.afr = units.afr || !strcmp(argv[i], "afr");
        rwd = rwd || !strcmp(argv[i], "rwd");
    }
    int page = -1;
    const bool diesel = frame_mode && !strcmp(argv[2], kDpfFile);
    if (frame_mode) {
        for (int p = 0; p < gauges::Ui::kPageCount; ++p) {
            if (!strcmp(argv[2], kFiles[p])) {
                page = p;
            }
        }
        if (diesel) {
            page = gauges::Ui::kIgnition;
        }
        if (page < 0 || number <= 0 || kSimFps % number != 0) {
            return usage(argv[0]);
        }
    }
    Scene *s = scene_new(width, height, units);
    if (!s) {
        return 1;
    }
    s->diesel = diesel;
    if (rwd) {
        s->ui.set_drivetrain(gauges::kDriveRwd);
    }
    const int rc =
        frame_mode ? frames(s, page, number) : stills(s, argv[1], static_cast<uint32_t>(number));
    s->display.shutdown();
    delete s;
    return rc;
}
