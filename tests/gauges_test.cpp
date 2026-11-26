// SPDX-License-Identifier: MPL-2.0
// Unit tests: HSFZ framing and identification, the OBD formulas and plan,
// definition files, units, the store's source priority, iDrive's JSON, wheel
// slip, the particulate filter's values.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "plot.hpp"
#include "car_art.hpp"
#include "channels.hpp"
#include "defs.hpp"
#include "hsfz.hpp"
#include "hsfz_link.hpp"
#include "obd.hpp"
#include "sources.hpp"
#include "units.hpp"

using namespace gauges;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))

void test_hsfz_frames() {
    uint8_t out[64];
    const uint8_t req[3] = {0x22, 0xF4, 0x0C};
    const size_t n = hsfz_diag(0xF4, 0x12, req, sizeof(req), out, sizeof(out));
    const uint8_t want[] = {0, 0, 0, 5, 0, 1, 0xF4, 0x12, 0x22, 0xF4, 0x0C};
    CHECK(n == sizeof(want) && !memcmp(out, want, n));
    CHECK(hsfz_diag(0xF4, 0x12, req, sizeof(req), out, 8) == 0);
    const size_t a = hsfz_alive_reply(0xF4, out, sizeof(out));
    const uint8_t alive[] = {0, 0, 0, 2, 0, 0x13, 0x00, 0xF4};
    CHECK(a == sizeof(alive) && !memcmp(out, alive, a));

    // A stream split anywhere: ack, then the answer.
    const uint8_t stream[] = {0, 0, 0, 5, 0, 2, 0xF4, 0x12, 0x22, 0xF4, 0x0C,
                              0, 0, 0, 7, 0, 1, 0x12, 0xF4, 0x62, 0xF4, 0x0C, 0x0D, 0x28};
    for (size_t cut = 0; cut <= sizeof(stream); ++cut) {
        HsfzReader r;
        HsfzFrame f;
        r.feed(stream, cut);
        int frames = 0;
        uint16_t controls[2] = {0, 0};
        while (r.next(&f)) {
            controls[frames++ & 1] = f.control;
        }
        r.feed(stream + cut, sizeof(stream) - cut);
        while (r.next(&f)) {
            if (frames < 2) {
                controls[frames] = f.control;
            }
            ++frames;
            if (f.control == kHsfzDiag) {
                CHECK(f.size == 7 && f.payload[0] == 0x12 && f.payload[5] == 0x0D);
            }
        }
        CHECK(frames == 2 && controls[0] == kHsfzAck && controls[1] == kHsfzDiag);
    }
    // An absurd length marks the stream bad.
    HsfzReader bad;
    const uint8_t huge[] = {0x7F, 0, 0, 0, 0, 1};
    HsfzFrame f;
    bad.feed(huge, sizeof(huge));
    CHECK(!bad.next(&f) && bad.bad());
}

void test_hsfz_ident() {
    const char body[] = "DIAGADR10BMWMAC001A37AABBCCBMWVINWBA3B5C50FF000001";
    uint8_t frame[128];
    const size_t n = hsfz_frame(kHsfzIdent, reinterpret_cast<const uint8_t *>(body), strlen(body),
                                frame, sizeof(frame));
    HsfzIdent id;
    CHECK(hsfz_parse_ident(frame, n, &id));
    CHECK(id.address == 0x10);
    CHECK(!strcmp(id.mac, "001A37AABBCC"));
    CHECK(!strcmp(id.vin, "WBA3B5C50FF000001"));
    frame[5] = 0x01;
    CHECK(!hsfz_parse_ident(frame, n, &id));
    frame[5] = 0x11;
    CHECK(!hsfz_parse_ident(frame, n - 1, &id)); // length mismatch
}

