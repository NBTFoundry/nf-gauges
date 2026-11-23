// SPDX-License-Identifier: MPL-2.0
#include "hsfz.hpp"

#include <string.h>

namespace gauges {
namespace {

void put_header(uint8_t *out, uint32_t length, uint16_t control) {
    out[0] = static_cast<uint8_t>(length >> 24);
    out[1] = static_cast<uint8_t>(length >> 16);
    out[2] = static_cast<uint8_t>(length >> 8);
    out[3] = static_cast<uint8_t>(length);
    out[4] = static_cast<uint8_t>(control >> 8);
    out[5] = static_cast<uint8_t>(control);
}

const uint8_t *find(const uint8_t *data, size_t size, const char *marker) {
    const size_t m = strlen(marker);
    for (size_t i = 0; i + m <= size; ++i) {
        if (!memcmp(data + i, marker, m)) {
            return data + i + m;
        }
    }
    return NULL;
}

bool is_hex(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int hex_value(uint8_t c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

} // namespace

size_t hsfz_frame(uint16_t control, const uint8_t *payload, size_t size, uint8_t *out,
                  size_t cap) {
    if (size > kHsfzMaxFrame - kHsfzHeader || cap < kHsfzHeader + size) {
        return 0;
    }
    put_header(out, static_cast<uint32_t>(size), control);
    if (size) {
        memcpy(out + kHsfzHeader, payload, size);
    }
    return kHsfzHeader + size;
}

size_t hsfz_diag(uint8_t source, uint8_t target, const uint8_t *data, size_t size, uint8_t *out,
                 size_t cap) {
    if (size + 2 > kHsfzMaxFrame - kHsfzHeader || cap < kHsfzHeader + 2 + size) {
        return 0;
    }
    put_header(out, static_cast<uint32_t>(size + 2), kHsfzDiag);
    out[6] = source;
    out[7] = target;
    if (size) {
        memcpy(out + 8, data, size);
    }
    return kHsfzHeader + 2 + size;
}

size_t hsfz_alive_reply(uint8_t tester, uint8_t *out, size_t cap) {
    const uint8_t payload[2] = {0x00, tester};
    return hsfz_frame(kHsfzAliveReply, payload, sizeof(payload), out, cap);
}

HsfzReader::HsfzReader() {
    reset();
}

void HsfzReader::reset() {
    len_ = consumed_ = 0;
    bad_ = false;
}

bool HsfzReader::feed(const uint8_t *data, size_t size) {
    if (consumed_) {
        memmove(buf_, buf_ + consumed_, len_ - consumed_);
        len_ -= consumed_;
        consumed_ = 0;
    }
    if (size > sizeof(buf_) - len_) {
        bad_ = true;
        return false;
    }
    memcpy(buf_ + len_, data, size);
    len_ += size;
    return true;
}

bool HsfzReader::next(HsfzFrame *frame) {
    const size_t avail = len_ - consumed_;
    if (bad_ || avail < kHsfzHeader) {
        return false;
    }
    const uint8_t *p = buf_ + consumed_;
    const uint32_t length = static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                            static_cast<uint32_t>(p[2]) << 8 | p[3];
    if (length > kHsfzMaxFrame - kHsfzHeader) {
        bad_ = true;
        return false;
    }
    if (avail < kHsfzHeader + length) {
        return false;
    }
    frame->control = static_cast<uint16_t>(p[4] << 8 | p[5]);
    frame->payload = p + kHsfzHeader;
    frame->size = length;
    consumed_ += kHsfzHeader + length;
    return true;
}

bool hsfz_parse_ident(const uint8_t *frame, size_t size, HsfzIdent *out) {
    if (size < kHsfzHeader) {
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(frame[0]) << 24 |
                            static_cast<uint32_t>(frame[1]) << 16 |
                            static_cast<uint32_t>(frame[2]) << 8 | frame[3];
    if (frame[4] != 0 || frame[5] != kHsfzIdent || length != size - kHsfzHeader) {
        return false;
    }
    const uint8_t *body = frame + kHsfzHeader;
    const size_t n = size - kHsfzHeader;
    const uint8_t *adr = find(body, n, "DIAGADR");
    const uint8_t *mac = find(body, n, "BMWMAC");
    const uint8_t *vin = find(body, n, "BMWVIN");
    const uint8_t *end = body + n;
    if (!adr || !mac || !vin || end - adr < 2 || end - mac < 12 || end - vin < 17) {
        return false;
    }
    // DIAGADR is the address in hex: "10" for the gateway.
    if (!is_hex(adr[0])) {
        return false;
    }
    out->address = static_cast<uint8_t>(hex_value(adr[0]));
    if (is_hex(adr[1])) {
        out->address = static_cast<uint8_t>(out->address << 4 | hex_value(adr[1]));
    }
    for (int i = 0; i < 12; ++i) {
        if (!is_hex(mac[i])) {
            return false;
        }
        out->mac[i] = static_cast<char>(mac[i]);
    }
    out->mac[12] = '\0';
    for (int i = 0; i < 17; ++i) {
        const uint8_t c = vin[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            return false;
        }
        out->vin[i] = static_cast<char>(c);
    }
    out->vin[17] = '\0';
    return true;
}

bool uds_negative(const uint8_t *data, size_t size, uint8_t *service, uint8_t *code) {
    if (size < 3 || data[0] != 0x7F) {
        return false;
    }
    *service = data[1];
    *code = data[2];
    return true;
}

const char *uds_nrc_text(uint8_t code) {
    switch (code) {
    case 0x10:
        return "general reject";
    case 0x11:
        return "service not supported";
    case 0x12:
        return "sub-function not supported";
    case 0x13:
        return "incorrect message length or format";
    case 0x14:
        return "response too long";
    case 0x21:
        return "busy, repeat request";
    case 0x22:
        return "conditions not correct";
    case 0x31:
        return "request out of range";
    case 0x33:
        return "security access denied";
    case 0x78:
        return "response pending";
    case 0x7E:
        return "sub-function not supported in active session";
    case 0x7F:
        return "service not supported in active session";
    default:
        return "negative response";
    }
}

} // namespace gauges
