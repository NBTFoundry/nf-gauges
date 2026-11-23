// SPDX-License-Identifier: MPL-2.0
// HSFZ over sockets: finding the car's gateway (UDP 6811 broadcast on every
// interface, the head unit's own link to the gateway included) and a tester
// connection to it (TCP 6801) that sends UDS requests to an ECU and returns
// its answers. Blocking calls with timeouts: run them off the screen's thread.
#ifndef GAUGES_HSFZ_LINK_HPP
#define GAUGES_HSFZ_LINK_HPP

#include <stddef.h>
#include <stdint.h>

#include "hsfz.hpp"

namespace gauges {

struct Gateway {
    uint32_t ip; // network order
    uint16_t port;
    HsfzIdent ident;
};

/* "a.b.c.d" (and ":port") into a Gateway (ident empty); false when malformed. */
bool parse_address(const char *text, Gateway *out);
void format_ip(uint32_t ip, char *out, size_t size);

/* Asks every interface's broadcast address, 169.254.255.255 and
 * 255.255.255.255 (and `extra`, "ip:port", when given) for vehicle
 * identification; collects the answers for `timeout_ms`. Returns how many
 * gateways answered (at most `max`); -1 when no socket could be opened.
 * `searched` (may be NULL) gets the addresses asked, for the log. */
int hsfz_discover(Gateway *out, int max, int timeout_ms, const char *extra, char *searched,
                  size_t searched_size);

class HsfzLink {
public:
    HsfzLink();
    ~HsfzLink();

    bool open(const Gateway &gateway, int timeout_ms);
    void close();
    bool is_open() const {
        return fd_ >= 0;
    }

    enum Result {
        kOk = 1,
        kTimeout = 0,  // no answer in time; the link stays open
        kBroken = -1,  // the connection failed: close() and open again
        kRefused = -2, // the gateway refused the request (control word 0x40-0x45, 0xFF)
    };
    /* Sends `data` to ECU `target` and waits up to `timeout_ms` for its
     * answer, its UDS bytes in `answer` (`*size` set). Acknowledgements are
     * skipped, alive checks answered, and "response pending" (7F xx 78)
     * extends the wait by `timeout_ms` (at most five times). A negative
     * answer is returned as kOk with the 7F bytes. */
    Result request(uint8_t target, const uint8_t *data, size_t size, uint8_t *answer, size_t cap,
                   size_t *answer_size, int timeout_ms);

    uint8_t tester() const {
        return tester_;
    }
    void set_tester(uint8_t tester) {
        tester_ = tester;
    }
    /* The control word of the gateway's last refusal (kRefused). */
    uint16_t refusal() const {
        return refusal_;
    }
    const char *error() const {
        return error_;
    }

private:
    HsfzLink(const HsfzLink &);
    HsfzLink &operator=(const HsfzLink &);

    bool send_all(const uint8_t *data, size_t size);
    /* Reads what arrives within `timeout_ms` into the reader; false when the
     * connection failed. */
    bool pump(int timeout_ms);
    void set_error(const char *what, int err);

    int fd_;
    uint8_t tester_;
    uint16_t refusal_;
    HsfzReader reader_;
    char error_[96];
};

} // namespace gauges

#endif