void test_addresses() {
    Gateway gw;
    CHECK(parse_address("169.254.10.20", &gw) && gw.port == kHsfzDiagPort);
    char ip[16];
    format_ip(gw.ip, ip, sizeof(ip));
    CHECK(!strcmp(ip, "169.254.10.20"));
    CHECK(parse_address("127.0.0.1:16801", &gw) && gw.port == 16801);
    CHECK(!parse_address("127.0.0.1:0", &gw));
    CHECK(!parse_address("300.0.0.1", &gw));
    CHECK(!parse_address("1.2.3", &gw));
    CHECK(!parse_address("1.2.3.4:5x", &gw));
}

void test_uds() {
    const uint8_t nrc[] = {0x7F, 0x22, 0x31};
    uint8_t service = 0, code = 0;
    CHECK(uds_negative(nrc, sizeof(nrc), &service, &code) && service == 0x22 && code == 0x31);
    const uint8_t pos[] = {0x62, 0xF4, 0x0C, 0, 0};
    CHECK(!uds_negative(pos, sizeof(pos), &service, &code));
    CHECK(!strcmp(uds_nrc_text(0x31), "request out of range"));
}

float decode1(uint8_t pid, const uint8_t *d, size_t n, int channel) {
    ObdValue v[kObdMaxValues];
    const int k = obd_decode(pid, d, n, v, kObdMaxValues);
    for (int i = 0; i < k; ++i) {
        if (v[i].channel == channel) {
            return v[i].value;
        }
    }
    return NAN;
}

void test_obd() {
    const uint8_t rpm[] = {0x0D, 0x28};
    CHECK_NEAR(decode1(0x0C, rpm, 2, kRpm), 842.0, 1e-3);
    const uint8_t timing[] = {0x90};
    CHECK_NEAR(decode1(0x0E, timing, 1, kTiming), 8.0, 1e-3);
    const uint8_t coolant[] = {0x7B};
    CHECK_NEAR(decode1(0x05, coolant, 1, kCoolant), 83.0, 1e-3);
    const uint8_t lambda34[] = {0x80, 0x00, 0x80, 0x00};
    CHECK_NEAR(decode1(0x34, lambda34, 4, kLambda), 1.0, 1e-4);
    const uint8_t lambda24[] = {0x80, 0x00, 0x00, 0x00};
    CHECK_NEAR(decode1(0x24, lambda24, 4, kLambda), 1.0, 1e-4);
    const uint8_t rail[] = {0x03, 0xE8};
    CHECK_NEAR(decode1(0x23, rail, 2, kRail), 10000.0, 1e-2);
    // 0x87: sensor A present, 200 kPa = 6400/32.
    const uint8_t map87[] = {0x01, 0x19, 0x00, 0x00, 0x00};
    CHECK_NEAR(decode1(0x87, map87, 5, kMap), 200.0, 1e-3);
    // 0x70: commanded and actual A present.
    const uint8_t boost70[] = {0x03, 0x19, 0x00, 0x16, 0x80, 0, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x70, boost70, 10, kBoostTarget), 200.0, 1e-3);
    CHECK_NEAR(decode1(0x70, boost70, 10, kMap), 180.0, 1e-3);
    const uint8_t only_target[] = {0x01, 0x19, 0x00, 0x16, 0x80, 0, 0, 0, 0, 0};
    CHECK(isnan(decode1(0x70, only_target, 10, kMap)));
    // Too short.
    CHECK(isnan(decode1(0x0C, rpm, 1, kRpm)));
    CHECK(obd_find(0x99) == NULL);

    // Support bitmaps: F400 says 0x01-0x20, its last bit that 0x20 block exists.
    uint8_t supported[32];
    memset(supported, 0, sizeof(supported));
    const uint8_t bits00[] = {0x18, 0x3B, 0x80, 0x11}; // 04 05 0b 0c 0d 0f 10 11 ... 20
    obd_support_set(supported, 0x00, bits00);
    CHECK(obd_supported(supported, 0x04) && obd_supported(supported, 0x05));
    CHECK(obd_supported(supported, 0x0B) && obd_supported(supported, 0x0C));
    CHECK(!obd_supported(supported, 0x0E));
    CHECK(obd_supported(supported, 0x20));
    const uint8_t bits80[] = {0x00, 0x00, 0x00, 0x00};
    obd_support_set(supported, 0x80, bits80);
    // 0x87 then 0x0B both give the manifold: the plan takes 0x87 only.
    const uint8_t with87[] = {0x02, 0x00, 0x00, 0x00}; // 0x87
    obd_support_set(supported, 0x80, with87);
    CHECK(obd_supported(supported, 0x87));
    uint8_t pids[64];
    const int n = obd_plan(supported, pids, 64);
    bool has87 = false, has0b = false;
    for (int i = 0; i < n; ++i) {
        has87 = has87 || pids[i] == 0x87;
        has0b = has0b || pids[i] == 0x0B;
    }
    CHECK(has87 && !has0b);
}

