// SPDX-License-Identifier: MPL-2.0
#include "dme.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hsfz_link.hpp"
#include "nbt/app/clock.hpp"
#include "nbt/app/log.hpp"
#include "obd.hpp"

namespace gauges {
namespace {

// The central gateway's usual address on the head unit's link to it.
const char kKnownGateway[] = "169.254.21.3";

enum {
    kMaxItems = 64,
    kDynamicDid = 0xF300,
    kMaxBatchEntries = 20,
    kMaxBatchBytes = 64,
    kRequestTimeoutMs = 500,
    kMinCycleMs = 40, // at most 25 fast cycles a second
    kMaxMisses = 3,   // another ECU's unanswered reads before its value is left out
};

const float kStandardAtmosphere = 101.325f;
// Tester addresses tried in turn when the gateway refuses ours (control 0x40).
const uint8_t kTesters[] = {0xF4, 0xF5, 0xF1};

struct Item {
    uint8_t ecu;
    uint16_t did;
    uint8_t pid;  // a standard value's PID, or 0
    int def;      // a definition's index, or -1
    uint8_t size; // its bytes in a batched read
    bool fast;
    bool batched;
    bool dead;
    bool seen;  // answered at least once (logged)
    int misses; // unanswered reads in a row (ECUs other than the DME)
};

void hex(char *out, size_t size, const uint8_t *data, size_t n) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 4 < size; ++i) {
        used += static_cast<size_t>(snprintf(out + used, size - used, "%s%02x", i ? " " : "", data[i]));
    }
    if (n && used + 4 >= size && size > 4) {
        snprintf(out + size - 4, 4, "...");
    }
}

uint64_t now_ms() {
    return nbt::app::monotonic_ms();
}

} // namespace

const char *dme_state_text(DmeStatus::State state) {
    switch (state) {
    case DmeStatus::kOff:
        return "off";
    case DmeStatus::kSearching:
        return "searching for the gateway";
    case DmeStatus::kConnecting:
        return "connecting";
    case DmeStatus::kIdentifying:
        return "asking the DME";
    case DmeStatus::kRunning:
        return "reading";
    case DmeStatus::kWaiting:
        return "waiting to retry";
    }
    return "";
}

Dme::Dme(Store *store)
    : store_(store), started_(false), stop_(false), enabled_(true), debug_(false),
      defs_generation_(0), diag_port_(kHsfzDiagPort) {
    pthread_mutex_init(&mutex_, NULL);
    pthread_cond_init(&wake_, NULL);
    memset(&defs_, 0, sizeof(defs_));
    memset(&status_, 0, sizeof(status_));
    status_.state = DmeStatus::kWaiting; // so the first state is logged
    status_.tester = kTesterAddress;
    gateway_[0] = extra_[0] = '\0';
}

Dme::~Dme() {
    stop();
    pthread_cond_destroy(&wake_);
    pthread_mutex_destroy(&mutex_);
}

bool Dme::start(const char *gateway, const char *extra, int diag_port) {
    if (started_) {
        return true;
    }
    diag_port_ = diag_port > 0 && diag_port < 65536 ? diag_port : kHsfzDiagPort;
    snprintf(gateway_, sizeof(gateway_), "%s", gateway ? gateway : "");
    snprintf(extra_, sizeof(extra_), "%s", extra ? extra : "");
    stop_ = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    started_ = pthread_create(&thread_, &attr, &Dme::thread_main, this) == 0;
    pthread_attr_destroy(&attr);
    if (!started_) {
        nbt::app::log_info("dme: no thread");
    }
    return started_;
}

void Dme::stop() {
    if (!started_) {
        return;
    }
    pthread_mutex_lock(&mutex_);
    stop_ = true;
    pthread_cond_broadcast(&wake_);
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, NULL);
    started_ = false;
}

void Dme::set_enabled(bool enabled) {
    pthread_mutex_lock(&mutex_);
    if (enabled_ != enabled) {
        enabled_ = enabled;
        pthread_cond_broadcast(&wake_);
    }
    pthread_mutex_unlock(&mutex_);
}

void Dme::set_debug(bool debug) {
    pthread_mutex_lock(&mutex_);
    debug_ = debug;
    pthread_mutex_unlock(&mutex_);
}

