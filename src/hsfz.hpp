// SPDX-License-Identifier: MPL-2.0
// BMW's diagnostic transport over Ethernet (HSFZ, "High Speed Fahrzeug
// Zugang"), as the car's gateway speaks it on TCP 6801 and UDP 6811. A frame
// is a 4-byte big-endian length (of what follows the 6-byte header), a 2-byte
// control word, then for diagnostic messages the source and target addresses
// and the UDS bytes. The gateway acknowledges each request by echoing it
// (control 0x0002) before the ECU's answer (0x0001), and asks the tester
// whether it is still there (0x0012; the tester answers 0x0013 with its
// address). Pure functions: the sockets are in hsfz_link.
#ifndef GAUGES_HSFZ_HPP
#define GAUGES_HSFZ_HPP

#include <stddef.h>
#include <stdint.h>

namespace gauges {

enum {
    kHsfzDiagPort = 6801,
    kHsfzIdentPort = 6811,
    kHsfzHeader = 6,
    kHsfzMaxFrame = 4096 + kHsfzHeader,
};

enum HsfzControl {
    kHsfzDiag = 0x0001,
    kHsfzAck = 0x0002,
    kHsfzIdent = 0x0011,
    kHsfzAliveCheck = 0x0012,
    kHsfzAliveReply = 0x0013,
    kHsfzBadTester = 0x0040,
    kHsfzBadControl = 0x0041,
    kHsfzBadFormat = 0x0042,
    kHsfzBadTarget = 0x0043,
    kHsfzTooLarge = 0x0044,
    kHsfzNotReady = 0x0045,
    kHsfzNack = 0x00FF,
};

enum { kTesterAddress = 0xF4, kDmeAddress = 0x12, kGatewayAddress = 0x10 };

/* A diagnostic message: header, source, target, data. Returns the frame's
 * size, or 0 when it does not fit. */
size_t hsfz_diag(uint8_t source, uint8_t target, const uint8_t *data, size_t size, uint8_t *out,
                 size_t cap);
/* A frame with any control word and payload. */
size_t hsfz_frame(uint16_t control, const uint8_t *payload, size_t size, uint8_t *out, size_t cap);
/* The answer to an alive check. */
size_t hsfz_alive_reply(uint8_t tester, uint8_t *out, size_t cap);

struct HsfzFrame {
    uint16_t control;
    const uint8_t *payload; // into the reader's buffer: valid until the next feed()
    size_t size;
};

/* Splits a TCP stream into frames. */
class HsfzReader {
public:
    HsfzReader();
    /* Appends bytes; false when they overflow the buffer (garbage: reset). */
    bool feed(const uint8_t *data, size_t size);
    /* The next complete frame, consumed; false when none is complete. A frame
     * longer than kHsfzMaxFrame makes the reader fail (bad()). */
    bool next(HsfzFrame *frame);
    bool bad() const {
        return bad_;
    }
    void reset();

private:
    uint8_t buf_[2 * kHsfzMaxFrame];
    size_t len_;
    size_t consumed_;
    bool bad_;
};

/* The gateway's answer to a vehicle identification request (UDP 6811):
 * "DIAGADR10BMWMAC...BMWVIN..." in a control 0x0011 frame. */
struct HsfzIdent {
    uint8_t address; // DIAGADR, hex
    char mac[13];
    char vin[18];
};
bool hsfz_parse_ident(const uint8_t *frame, size_t size, HsfzIdent *out);

/* UDS answers. */
bool uds_negative(const uint8_t *data, size_t size, uint8_t *service, uint8_t *code);
const char *uds_nrc_text(uint8_t code);

} // namespace gauges

#endif