void test_definitions() {
    DefinitionSet *set = new DefinitionSet;
    definitions_clear(set);
    const char text[] = "\xEF\xBB\xBF# a comment\r\n"
                        "id,label,unit,ecu,did,pos,type,mul,div,add,min,max\r\n"
                        "\r\n"
                        "corr1,,,12,0x4A10,1,s8,0.375,1,0,,\r\n"
                        "map,,,12,4B20,1,u16,1,100,0,,\n"
                        "wastegate,Wastegate,%,12,5A20,3,u16,100,65535,0,0,100\n"
                        "bad id,,,12,1,1,u8,1\n"
                        "corr1,,,12,4A11,1,s8,1\n"
                        "x,,,12,1234,1,u24,1\n"
                        "y,,,12,1234,1,u8,1,0\n";
    const int added = definitions_parse(set, "test.csv", text, strlen(text));
    CHECK(added == 3);
    CHECK(set->count == 3);
    CHECK(set->error_count == 4);
    CHECK(set->defs[0].channel == kCorr1 && set->defs[0].did == 0x4A10 && set->defs[0].is_signed);
    CHECK(set->defs[1].channel == kMap && set->defs[1].size == 2);
    const int wg = channel_by_key("wastegate");
    CHECK(wg == kCustom1 && custom_used(wg));
    CHECK(!strcmp(channel_info(wg).label, "WASTEGATE") && !strcmp(custom_unit(wg), "%"));
    CHECK(set->defs[2].channel == wg && set->defs[2].pos == 3);

    float v = 0;
    const uint8_t corr[] = {0xFC}; // -4 * 0.375
    CHECK(definition_value(set->defs[0], corr, 1, &v));
    CHECK_NEAR(v, -1.5, 1e-6);
    const uint8_t map[] = {0x32, 0xB9}; // 12985 / 100 = 129.85 kPa
    CHECK(definition_value(set->defs[1], map, 2, &v));
    CHECK_NEAR(v, 129.85, 1e-3);
    const uint8_t wgrec[] = {0, 0, 0x80, 0x00};
    CHECK(definition_value(set->defs[2], wgrec, 4, &v));
    CHECK_NEAR(v, 50.0, 0.01);
    CHECK(!definition_value(set->defs[2], wgrec, 3, &v));
    const uint8_t s32[] = {0xFF, 0xFF, 0xFF, 0xFE};
    Definition d = set->defs[0];
    d.size = 4;
    d.mul = 1;
    CHECK(definition_value(d, s32, 4, &v));
    CHECK_NEAR(v, -2.0, 1e-6);
    definitions_clear(set);
    CHECK(!custom_used(kCustom1) && channel_by_key("wastegate") == -1);
    delete set;
}

