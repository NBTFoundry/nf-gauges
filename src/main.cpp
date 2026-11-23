// SPDX-License-Identifier: MPL-2.0
// Gauges: live engine values on the iDrive screen. The DME through the car's
// gateway (dme.cpp), iDrive's own car data, or a simulated drive; LVGL draws
// the pages (ui.cpp) on the SDK's OpenGL ES 2 window (display_gl.cpp).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channels.hpp"
#include "defs.hpp"
#include "display_gl.hpp"
#include "dme.hpp"
#include "lvgl.h"
#include "nbt/app/car_data.hpp"
#include "nbt/app/clock.hpp"
#include "nbt/app/files.hpp"
#include "nbt/app/lifecycle.hpp"
#include "nbt/app/log.hpp"
#include "nbt/app/video.hpp"
#include "nbt/controller/input.hpp"
#include "sources.hpp"
#include "ui.hpp"
#include "units.hpp"

extern "C" {
extern const unsigned char gauges_font_ttf[];
extern const unsigned int gauges_font_ttf_size;
extern const char gauges_version[];
}

namespace {

using gauges::SourceMode;

const int kFrameMs = 33;

struct Config {
    gauges::Units units;
    float boost_max; // kPa, relative
    SourceMode mode;
    gauges::Drivetrain drivetrain;
    bool debug;
};

Config read_config(const nbt::app::Settings &s) {
    Config c;
    c.units.imperial = !strcmp(s.get("units", "metric"), "imperial");
    c.units.afr = !strcmp(s.get("lambda", "lambda"), "afr");
    const char *scale = s.get("boost_scale", "2bar");
    c.boost_max = !strcmp(scale, "1bar")     ? 100.0f
                  : !strcmp(scale, "1-5bar") ? 150.0f
                  : !strcmp(scale, "2-5bar") ? 250.0f
                  : !strcmp(scale, "3bar")   ? 300.0f
                                             : 200.0f;
    const char *drive = s.get("drivetrain", "auto");
    c.drivetrain = !strcmp(drive, "rwd")      ? gauges::kDriveRwd
                   : !strcmp(drive, "xdrive") ? gauges::kDriveXdrive
                                              : gauges::kDriveAuto;
    const char *source = s.get("source", "auto");
    c.mode = !strcmp(source, "dme")       ? gauges::kSourceDme
             : !strcmp(source, "cardata") ? gauges::kSourceCarData
             : !strcmp(source, "demo")    ? gauges::kSourceDemo
                                          : gauges::kSourceAuto;
    c.debug = !strcmp(s.get("debug_log", "off"), "on");
    return c;
}

// Every .csv file of the files folder, in name order.
void load_definitions(const char *files_dir, gauges::DefinitionSet *set) {
    gauges::definitions_clear(set);
    if (!files_dir || !*files_dir) {
        return;
    }
    static const char *const kExtensions[] = {"csv", NULL};
    nbt::app::FileList *list = new nbt::app::FileList;
    if (list->load(files_dir, kExtensions)) {
        for (size_t i = 0; i < list->count(); ++i) {
            size_t size = 0;
            void *data = list->read(i, 256 * 1024, &size);
            if (!data) {
                continue;
            }
            const int n = gauges::definitions_parse(set, list->at(i).name,
                                                    static_cast<const char *>(data), size);
            nbt::app::log_info("definitions: %s: %d values", list->at(i).name, n);
            free(data);
        }
    }
    delete list;
    for (int i = 0; i < set->error_count && i < gauges::DefinitionSet::kMaxErrors; ++i) {
        nbt::app::log_info("definitions: %s", set->errors[i]);
    }
}

// The page shown last, kept in the data folder.
int load_page(const char *data_dir) {
    char path[600];
    if (!data_dir || !*data_dir ||
        snprintf(path, sizeof(path), "%s/page", data_dir) >= static_cast<int>(sizeof(path))) {
        return 0;
    }
    FILE *f = fopen(path, "r");
    int page = 0;
    if (f) {
        if (fscanf(f, "%d", &page) != 1) {
            page = 0;
        }
        fclose(f);
    }
    return page >= 0 && page < gauges::Ui::kPageCount ? page : 0;
}

void save_page(const char *data_dir, int page) {
    char path[600];
    if (!data_dir || !*data_dir ||
        snprintf(path, sizeof(path), "%s/page", data_dir) >= static_cast<int>(sizeof(path))) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", page);
        fclose(f);
    }
}

