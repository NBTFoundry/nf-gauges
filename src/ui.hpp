// SPDX-License-Identifier: MPL-2.0
// The screens, in LVGL: a top bar (page name, page dots, where the data
// comes from), then one page at a time:
//   ENGINE        boost, lambda and timing as round gauges
//   FUEL & AIR    rpm, rail pressure, air mass, throttle, load, boost target
//   TEMPERATURES  coolant, oil, intake air, charge air, ambient, catalyst
//   IGNITION      timing and the per-cylinder corrections; on a diesel DPF:
//                 the particulate filter's load, regeneration, temperatures
//   TRANSMISSION  the car from above: wheel speeds and slip, the drivetrain
//                 (rear-wheel drive or xDrive, its front/rear split), the
//                 steering; the gear and drive mode; torque and power curves
//   MORE          custom values and whatever else the car gives
//   CONNECTION    the gateway, the DME, what answers, what failed
// The iDrive controller turns the pages and its push resets the peaks.
#ifndef GAUGES_UI_HPP
#define GAUGES_UI_HPP

#include <stddef.h>
#include <stdint.h>

#include "channels.hpp"
#include "dme.hpp"
#include "units.hpp"

namespace gauges {

enum SourceMode { kSourceAuto, kSourceDme, kSourceCarData, kSourceDemo };

/* How the TRANSMISSION page draws the drivetrain: automatic (xDrive once
 * its split or transfer case answers), rear-wheel drive, xDrive. */
enum Drivetrain { kDriveAuto, kDriveRwd, kDriveXdrive };

struct UiInput {
    const Sample *samples; // kChannelCount
    uint64_t now_ms;
    SourceMode mode;
    DmeStatus dme;
    bool car_data;     // capability granted
    int car_data_fresh; // values iDrive gave in the last seconds
    int defs_count;     // definition file values
    int defs_errors;
    const char *defs_error; // the first, or ""
    const char *version;
};

class Ui {
public:
    enum Page {
        kEngine,
        kFuelAir,
        kTemperatures,
        kIgnition, // DPF on a diesel
        kTransmission,
        kMore,
        kConnection,
        kPageCount
    };

    Ui();
    /* Builds every page on LVGL's active screen, `width` x `height` pixels,
     * with text from the TrueType font `ttf` (kept by the caller). */
    bool init(int width, int height, const uint8_t *ttf, size_t ttf_size);
    /* Display units and the boost gauge's top (kPa, relative). */
    void configure(const Units &units, float boost_max);
    void show_page(int page);
    int page() const {
        return page_;
    }
    void turn(int detents);
    /* Updates every visible widget; call once per frame before LVGL draws. */
    void update(const UiInput &in);
    void set_drivetrain(Drivetrain drivetrain);
    /* Forgets the torque and power curves (the controller's push). */
    void reset_peaks();
    /* Whether the car showed a diesel (fuel type, a particulate filter): the
     * IGNITION page is then DPF. It stays so once seen. */
    bool diesel() const;

private:
    Ui(const Ui &);
    Ui &operator=(const Ui &);

    int page_;
    bool built_;
    uint64_t last_slow_ms_;
};

/* LVGL's warnings into the application's log. */
void ui_install_log();

} // namespace gauges

#endif