void test_units() {
    Units metric = {false, false}, imperial = {true, true};
    char text[16];
    format_value(text, sizeof(text), kBoost, 123.0f, true, metric);
    CHECK(!strcmp(text, "1.23"));
    format_value(text, sizeof(text), kBoost, 100.0f, true, imperial);
    CHECK(!strcmp(text, "14.5"));
    format_value(text, sizeof(text), kBoost, -0.2f, true, metric);
    CHECK(!strcmp(text, "0.00"));
    format_value(text, sizeof(text), kCoolant, 100.0f, true, imperial);
    CHECK(!strcmp(text, "212"));
    format_value(text, sizeof(text), kLambda, 1.0f, true, imperial);
    CHECK(!strcmp(text, "14.7"));
    format_value(text, sizeof(text), kLambda, 0.85f, true, metric);
    CHECK(!strcmp(text, "0.85"));
    format_value(text, sizeof(text), kRail, 15000.0f, true, metric);
    CHECK(!strcmp(text, "150"));
    format_value(text, sizeof(text), kTiming, 12.5f, false, metric);
    CHECK(!strcmp(text, "--"));
    CHECK(!strcmp(unit_text(kBoost, imperial), "psi"));
    CHECK(!strcmp(unit_text(kLambda, metric), ""));
    CHECK(!strcmp(unit_text(kLambda, imperial), "AFR"));
}

void test_store() {
    Store *s = new Store;
    Sample all[kChannelCount];
    s->set(kRpm, 800, kCarData, 1000);
    s->set(kRpm, 850, kDme, 1100);
    s->set(kRpm, 900, kCarData, 1200); // the DME's is current: ignored
    s->snapshot(all);
    CHECK(all[kRpm].value == 850 && all[kRpm].source == kDme);
    s->set(kRpm, 950, kCarData, 1100 + Store::kStaleMs + 1); // the DME's is stale
    s->snapshot(all);
    CHECK(all[kRpm].value == 950 && all[kRpm].source == kCarData);
    CHECK(all[kRpm].max == 950 && all[kRpm].min == 800);
    s->reset_peaks();
    s->snapshot(all);
    CHECK(all[kRpm].max == 950 && all[kRpm].min == 950);
    s->set(kRpm, NAN, kDme, 5000);
    s->snapshot(all);
    CHECK(all[kRpm].value == 950);
    CHECK(fresh(all[kRpm], 4000) && !fresh(all[kRpm], 1100 + 2 * Store::kStaleMs + 2));
    s->drop(kCarData);
    s->snapshot(all);
    CHECK(!all[kRpm].valid);
    delete s;
}

void test_json() {
    double v = 0;
    CHECK(json_number("{\"temperature\":{\"engine\":92,\"oil\":104}}", "oil", &v) && v == 104);
    CHECK(json_number("{\"temperature\":{\"engine\":92,\"oil\":104}}", "engine", &v) && v == 92);
    CHECK(json_number("{\"RPMSpeed\":812}", "RPMSpeed", &v) && v == 812);
    CHECK(json_number("{\"temperatureExterior\":-3.5}", "temperatureExterior", &v) && v == -3.5);
    CHECK(!json_number("{\"RPMSpeed\":812}", "RPM", &v));
    CHECK(!json_number("{\"a\":\"x\"}", "a", &v));
}