void Dme::set_definitions(const DefinitionSet &set) {
    pthread_mutex_lock(&mutex_);
    defs_ = set;
    ++defs_generation_;
    pthread_cond_broadcast(&wake_);
    pthread_mutex_unlock(&mutex_);
}

void Dme::status(DmeStatus *out) const {
    pthread_mutex_lock(&mutex_);
    *out = status_;
    pthread_mutex_unlock(&mutex_);
}

void *Dme::thread_main(void *self) {
    static_cast<Dme *>(self)->run();
    return NULL;
}

// The thread's work, as one session after another.
class DmeSession {
public:
    DmeSession(Store *store, pthread_mutex_t *mutex, pthread_cond_t *wake, bool *stop,
               bool *enabled, bool *debug, uint32_t *generation, DmeStatus *status)
        : store_(store), mutex_(mutex), wake_(wake), stop_(stop), enabled_(enabled),
          debug_(debug), generation_(generation), status_(status), tester_index_(0),
          item_count_(0), batch_count_(0), batch_bytes_(0), baro_(kStandardAtmosphere),
          reference_torque_(0), slow_next_(0) {
        memset(def_channel_, 0, sizeof(def_channel_));
    }

    // Whether to keep going (false: stop asked or the user turned it off).
    bool wanted() {
        pthread_mutex_lock(mutex_);
        const bool go = !*stop_ && *enabled_;
        pthread_mutex_unlock(mutex_);
        return go;
    }

    bool stopping() {
        pthread_mutex_lock(mutex_);
        const bool s = *stop_;
        pthread_mutex_unlock(mutex_);
        return s;
    }

    // Waits `ms` or until woken; false when the thread must stop.
    bool pause(int ms) {
        pthread_mutex_lock(mutex_);
        if (!*stop_) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += ms / 1000;
            until.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec += 1;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(wake_, mutex_, &until);
        }
        const bool go = !*stop_;
        pthread_mutex_unlock(mutex_);
        return go;
    }

    void state(DmeStatus::State s, const char *error = NULL) {
        pthread_mutex_lock(mutex_);
        const bool changed = status_->state != s;
        status_->state = s;
        if (error) {
            snprintf(status_->error, sizeof(status_->error), "%s", error);
        }
        pthread_mutex_unlock(mutex_);
        if (changed || error) {
            nbt::app::log_info("dme: %s%s%s", dme_state_text(s), error ? ": " : "",
                               error ? error : "");
        }
    }

    template <typename F> void update(F f) {
        pthread_mutex_lock(mutex_);
        f(status_);
        pthread_mutex_unlock(mutex_);
    }

    bool debug() {
        pthread_mutex_lock(mutex_);
        const bool d = *debug_;
        pthread_mutex_unlock(mutex_);
        return d;
    }

    // One request; counts and logs it.
    HsfzLink::Result ask(uint8_t ecu, const uint8_t *req, size_t n, uint8_t *answer,
                         size_t *answer_size) {
        const HsfzLink::Result r =
            link_.request(ecu, req, n, answer, 512, answer_size, kRequestTimeoutMs);
        pthread_mutex_lock(mutex_);
        ++status_->requests;
        if (r == HsfzLink::kOk) {
            ++status_->answers;
        } else if (r == HsfzLink::kTimeout) {
            ++status_->timeouts;
        }
        const bool dbg = *debug_;
        pthread_mutex_unlock(mutex_);
        if (dbg) {
            char q[64], a[96];
            hex(q, sizeof(q), req, n);
            hex(a, sizeof(a), answer, r == HsfzLink::kOk ? *answer_size : 0);
            nbt::app::log_debug("dme: %02x <- %s -> %s", ecu, q,
                                r == HsfzLink::kOk        ? a
                                : r == HsfzLink::kTimeout ? "(no answer)"
                                                          : link_.error());
        }
        return r;
    }

    // ReadDataByIdentifier; the record (after 62 hi lo) in `record`.
    // Returns kOk with *nrc 0 on a positive answer, kOk with the negative
    // response code otherwise.
    HsfzLink::Result read(uint8_t ecu, uint16_t did, uint8_t *record, size_t *size, uint8_t *nrc) {
        const uint8_t req[3] = {0x22, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did)};
        uint8_t answer[512];
        size_t n = 0;
        *nrc = 0;
        *size = 0;
        const HsfzLink::Result r = ask(ecu, req, sizeof(req), answer, &n);
        if (r != HsfzLink::kOk) {
            return r;
        }
        uint8_t service = 0;
        if (uds_negative(answer, n, &service, nrc)) {
            return HsfzLink::kOk;
        }
        if (n < 3 || answer[0] != 0x62 || answer[1] != req[1] || answer[2] != req[2]) {
            *nrc = 0x10; // not the record we asked for
            return HsfzLink::kOk;
        }
        memcpy(record, answer + 3, n - 3);
        *size = n - 3;
        return HsfzLink::kOk;
    }

    void run(const char *fixed_gateway, const char *extra, int diag_port,
             const DefinitionSet &defs) {
        defs_ = defs;
        memset(def_channel_, 0, sizeof(def_channel_));
        for (int i = 0; i < defs_.count; ++i) {
            def_channel_[defs_.defs[i].channel] = true;
        }
        Gateway gateway;
        if (!find_gateway(fixed_gateway, extra, diag_port, &gateway)) {
            return;
        }
        if (!(fixed_gateway && *fixed_gateway)) {
            gateway.port = static_cast<uint16_t>(diag_port);
        }
        state(DmeStatus::kConnecting);
        link_.set_tester(kTesters[tester_index_]);
        if (!link_.open(gateway, 2000)) {
            state(DmeStatus::kWaiting, link_.error());
            pause(2000);
            return;
        }
        update([&](DmeStatus *s) {
            ++s->reconnects;
            s->tester = link_.tester();
        });
        nbt::app::log_info("dme: connected to the gateway, tester %02x", link_.tester());
        const bool ok = identify() && plan() && poll();
        clear_batch();
        link_.close();
        store_->drop(kDme);
        if (!ok && !stopping()) {
            pause(2000);
        }
    }

    uint32_t generation_seen_;