struct Input {
    int turn;
    bool reset, leave;
};

void on_controller(void *context, const nbt_controller_wire_state &,
                   const nbt::controller::Event &event) {
    Input *in = static_cast<Input *>(context);
    in->turn += event.rotation;
    if (event.pressed & NBT_CONTROLLER_TILT_RIGHT) {
        ++in->turn;
    }
    if (event.pressed & NBT_CONTROLLER_TILT_LEFT) {
        --in->turn;
    }
    if (event.pressed & NBT_CONTROLLER_PUSH) {
        in->reset = true;
    }
    if (event.pressed & (NBT_CONTROLLER_BACK | NBT_CONTROLLER_MENU)) {
        in->leave = true;
    }
}

uint32_t tick_ms() {
    return static_cast<uint32_t>(nbt::app::monotonic_ms());
}

void status_text(char *out, size_t size, const gauges::DmeStatus &dme, SourceMode mode,
                 int car_fresh) {
    if (mode == gauges::kSourceDemo) {
        snprintf(out, size, "Demo");
    } else if (dme.state == gauges::DmeStatus::kRunning && mode != gauges::kSourceCarData) {
        snprintf(out, size, "DME %.0f Hz", static_cast<double>(dme.rate_hz));
    } else if (car_fresh && mode != gauges::kSourceDme) {
        snprintf(out, size, "iDrive car data");
    } else if (dme.state == gauges::DmeStatus::kSearching) {
        snprintf(out, size, "Searching");
    } else {
        snprintf(out, size, "Connecting");
    }
}

} // namespace

