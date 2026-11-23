// SPDX-License-Identifier: MPL-2.0
// Reads the engine computer (DME, address 0x12) through the car's gateway, as
// a diagnostic tester does over an ENET cable, from the head unit's own link
// to the gateway. Read-only: ReadDataByIdentifier (0x22) and a dynamically
// defined identifier (0x2C, 0xF300, cleared when done) that batches the fast
// values into one request, as BMW's own STATUS_BLOCK_LESEN job does. Values go
// to the Store; the screen shows the state.
#ifndef GAUGES_DME_HPP
#define GAUGES_DME_HPP

#include <pthread.h>
#include <stdint.h>

#include "channels.hpp"
#include "defs.hpp"

namespace gauges {

struct DmeStatus {
    enum State { kOff, kSearching, kConnecting, kIdentifying, kRunning, kWaiting };
    State state;
    char gateway[16];  // the gateway's address
    char vin[18];      // from its identification
    char index[72];    // the DME's diagnostic index (22 F150), hex
    char standard[96]; // the standard PIDs it gives, hex
    int standard_count;
    int defined_ok, defined_failed; // definition file values answering, refused
    bool batched;                   // fast values in one dynamically defined read
    uint8_t tester;
    float rate_hz; // fast cycles per second
    uint32_t requests, answers, timeouts, reconnects;
    char error[192];    // the last problem
    char searched[160]; // the addresses the last search asked
};

const char *dme_state_text(DmeStatus::State state);

class Dme {
public:
    explicit Dme(Store *store);
    ~Dme();

    /* Starts the thread. `gateway` ("ip[:port]", may be NULL) skips the
     * search; `extra` ("ip:port", may be NULL) is one more address it asks;
     * `diag_port` the found gateway's diagnostic port (0: 6801). */
    bool start(const char *gateway, const char *extra, int diag_port);
    void stop();
    /* Off: the connection is closed and the DME left alone. */
    void set_enabled(bool enabled);
    void set_debug(bool debug);
    /* Replaces the definition file values; applied at the next connection. */
    void set_definitions(const DefinitionSet &set);
    void status(DmeStatus *out) const;

private:
    Dme(const Dme &);
    Dme &operator=(const Dme &);
    static void *thread_main(void *self);
    void run();

    Store *store_;
    pthread_t thread_;
    bool started_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t wake_;
    bool stop_, enabled_, debug_;
    uint32_t defs_generation_;
    DefinitionSet defs_;
    DmeStatus status_;
    char gateway_[32];
    char extra_[32];
    int diag_port_;
};

} // namespace gauges

#endif