private:
    bool find_gateway(const char *fixed, const char *extra, int diag_port, Gateway *out) {
        if (fixed && *fixed) {
            if (!parse_address(fixed, out)) {
                state(DmeStatus::kWaiting, "the gateway address is not ip[:port]");
                pause(5000);
                return false;
            }
            update([&](DmeStatus *s) { format_ip(out->ip, s->gateway, sizeof(s->gateway)); });
            return true;
        }
        state(DmeStatus::kSearching);
        // The car's packet filter on the head unit's gateway link lets TCP 6801
        // through but not the UDP identification broadcast: the usual gateway
        // address first, by its TCP port, then the broadcast (the bench).
        if (known_gateway(diag_port, out)) {
            return true;
        }
        Gateway found[4];
        char searched[160];
        const int n = hsfz_discover(found, 4, 1500, extra, searched, sizeof(searched));
        update([&](DmeStatus *s) { snprintf(s->searched, sizeof(s->searched), "%s", searched); });
        if (n <= 0) {
            static uint64_t last_logged = 0;
            if (now_ms() - last_logged > 60000) {
                last_logged = now_ms();
                nbt::app::log_info("dme: no gateway answered (asked %s)", searched);
            }
            state(DmeStatus::kSearching, n < 0 ? "no network socket" : "no gateway answered");
            pause(3000);
            return false;
        }
        *out = found[0];
        char ip[16];
        format_ip(out->ip, ip, sizeof(ip));
        nbt::app::log_info("dme: gateway %s (address %02x, VIN %s)", ip, out->ident.address,
                           out->ident.vin);
        update([&](DmeStatus *s) {
            snprintf(s->gateway, sizeof(s->gateway), "%s", ip);
            snprintf(s->vin, sizeof(s->vin), "%s", out->ident.vin);
        });
        return true;
    }

    bool known_gateway(int diag_port, Gateway *out) {
        const char *known = getenv("NBT_GAUGES_KNOWN_GATEWAY"); // tests; empty: none
        if (!known) {
            known = kKnownGateway;
        }
        if (!*known || !parse_address(known, out)) {
            return false;
        }
        out->port = static_cast<uint16_t>(diag_port);
        HsfzLink probe;
        if (!probe.open(*out, 1500)) {
            return false;
        }
        probe.close();
        char ip[16];
        format_ip(out->ip, ip, sizeof(ip));
        nbt::app::log_info("dme: gateway %s answers on its diagnostic port", ip);
        update([&](DmeStatus *s) { snprintf(s->gateway, sizeof(s->gateway), "%s", ip); });
        return true;
    }

    // A link failure: why, and the tester address to try next.
    void broken(HsfzLink::Result r, const char *during) {
        char what[192];
        if (r == HsfzLink::kTimeout) {
            snprintf(what, sizeof(what), "%s: the DME did not answer", during);
        } else {
            snprintf(what, sizeof(what), "%s: %s", during, link_.error());
        }
        if (r == HsfzLink::kRefused && link_.refusal() == kHsfzBadTester) {
            tester_index_ = (tester_index_ + 1) % static_cast<int>(sizeof(kTesters));
        }
        state(DmeStatus::kWaiting, what);
    }

    bool identify() {
        state(DmeStatus::kIdentifying);
        uint8_t record[512], nrc = 0;
        size_t n = 0;
        // The diagnostic index BMW's tools identify the DME by.
        HsfzLink::Result r = read(kDmeAddress, 0xF150, record, &n, &nrc);
        if (r != HsfzLink::kOk) {
            broken(r, "identification");
            return false;
        }
        char text[72];
        if (nrc) {
            snprintf(text, sizeof(text), "(%s)", uds_nrc_text(nrc));
        } else {
            hex(text, sizeof(text), record, n);
        }
        nbt::app::log_info("dme: diagnostic index %s", text);
        update([&](DmeStatus *s) { snprintf(s->index, sizeof(s->index), "%s", text); });
        // Which standard values it gives.
        memset(supported_, 0, sizeof(supported_));
        bool any = false;
        for (unsigned base = 0; base <= 0xC0; base += 0x20) {
            r = read(kDmeAddress, static_cast<uint16_t>(0xF400 + base), record, &n, &nrc);
            if (r != HsfzLink::kOk) {
                broken(r, "standard values");
                return false;
            }
            if (nrc || n < 4) {
                if (base == 0) {
                    nbt::app::log_info("dme: no standard values (22 f400: %s)",
                                       nrc ? uds_nrc_text(nrc) : "short answer");
                }
                break;
            }
            any = true;
            obd_support_set(supported_, static_cast<uint8_t>(base), record);
            // The last bit says whether the next block of 32 exists.
            if (!obd_supported(supported_, static_cast<uint8_t>(base + 0x20))) {
                break;
            }
        }
        char list[96] = "";
        int count = 0;
        size_t used = 0;
        for (unsigned pid = 1; pid < 256; ++pid) {
            if ((pid & 0x1F) && obd_supported(supported_, static_cast<uint8_t>(pid)) &&
                used + 4 < sizeof(list)) {
                used += static_cast<size_t>(snprintf(list + used, sizeof(list) - used, "%s%02x",
                                                     count ? " " : "", pid));
                ++count;
            }
        }
        nbt::app::log_info("dme: standard values: %s", any ? list : "none");
        update([&](DmeStatus *s) {
            snprintf(s->standard, sizeof(s->standard), "%s", any ? list : "none");
            s->standard_count = count;
        });
        return true;
    }

    void add(uint8_t ecu, uint16_t did, uint8_t pid, int def, uint8_t size, bool fast) {
        if (item_count_ == kMaxItems) {
            return;
        }
        Item &it = items_[item_count_++];
        it.ecu = ecu;
        it.did = did;
        it.pid = pid;
        it.def = def;
        it.size = size;
        it.fast = fast;
        it.batched = it.dead = it.seen = false;
        it.misses = 0;
    }

    bool plan() {
        item_count_ = 0;
        update([](DmeStatus *s) { s->defined_ok = s->defined_failed = 0; });
        uint8_t pids[64];
        const int n = obd_plan(supported_, pids, 64);
        for (int i = 0; i < n; ++i) {
            const ObdPid *p = obd_find(pids[i]);
            ObdValue v[kObdMaxValues];
            uint8_t all[16];
            memset(all, 0xFF, sizeof(all)); // every value there
            const int k = obd_decode(pids[i], all, sizeof(all), v, kObdMaxValues);
            bool fast = false, needed = k == 0; // 0x63 gives no channel of its own
            for (int j = 0; j < k; ++j) {
                fast = fast || channel_info(v[j].channel).fast;
                needed = needed || !def_channel_[v[j].channel];
            }
            if (needed) {
                add(kDmeAddress, static_cast<uint16_t>(0xF400 + pids[i]), pids[i], -1, p->size,
                    fast);
            }
        }
        for (int i = 0; i < defs_.count; ++i) {
            const Definition &d = defs_.defs[i];
            add(d.ecu, d.did, 0, i, d.size, channel_info(d.channel).fast);
        }
        if (!item_count_) {
            state(DmeStatus::kWaiting, "nothing to read: no standard values, no definition file");
            pause(10000);
            return false;
        }
        // Each identifier once on its own first: what the DME refuses is left
        // out (and out of the batch, which one refusal would fail).
        const uint64_t now = now_ms();
        for (int i = 0; i < item_count_; ++i) {
            if (!items_[i].dead && first_of(i)) {
                const HsfzLink::Result r = read_items(i, now);
                if (r != HsfzLink::kOk) {
                    broken(r, "first reading");
                    return false;
                }
            }
        }
        if (all_dead()) {
            state(DmeStatus::kWaiting, "the DME refused every value");
            pause(10000);
            return false;
        }
        define_batch();
        return true;
    }

    // One dynamically defined identifier with the fast values of the DME.
    void define_batch() {
        batch_count_ = batch_bytes_ = 0;
        uint8_t req[4 + 4 * kMaxBatchEntries] = {0x2C, 0x01, kDynamicDid >> 8, kDynamicDid & 0xFF};
        size_t len = 4;
        int entries[kMaxBatchEntries];
        for (int i = 0; i < item_count_ && batch_count_ < kMaxBatchEntries; ++i) {
            const Item &it = items_[i];
            if (!it.fast || it.dead || it.ecu != kDmeAddress ||
                batch_bytes_ + it.size > kMaxBatchBytes) {
                continue;
            }
            const uint8_t pos = it.def >= 0 ? defs_.defs[it.def].pos : 1;
            req[len++] = static_cast<uint8_t>(it.did >> 8);
            req[len++] = static_cast<uint8_t>(it.did);
            req[len++] = pos;
            req[len++] = it.size;
            entries[batch_count_++] = i;
            batch_bytes_ += it.size;
        }
        if (batch_count_ < 2) {
            batch_count_ = 0;
            return;
        }
        clear_batch();
        uint8_t answer[512];
        size_t n = 0;
        const HsfzLink::Result r = ask(kDmeAddress, req, len, answer, &n);
        uint8_t service = 0, nrc = 0;
        if (r != HsfzLink::kOk || uds_negative(answer, n, &service, &nrc) || n < 2 ||
            answer[0] != 0x6C) {
            nbt::app::log_info("dme: no batched reading (2c 01 f300: %s); one request per value",
                               r != HsfzLink::kOk ? "no answer" : nrc ? uds_nrc_text(nrc) : "unexpected answer");
            batch_count_ = 0;
            return;
        }
        for (int i = 0; i < batch_count_; ++i) {
            items_[entries[i]].batched = true;
        }
        nbt::app::log_info("dme: %d fast values in one read (f300, %d bytes)", batch_count_,
                           batch_bytes_);
    }

    void clear_batch() {
        if (!link_.is_open()) {
            return;
        }
        const uint8_t req[4] = {0x2C, 0x03, kDynamicDid >> 8, kDynamicDid & 0xFF};
        uint8_t answer[64];
        size_t n = 0;
        ask(kDmeAddress, req, sizeof(req), answer, &n);
    }

    void store_standard(uint8_t pid, const uint8_t *data, size_t size, uint64_t now) {
        if (pid == 0x63) {
            if (size >= 2) {
                reference_torque_ = static_cast<float>(data[0] << 8 | data[1]);
            }
            return;
        }
        ObdValue v[kObdMaxValues];
        const int k = obd_decode(pid, data, size, v, kObdMaxValues);
        for (int j = 0; j < k; ++j) {
            float value = v[j].value;
            const int ch = v[j].channel;
            if (def_channel_[ch]) {
                continue; // a definition file gives it
            }
            if (ch == kTorque) {
                if (reference_torque_ <= 0) {
                    continue;
                }
                value = value * reference_torque_ / 100.0f;
            } else if (ch == kBoostTarget) {
                value -= baro_; // 0x70 gives it absolute
            }
            put(ch, value, now);
        }
    }

    void put(int channel, float value, uint64_t now) {
        if (channel == kBaro) {
            baro_ = value;
        }
        store_->set(channel, value, kDme, now);
        if (channel == kMap && !def_channel_[kBoost]) {
            store_->set(kBoost, value - baro_, kDme, now);
        }
        if (channel >= kWheelFL && channel <= kWheelRR) {
            derive_slip(store_, kDme, now);
        }
    }

    // Another ECU (DSC, transfer case...) that does not answer: absent from
    // this car, or asleep. Its values are left out after a few tries; the
    // DME's reading goes on.
    bool missed(const Item &head, HsfzLink::Result r) {
        const bool absent = r == HsfzLink::kRefused && link_.refusal() == kHsfzBadTarget;
        if (head.ecu == kDmeAddress || (r != HsfzLink::kTimeout && !absent)) {
            return false;
        }
        for (int i = 0; i < item_count_; ++i) {
            Item &it = items_[i];
            if (it.dead || it.ecu != head.ecu || it.did != head.did) {
                continue;
            }
            if (absent || ++it.misses >= kMaxMisses) {
                it.dead = true;
                nbt::app::log_info("dme: %02x %04x: %s: left out", it.ecu, it.did,
                                   absent ? "the gateway does not know this ECU" : "no answer");
                if (it.def >= 0) {
                    update([](DmeStatus *s) { ++s->defined_failed; });
                }
            }
        }
        return true;
    }

    // `log`: the first answer is logged (once per identifier).
    void store_item(Item &it, const uint8_t *record, size_t size, bool batched, uint64_t now,
                    bool log = true) {
        if (!it.seen) {
            it.seen = true;
            char text[64];
            hex(text, sizeof(text), record, size);
            if (log) {
                nbt::app::log_info("dme: %02x %04x answers: %s", it.ecu, it.did, text);
            }
            if (it.def >= 0) {
                update([](DmeStatus *s) { ++s->defined_ok; });
            }
        }
        if (it.pid) {
            store_standard(it.pid, record, size, now);
            return;
        }
        Definition d = defs_.defs[it.def];
        if (batched) {
            d.pos = 1; // the batch holds exactly its bytes
        }
        float value = 0;
        if (definition_value(d, record, size, &value)) {
            put(d.channel, value, now);
        }
    }

    // Reads one identifier and gives its record to every item reading it.
    HsfzLink::Result read_items(int first, uint64_t now) {
        Item &head = items_[first];
        uint8_t record[512], nrc = 0;
        size_t n = 0;
        const HsfzLink::Result r = read(head.ecu, head.did, record, &n, &nrc);
        if (r != HsfzLink::kOk) {
            return missed(head, r) ? HsfzLink::kOk : r;
        }
        bool logged = false;
        for (int i = first; i < item_count_; ++i) {
            Item &it = items_[i];
            if (it.dead || it.batched || it.ecu != head.ecu || it.did != head.did) {
                continue;
            }
            it.misses = 0;
            if (nrc == 0x21 || nrc == 0x22 || nrc == 0x10) {
                continue; // busy or not now: next time
            }
            if (nrc) {
                it.dead = true;
                nbt::app::log_info("dme: %02x %04x refused (%s): left out", it.ecu, it.did,
                                   uds_nrc_text(nrc));
                if (it.def >= 0) {
                    update([](DmeStatus *s) { ++s->defined_failed; });
                }
                continue;
            }
            const bool unseen = !it.seen;
            store_item(it, record, n, false, now, !logged);
            logged = logged || unseen;
        }
        return r;
    }

    HsfzLink::Result read_batch(uint64_t now) {
        uint8_t record[512], nrc = 0;
        size_t n = 0;
        const HsfzLink::Result r = read(kDmeAddress, kDynamicDid, record, &n, &nrc);
        if (r != HsfzLink::kOk) {
            return r;
        }
        if (nrc || n != static_cast<size_t>(batch_bytes_)) {
            nbt::app::log_info("dme: batched read failed (%s, %u bytes): one request per value",
                               nrc ? uds_nrc_text(nrc) : "size", static_cast<unsigned>(n));
            for (int i = 0; i < item_count_; ++i) {
                items_[i].batched = false;
            }
            batch_count_ = 0;
            return r;
        }
        size_t at = 0;
        for (int i = 0; i < item_count_; ++i) {
            Item &it = items_[i];
            if (it.batched) {
                store_item(it, record + at, it.size, true, now);
                at += it.size;
            }
        }
        return r;
    }

    bool poll() {
        state(DmeStatus::kRunning);
        update([&](DmeStatus *s) {
            s->batched = batch_count_ > 0;
            s->error[0] = '\0';
        });
        int timeouts = 0;
        float rate = 0;
        uint64_t last_cycle = now_ms();
        const uint32_t generation = generation_seen_;
        while (wanted()) {
            pthread_mutex_lock(mutex_);
            const bool redefined = *generation_ != generation;
            pthread_mutex_unlock(mutex_);
            if (redefined) {
                nbt::app::log_info("dme: definitions changed: reconnecting");
                return true;
            }
            const uint64_t start = now_ms();
            HsfzLink::Result r = HsfzLink::kOk;
            if (batch_count_) {
                r = read_batch(start);
            }
            // Fast values not in the batch, each identifier once.
            for (int i = 0; i < item_count_ && r == HsfzLink::kOk; ++i) {
                const Item &it = items_[i];
                if (it.fast && !it.batched && !it.dead && first_of(i)) {
                    r = read_items(i, start);
                }
            }
            // One slow identifier per cycle, in turn.
            if (r == HsfzLink::kOk) {
                for (int tries = 0; tries < item_count_; ++tries) {
                    const int i = slow_next_++ % item_count_;
                    const Item &it = items_[i];
                    if (!it.fast && !it.dead && first_of(i)) {
                        r = read_items(i, start);
                        break;
                    }
                }
            }
            if (r == HsfzLink::kTimeout) {
                if (++timeouts >= 3) {
                    broken(r, "reading");
                    return false;
                }
            } else if (r != HsfzLink::kOk) {
                broken(r, "reading");
                return false;
            } else {
                timeouts = 0;
            }
            if (all_dead()) {
                state(DmeStatus::kWaiting, "the DME refused every value");
                pause(10000);
                return false;
            }
            const uint64_t end = now_ms();
            const uint64_t period = end - last_cycle;
            last_cycle = end;
            if (period > 0) {
                const float hz = 1000.0f / static_cast<float>(period);
                rate = rate == 0 ? hz : rate * 0.9f + hz * 0.1f;
                update([&](DmeStatus *s) { s->rate_hz = rate; });
            }
            const uint64_t spent = end - start;
            if (spent < kMinCycleMs && !pause(static_cast<int>(kMinCycleMs - spent))) {
                break;
            }
        }
        return true;
    }

    // Whether item i is the first live, unbatched item reading its identifier.
    bool first_of(int i) const {
        for (int j = 0; j < i; ++j) {
            const Item &a = items_[j];
            if (!a.dead && !a.batched && a.ecu == items_[i].ecu && a.did == items_[i].did) {
                return false;
            }
        }
        return true;
    }

    bool all_dead() const {
        for (int i = 0; i < item_count_; ++i) {
            if (!items_[i].dead) {
                return false;
            }
        }
        return true;
    }

    Store *store_;
    pthread_mutex_t *mutex_;
    pthread_cond_t *wake_;
    bool *stop_, *enabled_, *debug_;
    uint32_t *generation_;
    DmeStatus *status_;
    HsfzLink link_;
    int tester_index_;
    DefinitionSet defs_;
    bool def_channel_[kChannelCount];
    uint8_t supported_[32];
    Item items_[kMaxItems];
    int item_count_;
    int batch_count_, batch_bytes_;
    float baro_, reference_torque_;
    unsigned slow_next_;
};

void Dme::run() {
    // The session is large (definitions, items): keep it off the stack.
    DmeSession *session = new DmeSession(store_, &mutex_, &wake_, &stop_, &enabled_, &debug_,
                             &defs_generation_, &status_);
    while (!session->stopping()) {
        if (!session->wanted()) {
            session->state(DmeStatus::kOff);
            store_->drop(kDme);
            session->pause(1000);
            continue;
        }
        DefinitionSet *defs = new DefinitionSet;
        pthread_mutex_lock(&mutex_);
        *defs = defs_;
        session->generation_seen_ = defs_generation_;
        pthread_mutex_unlock(&mutex_);
        session->run(gateway_, extra_, diag_port_, *defs);
        delete defs;
    }
    session->state(DmeStatus::kOff);
    delete session;
}

} // namespace gauges