int main(int argc, char **argv) {
    nbt::app::log_init("gauges");
    nbt::app::Lifecycle lifecycle;
    const int started = lifecycle.init(argc, argv);
    if (started != 0) {
        return started;
    }
    nbt::app::log_install_crash_handlers();
    nbt::app::log_info("gauges %s, caps %s", gauges_version, lifecycle.bridge_caps());
    Config config = read_config(lifecycle.settings());
    nbt::app::log_set_debug(config.debug);

    nbt::app::VideoEgl video;
    const bool user = lifecycle.is_user_mode();
    if (user) {
        const nbt::app::VideoConfig window = {"Gauges", 1280, 480};
        if (!video.init(lifecycle.surface_id(), &window)) {
            nbt::app::log_info("no window: the car will not show it");
            return 1;
        }
    }
    // LVGL, the screens and one frame: also in a health check, without a
    // window, so the check covers them.
    nbt::app::log_stage("lvgl");
    lv_init();
    gauges::ui_install_log();
    lv_tick_set_cb(&tick_ms);
    const int width = user ? video.window_width() : 1280;
    const int height = user ? video.window_height() : 480;
    gauges::DisplayGl display;
    gauges::Ui ui;
    if (!display.init(width, height, user) ||
        !ui.init(width, height, gauges_font_ttf, gauges_font_ttf_size)) {
        nbt::app::log_info("the screens could not be built (%dx%d)", width, height);
        return 1;
    }
    ui.configure(config.units, config.boost_max);
    ui.set_drivetrain(config.drivetrain);
    ui.show_page(load_page(lifecycle.data_dir()));
    nbt::app::log_stage("first frame");
    lv_refr_now(NULL);
    if (display.take_frame() && user) {
        display.draw(width, height);
        video.swap();
    }
    nbt::app::log_info("screens ready, %dx%d", width, height);

    gauges::Store *store = new gauges::Store;
    gauges::DefinitionSet *defs = new gauges::DefinitionSet;
    load_definitions(lifecycle.files_dir(), defs);
    gauges::Dme dme(store);
    nbt::app::CarData car;
    const bool car_data = car.init();
    if (user) {
        dme.set_definitions(*defs);
        dme.set_debug(config.debug);
        dme.set_enabled(config.mode == gauges::kSourceAuto || config.mode == gauges::kSourceDme);
        // Desktop and tests: a fixed gateway, one more address to search, the
        // diagnostic port of the one found (the car's: 6801).
        const char *port = getenv("NBT_GAUGES_DIAG_PORT");
        dme.start(getenv("NBT_GAUGES_GATEWAY"), getenv("NBT_GAUGES_DISCOVERY"),
                  port ? atoi(port) : 0);
    }
    if (!lifecycle.send_ready()) {
        return 1;
    }
    nbt::app::log_stage("running");

    nbt::controller::Input controller;
    gauges::Sample *samples = new gauges::Sample[gauges::kChannelCount];
    const uint64_t start = nbt::app::monotonic_ms();
    uint64_t last_car = 0, car_fresh_until = 0;
    int result = 0;
    while (!lifecycle.stopping()) {
        const uint64_t frame_start = nbt::app::monotonic_ms();
        Input in = {0, false, false};
        if (lifecycle.controller_fd() >= 0) {
            controller.poll(lifecycle.controller_fd(), &on_controller, &in);
        }
        nbt::app::HostEvent ev;
        while (user && video.poll_event(&ev)) {
            if (ev.type == nbt::app::HostEvent::kClose) {
                in.leave = true;
            } else if (ev.type == nbt::app::HostEvent::kKeyDown) {
                if (ev.key == nbt::app::kHostKeyRight || ev.key == nbt::app::kHostKeyDown) {
                    ++in.turn;
                } else if (ev.key == nbt::app::kHostKeyLeft || ev.key == nbt::app::kHostKeyUp) {
                    --in.turn;
                } else if (ev.key == nbt::app::kHostKeyReturn || ev.key == ' ') {
                    in.reset = true;
                } else if (ev.key == nbt::app::kHostKeyEscape) {
                    in.leave = true;
                }
            }
        }
        if (in.leave) {
            nbt::app::log_info("BACK: closing");
            break;
        }
        if (in.turn) {
            ui.turn(in.turn);
            save_page(lifecycle.data_dir(), ui.page());
        }
        if (in.reset) {
            store->reset_peaks();
            ui.reset_peaks();
            nbt::app::log_info("peaks reset");
        }
        const uint64_t now = nbt::app::monotonic_ms();
        // The sources.
        if (config.mode == gauges::kSourceDemo) {
            gauges::demo_values(now - start, store, now);
        } else if (car_data && config.mode != gauges::kSourceDme && now - last_car >= 200) {
            last_car = now;
            if (gauges::poll_car_data(car, store, now) > 0) {
                car_fresh_until = now + 5000;
            }
        }
        gauges::DmeStatus dme_status;
        dme.status(&dme_status);
        char status[32];
        status_text(status, sizeof(status), dme_status, config.mode, now < car_fresh_until);
        bool changed = false;
        lifecycle.tick(now, status, &changed);
        if (changed) {
            const SourceMode was = config.mode;
            config = read_config(lifecycle.settings());
            nbt::app::log_set_debug(config.debug);
            dme.set_debug(config.debug);
            dme.set_enabled(config.mode == gauges::kSourceAuto ||
                            config.mode == gauges::kSourceDme);
            ui.configure(config.units, config.boost_max);
            ui.set_drivetrain(config.drivetrain);
            if (was == gauges::kSourceDemo && config.mode != was) {
                store->drop(gauges::kDemo);
            }
            nbt::app::log_info("settings changed");
        }
        (void)lifecycle.consume_show_requested();
        if (user) {
            store->snapshot(samples);
            gauges::UiInput ui_in;
            ui_in.samples = samples;
            ui_in.now_ms = now;
            ui_in.mode = config.mode;
            ui_in.dme = dme_status;
            ui_in.car_data = car_data;
            ui_in.car_data_fresh = now < car_fresh_until;
            ui_in.defs_count = defs->count;
            ui_in.defs_errors = defs->error_count;
            ui_in.defs_error = defs->error_count ? defs->errors[0] : "";
            ui_in.version = gauges_version;
            ui.update(ui_in);
            lv_timer_handler();
            if (display.take_frame()) {
                display.draw(width, height);
                video.swap();
            }
        }
        const uint64_t spent = nbt::app::monotonic_ms() - frame_start;
        usleep(static_cast<useconds_t>((spent < static_cast<uint64_t>(kFrameMs) ? kFrameMs - spent : 1) * 1000));
    }
    nbt::app::log_stage("closing");
    dme.stop();
    delete[] samples;
    delete defs;
    display.shutdown();
    video.shutdown();
    delete store;
    lifecycle.close_fds();
    return result;
}