void test_dpf() {
    // 0x8B: all there; regenerating, active; trigger 50% (0x80 = 50.2%);
    // 400 minutes, 420 km between regenerations.
    const uint8_t after[] = {0x73, 0x03, 0x80, 0x01, 0x90, 0x01, 0xA4};
    CHECK_NEAR(decode1(0x8B, after, 7, kDpfRegen), kRegenActive, 1e-6);
    CHECK_NEAR(decode1(0x8B, after, 7, kDpfLoad), 50.196, 1e-2);
    CHECK_NEAR(decode1(0x8B, after, 7, kDpfAvgTime), 400.0, 1e-3);
    CHECK_NEAR(decode1(0x8B, after, 7, kDpfAvgDistance), 420.0, 1e-3);
    const uint8_t passive[] = {0x03, 0x01, 0, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x8B, passive, 7, kDpfRegen), kRegenPassive, 1e-6);
    CHECK(isnan(decode1(0x8B, passive, 7, kDpfLoad)));
    const uint8_t untyped[] = {0x01, 0x01, 0, 0, 0, 0, 0}; // type not given
    CHECK_NEAR(decode1(0x8B, untyped, 7, kDpfRegen), kRegenOn, 1e-6);
    const uint8_t idle[] = {0x03, 0x00, 0, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x8B, idle, 7, kDpfRegen), kRegenOff, 1e-6);
    // 0x7A: the drop, signed, 1/100 kPa: 12.5 kPa; then -0.5 kPa.
    const uint8_t drop[] = {0x01, 0x04, 0xE2, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x7A, drop, 7, kDpfDelta), 12.5, 1e-3);
    const uint8_t negative[] = {0x01, 0xFF, 0xCE, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x7A, negative, 7, kDpfDelta), -0.5, 1e-3);
    // 0x7C: inlet 600.0 C ((6400)/10 - 40), outlet 620.0 C.
    const uint8_t temps[] = {0x03, 0x19, 0x00, 0x19, 0xC8, 0, 0, 0, 0};
    CHECK_NEAR(decode1(0x7C, temps, 9, kDpfInTemp), 600.0, 1e-3);
    CHECK_NEAR(decode1(0x7C, temps, 9, kDpfOutTemp), 620.0, 1e-3);
    const uint8_t diesel[] = {4};
    CHECK_NEAR(decode1(0x51, diesel, 1, kFuelType), 4.0, 1e-6);
    CHECK(diesel_fuel(4) && diesel_fuel(19) && diesel_fuel(23) && !diesel_fuel(1));
    Units metric = {false, false}, imperial = {true, false};
    char text[32];
    format_value(text, sizeof(text), kDpfRegen, kRegenActive, true, metric);
    CHECK(!strcmp(text, "ACTIVE"));
    format_value(text, sizeof(text), kDpfDelta, 12.5f, true, metric);
    CHECK(!strcmp(text, "125") && !strcmp(unit_text(kDpfDelta, metric), "mbar"));
    format_value(text, sizeof(text), kDpfDelta, 6.894757f, true, imperial);
    CHECK(!strcmp(text, "1.00") && !strcmp(unit_text(kDpfDelta, imperial), "psi"));
    format_value(text, sizeof(text), kDpfAvgDistance, 160.9344f, true, imperial);
    CHECK(!strcmp(text, "100") && !strcmp(unit_text(kDpfAvgDistance, imperial), "mi"));
}

