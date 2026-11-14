// Everything platform-specific the library needs, as three small interfaces.
// Implementations: ports/posix (host builds and tests), ports/arduino (ESP32).
#pragma once

#include <cstddef>
#include <cstdint>

namespace tftcore {

// A TCP byte stream. read() must not block: 0 means "nothing available right now".
struct ByteStream {
    virtual ~ByteStream() = default;
    virtual bool connect(const char *host, uint16_t port, uint32_t timeoutMs) = 0;
    virtual bool connected() const = 0;
    virtual int read(uint8_t *buf, size_t n) = 0;          // >0 bytes, 0 none, <0 error
    virtual int write(const uint8_t *buf, size_t n) = 0;   // all or <0; short writes are errors
    virtual void close() = 0;
};

// UDP, for discovery: one socket that can send to a multicast group and to a broadcast
// address, and read the replies.
struct UdpSocket {
    virtual ~UdpSocket() = default;
    virtual bool open() = 0;        // bind an ephemeral port, enable broadcast, TTL 1
    virtual bool sendTo(const char *ip, uint16_t port, const uint8_t *buf, size_t n) = 0;
    // Sends to the broadcast address of every usable interface. Networks that drop multicast
    // (plenty of access points do) still answer this one.
    virtual bool sendBroadcast(uint16_t port, const uint8_t *buf, size_t n) = 0;
    // >0 = bytes read and `fromIp` filled, 0 = nothing waiting, <0 = error.
    // An implementation may wait a few milliseconds inside this call rather than spin.
    virtual int recvFrom(uint8_t *buf, size_t n, char *fromIp, size_t fromIpLen) = 0;
    // Our own address on the network we are discovering over, so a reply from an unrelated
    // interface of the server can be told apart from one we can actually reach.
    // false = unknown, and every candidate is then judged on its address alone.
    virtual bool localAddress(char *out, size_t outLen) {
        (void)out;
        (void)outLen;
        return false;
    }
    virtual void close() = 0;
};

struct Clock {
    virtual ~Clock() = default;
    virtual uint32_t nowMs() = 0;   // monotonic, wrap-around tolerated (compare by difference)
};

}  // namespace tftcore
