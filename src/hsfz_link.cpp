// SPDX-License-Identifier: MPL-2.0
#include "hsfz_link.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if defined(__has_include)
#if __has_include(<sys/sockio.h>)
#include <sys/sockio.h> // QNX: the SIOC* requests
#endif
#endif
#include <unistd.h>

#include "nbt/app/clock.hpp"

namespace gauges {
namespace {

int remaining(uint64_t deadline) {
    const uint64_t now = nbt::app::monotonic_ms();
    return now >= deadline ? 0 : static_cast<int>(deadline - now);
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct Targets {
    enum { kMax = 20 };
    sockaddr_in addr[kMax];
    int count;
    void add(uint32_t ip, uint16_t port) {
        for (int i = 0; i < count; ++i) {
            if (addr[i].sin_addr.s_addr == ip && addr[i].sin_port == htons(port)) {
                return;
            }
        }
        if (count < kMax) {
            memset(&addr[count], 0, sizeof(addr[count]));
            addr[count].sin_family = AF_INET;
            addr[count].sin_port = htons(port);
            addr[count].sin_addr.s_addr = ip;
            ++count;
        }
    }
};

// Each up interface's IPv4 broadcast address (the head unit's link to the
// gateway, its Wi-Fi networks; the desktop's own).
void add_interface_broadcasts(int fd, Targets *targets) {
    struct if_nameindex *names = if_nameindex();
    if (!names) {
        return;
    }
    for (struct if_nameindex *n = names; n->if_index != 0 && n->if_name; ++n) {
        struct ifreq req;
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", n->if_name);
        if (ioctl(fd, SIOCGIFFLAGS, &req) != 0) {
            continue;
        }
        const int flags = req.ifr_flags;
        if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK) || !(flags & IFF_BROADCAST)) {
            continue;
        }
        memset(&req.ifr_ifru, 0, sizeof(req.ifr_ifru));
        if (ioctl(fd, SIOCGIFBRDADDR, &req) != 0 || req.ifr_broadaddr.sa_family != AF_INET) {
            continue;
        }
        sockaddr_in in;
        memcpy(&in, &req.ifr_broadaddr, sizeof(in));
        if (in.sin_addr.s_addr != 0) {
            targets->add(in.sin_addr.s_addr, kHsfzIdentPort);
        }
    }
    if_freenameindex(names);
}

void append(char *out, size_t size, const char *text) {
    const size_t used = strlen(out);
    if (used + 1 < size) {
        snprintf(out + used, size - used, "%s", text);
    }
}

} // namespace

void format_ip(uint32_t ip, char *out, size_t size) {
    const uint32_t h = ntohl(ip);
    snprintf(out, size, "%u.%u.%u.%u", (h >> 24) & 255u, (h >> 16) & 255u, (h >> 8) & 255u,
             h & 255u);
}

bool parse_address(const char *text, Gateway *out) {
    unsigned a, b, c, d, port = kHsfzDiagPort;
    char tail = 0;
    if (!text) {
        return false;
    }
    const int n = sscanf(text, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail);
    if (n != 4 && n != 5) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255 || port == 0 || port > 65535) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->ip = htonl(a << 24 | b << 16 | c << 8 | d);
    out->port = static_cast<uint16_t>(port);
    return true;
}

int hsfz_discover(Gateway *out, int max, int timeout_ms, const char *extra, char *searched,
                  size_t searched_size) {
    if (searched && searched_size) {
        searched[0] = '\0';
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    set_nonblocking(fd);
    Targets targets;
    targets.count = 0;
    add_interface_broadcasts(fd, &targets);
    targets.add(htonl(0xA9FEFFFFu), kHsfzIdentPort); // 169.254.255.255: the ENET link
    targets.add(htonl(0xFFFFFFFFu), kHsfzIdentPort);
    Gateway unicast;
    if (extra && *extra && parse_address(extra, &unicast)) {
        targets.add(unicast.ip, unicast.port);
    }
    const uint8_t request[kHsfzHeader] = {0, 0, 0, 0, 0, kHsfzIdent};
    for (int i = 0; i < targets.count; ++i) {
        const ssize_t sent = sendto(fd, request, sizeof(request), 0,
                                    reinterpret_cast<const sockaddr *>(&targets.addr[i]),
                                    sizeof(targets.addr[i]));
        if (searched) {
            char ip[16], item[40];
            format_ip(targets.addr[i].sin_addr.s_addr, ip, sizeof(ip));
            snprintf(item, sizeof(item), "%s%s%s", searched[0] ? " " : "", ip,
                     sent < 0 ? "(!)" : "");
            append(searched, searched_size, item);
        }
    }
    int found = 0;
    const uint64_t deadline = nbt::app::monotonic_ms() + static_cast<uint64_t>(timeout_ms);
    for (int left; (left = remaining(deadline)) > 0 && found < max;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) {
            continue;
        }
        uint8_t buf[512];
        sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from),
                                   &from_len);
        HsfzIdent ident;
        if (n <= 0 || !hsfz_parse_ident(buf, static_cast<size_t>(n), &ident)) {
            continue;
        }
        bool seen = false;
        for (int i = 0; i < found; ++i) {
            seen = seen || out[i].ip == from.sin_addr.s_addr;
        }
        if (!seen) {
            out[found].ip = from.sin_addr.s_addr;
            out[found].port = kHsfzDiagPort;
            out[found].ident = ident;
            ++found;
        }
    }
    ::close(fd);
    return found;
}