void test_wheels() {
    // The rear wheels spin 10% faster than the fronts.
    const float spin[4] = {100, 100, 110, 110};
    float slip[4];
    slip_of(spin, false, slip);
    CHECK_NEAR(slip[0], 0, 1e-4);
    CHECK_NEAR(slip[2], 10, 1e-4);
    // A bend: the outer (right) wheels faster, no slip.
    const float bend[4] = {96, 104, 96, 104};
    slip_of(bend, false, slip);
    CHECK(slip[0] == 0 && slip[1] == 0 && slip[2] == 0 && slip[3] == 0);
    // One wheel locks under braking: it alone is negative.
    const float lock[4] = {60, 100, 100, 100};
    slip_of(lock, true, slip);
    CHECK_NEAR(slip[0], -40, 1e-3);
    CHECK(slip[1] == 0 && slip[2] == 0 && slip[3] == 0);
    // Below 5 km/h: none.
    const float crawl[4] = {1, 4, 2, 3};
    slip_of(crawl, false, slip);
    CHECK(slip[0] == 0 && slip[1] == 0);
    // From the store: only with all four wheels current.
    Store store;
    store.set(kWheelFL, 50, kDme, 1000);
    store.set(kWheelFR, 50, kDme, 1000);
    store.set(kWheelRL, 55, kDme, 1000);
    CHECK(!derive_slip(&store, kDme, 1000));
    store.set(kWheelRR, 55, kDme, 1000);
    CHECK(derive_slip(&store, kDme, 1000));
    CHECK_NEAR(store.get(kSlipRR).value, 10, 1e-3);
    // Braking: against the faster wheel, the fronts lose 9%.
    store.set(kAccelLong, -5.0f, kCarData, 1000);
    CHECK(derive_slip(&store, kDme, 1000));
    CHECK_NEAR(store.get(kSlipFL).value, -9.0909, 1e-3);
    CHECK_NEAR(store.get(kSlipRR).value, 0, 1e-4);
    CHECK(store.get(kSlipRR).source == kDme);
    CHECK(!derive_slip(&store, kDme, 1000 + Store::kStaleMs));
    // Slip is not a definition file's: the speeds are.
    clear_custom();
    DefinitionSet *set = new DefinitionSet;
    memset(set, 0, sizeof(*set));
    const char csv[] = "wheel_fl,,,29,4D10,1,s16,1,100,0,,\nslip_fl,,,29,4D10,1,s16,1,100,0,,\n";
    CHECK(definitions_parse(set, "dsc.csv", csv, sizeof(csv) - 1) == 1);
    CHECK(set->count == 1 && set->defs[0].channel == kWheelFL && set->defs[0].ecu == 0x29);
    CHECK(set->error_count == 1 && strstr(set->errors[0], "wheel speeds"));
    delete set;
    // Acceleration in g.
    Units metric = {false, false};
    char text[16];
    format_value(text, sizeof(text), kAccelLat, 4.903325f, true, metric);
    CHECK(!strcmp(text, "0.50") && !strcmp(unit_text(kAccelLat, metric), "g"));
}

void test_transmission() {
    // iDrive's gear codes, the drive mode, the gear as shown.
    float gear = 99;
    CHECK(car_data_gear(1, &gear) && gear == 0);
    CHECK(car_data_gear(2, &gear) && gear == -1);
    CHECK(car_data_gear(3, &gear) && gear == -2);
    CHECK(car_data_gear(5, &gear) && gear == 1);
    CHECK(car_data_gear(12, &gear) && gear == 8);
    CHECK(!car_data_gear(0, &gear) && !car_data_gear(4, &gear) && !car_data_gear(17, &gear));
    char text[8];
    gear_text(text, sizeof(text), 3, 5); // Sport+
    CHECK(!strcmp(text, "S3"));
    gear_text(text, sizeof(text), 3, 2); // Comfort
    CHECK(!strcmp(text, "D3"));
    gear_text(text, sizeof(text), 4, -1); // no mode: the gear alone
    CHECK(!strcmp(text, "4"));
    gear_text(text, sizeof(text), -1, 5);
    CHECK(!strcmp(text, "R"));
    CHECK(!strcmp(state_text(kDriveMode, 5), "SPORT+") && !strcmp(state_text(kDriveMode, 7), "ECO PRO"));
    float v = 0;
    CHECK(car_data_value(kGear, 7, &v) && v == 3);
    CHECK(!car_data_value(kAccelLong, 65535.0, &v));
    CHECK(car_data_value(kAccelLat, -2.5, &v) && v == -2.5f);
    // The car's image: opaque paint in the middle, clear in the corners, the
    // wheels' and gearbox's places within it.
    const CarGeometry geo = car_geometry(405);
    CHECK(geo.width > 150 && geo.width < 220 && geo.height > 405 && geo.height < 440);
    CHECK(geo.front_axle < geo.gearbox_top && geo.gearbox_bottom < geo.rear_axle);
    CHECK(geo.rear_tyre_width >= geo.front_tyre_width && geo.track < geo.width / 2);
    const int stride = geo.width * 4;
    uint8_t *img = new uint8_t[static_cast<size_t>(stride) * static_cast<size_t>(geo.height)];
    memset(img, 0x55, static_cast<size_t>(stride) * static_cast<size_t>(geo.height));
    car_draw(geo, img, stride);
    const uint8_t *corner = img;
    CHECK(corner[3] == 0);
    const uint8_t *bonnet = img + static_cast<size_t>(geo.top + 100) * stride + static_cast<size_t>(geo.mid_x) * 4;
    CHECK(bonnet[3] == 255);
    const uint8_t *beside = img + static_cast<size_t>(geo.height / 2) * stride + 2 * 4;
    CHECK(beside[3] == 0);
    delete[] img;
}