HsfzLink::HsfzLink() : fd_(-1), tester_(kTesterAddress), refusal_(0) {
    error_[0] = '\0';
}

HsfzLink::~HsfzLink() {
    close();
}

void HsfzLink::set_error(const char *what, int err) {
    snprintf(error_, sizeof(error_), "%s: %s", what, err ? strerror(err) : "failed");
}

bool HsfzLink::open(const Gateway &gateway, int timeout_ms) {
    close();
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error("socket", errno);
        return false;
    }
    set_nonblocking(fd);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gateway.port ? gateway.port : static_cast<uint16_t>(kHsfzDiagPort));
    addr.sin_addr.s_addr = gateway.ip;
    if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 &&
        errno != EINPROGRESS) {
        set_error("connect", errno);
        ::close(fd);
        return false;
    }
    struct pollfd p = {fd, POLLOUT, 0};
    int err = 0;
    socklen_t len = sizeof(err);
    if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
        err != 0) {
        set_error("connect", err ? err : static_cast<int>(ETIMEDOUT));
        ::close(fd);
        return false;
    }
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    fd_ = fd;
    reader_.reset();
    error_[0] = '\0';
    return true;
}

void HsfzLink::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    reader_.reset();
}

bool HsfzLink::send_all(const uint8_t *data, size_t size) {
    const uint64_t deadline = nbt::app::monotonic_ms() + 1000;
    while (size) {
        const ssize_t n = send(fd_, data, size, 0);
        if (n > 0) {
            data += n;
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd p = {fd_, POLLOUT, 0};
            if (remaining(deadline) > 0 && poll(&p, 1, remaining(deadline)) >= 0) {
                continue;
            }
        }
        set_error("send", n < 0 ? errno : EPIPE);
        return false;
    }
    return true;
}

bool HsfzLink::pump(int timeout_ms) {
    struct pollfd p = {fd_, POLLIN, 0};
    const int ready = poll(&p, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return true;
        }
        set_error("poll", errno);
        return false;
    }
    if (ready == 0) {
        return true;
    }
    uint8_t buf[2048];
    const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
    if (n == 0) {
        set_error("gateway closed the connection", 0);
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true;
        }
        set_error("recv", errno);
        return false;
    }
    if (!reader_.feed(buf, static_cast<size_t>(n))) {
        set_error("frame too large", 0);
        return false;
    }
    return true;
}

HsfzLink::Result HsfzLink::request(uint8_t target, const uint8_t *data, size_t size,
                                   uint8_t *answer, size_t cap, size_t *answer_size,
                                   int timeout_ms) {
    *answer_size = 0;
    refusal_ = 0;
    if (fd_ < 0) {
        set_error("not connected", 0);
        return kBroken;
    }
    uint8_t frame[kHsfzMaxFrame];
    const size_t n = hsfz_diag(tester_, target, data, size, frame, sizeof(frame));
    if (!n || !send_all(frame, n)) {
        return kBroken;
    }
    uint64_t deadline = nbt::app::monotonic_ms() + static_cast<uint64_t>(timeout_ms);
    int pending = 0;
    for (;;) {
        HsfzFrame f;
        while (reader_.next(&f)) {
            switch (f.control) {
            case kHsfzAliveCheck: {
                uint8_t reply[16];
                const size_t r = hsfz_alive_reply(tester_, reply, sizeof(reply));
                if (!send_all(reply, r)) {
                    return kBroken;
                }
                break;
            }
            case kHsfzAck:
                break;
            case kHsfzDiag:
                // From our target to us; anything else is an answer to an
                // earlier request that timed out.
                if (f.size >= 2 && f.payload[0] == target && f.payload[1] == tester_) {
                    const uint8_t *uds = f.payload + 2;
                    const size_t len = f.size - 2;
                    if (len >= 3 && uds[0] == 0x7F && uds[1] == data[0] && uds[2] == 0x78 &&
                        pending < 5) {
                        ++pending;
                        deadline = nbt::app::monotonic_ms() + static_cast<uint64_t>(timeout_ms);
                        break;
                    }
                    // Our answer: the positive service id, or a negative one
                    // for our service.
                    const bool ours = (len >= 1 && uds[0] == (data[0] | 0x40)) ||
                                      (len >= 2 && uds[0] == 0x7F && uds[1] == data[0]);
                    if (!ours) {
                        break;
                    }
                    const size_t copy = len < cap ? len : cap;
                    memcpy(answer, uds, copy);
                    *answer_size = copy;
                    return kOk;
                }
                break;
            case kHsfzBadTester:
            case kHsfzBadControl:
            case kHsfzBadFormat:
            case kHsfzBadTarget:
            case kHsfzTooLarge:
            case kHsfzNotReady:
            case kHsfzNack:
                refusal_ = f.control;
                snprintf(error_, sizeof(error_), "gateway refused the request (control 0x%04x)",
                         f.control);
                return kRefused;
            default:
                break;
            }
        }
        if (reader_.bad()) {
            set_error("malformed frame", 0);
            return kBroken;
        }
        const int left = remaining(deadline);
        if (left <= 0) {
            return kTimeout;
        }
        if (!pump(left)) {
            return kBroken;
        }
    }
}

} // namespace gauges