void test_channel_table() {
    for (int c = 0; c < kCustom1; ++c) {
        const ChannelInfo &info = channel_info(c);
        CHECK(info.key && *info.key && info.label && *info.label);
        CHECK(info.max > info.min);
        CHECK(channel_by_key(info.key) == c);
    }
}

} // namespace

void test_plot() {
    enum { kW = 24, kH = 16 };
    uint8_t image[kW * kH * 4];
    gauges::plot_clear(image, kH, kW * 4);
    // A level line three pixels wide: full on its row and the two beside it,
    // nothing two rows away; its colour as given (B, G, R).
    const float x[] = {2, 12, 21}, y[] = {8, 8, 8};
    CHECK(gauges::plot_line(image, kW, kH, kW * 4, x, y, 3, 3.0f, 0x2EC5FF));
    const uint8_t *on = image + (8 * kW + 10) * 4, *beside = image + (9 * kW + 10) * 4;
    CHECK(on[3] == 255 && on[0] == 0xFF && on[1] == 0xC5 && on[2] == 0x2E);
    CHECK(beside[3] == 255 && image[(10 * kW + 10) * 4 + 3] == 0 && image[(8 * kW + 23) * 4 + 3] == 0);
    // Half a pixel lower: the rows above and below share it.
    gauges::plot_clear(image, kH, kW * 4);
    const float lower[] = {8.5f, 8.5f, 8.5f};
    CHECK(gauges::plot_line(image, kW, kH, kW * 4, x, lower, 3, 1.0f, 0xFFFFFF));
    CHECK_NEAR(image[(8 * kW + 10) * 4 + 3], 128, 2);
    CHECK_NEAR(image[(9 * kW + 10) * 4 + 3], 128, 2);
    // Between two points the curve stays between them, and a second line goes over the first.
    gauges::plot_clear(image, kH, kW * 4);
    const float step_y[] = {12, 12, 4};
    CHECK(gauges::plot_line(image, kW, kH, kW * 4, x, step_y, 3, 1.0f, 0xFFFFFF));
    for (int row = 13; row < kH; ++row) {
        for (int col = 0; col < kW; ++col) {
            CHECK(image[(row * kW + col) * 4 + 3] < 128 || row == 13);
        }
    }
    CHECK(gauges::plot_line(image, kW, kH, kW * 4, x, y, 3, 3.0f, 0xFF0000));
    CHECK(image[(8 * kW + 5) * 4 + 2] == 0xFF && image[(8 * kW + 5) * 4 + 0] == 0);
    // One point is a dot.
    gauges::plot_clear(image, kH, kW * 4);
    CHECK(gauges::plot_line(image, kW, kH, kW * 4, x, y, 1, 3.0f, 0xFFFFFF));
    CHECK(image[(8 * kW + 2) * 4 + 3] == 255 && image[(8 * kW + 6) * 4 + 3] == 0);
}

int main() {
    test_hsfz_frames();
    test_hsfz_ident();
    test_addresses();
    test_uds();
    test_obd();
    test_definitions();
    test_units();
    test_store();
    test_json();
    test_channel_table();
    test_dpf();
    test_wheels();
    test_plot();
    test_transmission();
    if (g_failures) {
        fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("OK: %d checks\n", g_checks);
    return 0;
}
