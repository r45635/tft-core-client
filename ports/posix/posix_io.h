// Host implementations of the three interfaces, for tests and the command-line example.
#pragma once

#include <cstdint>

#include "tftcore/io.h"

namespace tftcore::posix {

class TcpStream : public ByteStream {
  public:
    ~TcpStream() override { close(); }
    bool connect(const char *host, uint16_t port, uint32_t timeoutMs) override;
    bool connected() const override { return fd_ >= 0; }
    int read(uint8_t *buf, size_t n) override;
    int write(const uint8_t *buf, size_t n) override;
    void close() override;

  private:
    int fd_ = -1;
};

class Udp : public UdpSocket {
  public:
    ~Udp() override { close(); }
    bool open() override;
    bool sendTo(const char *ip, uint16_t port, const uint8_t *buf, size_t n) override;
    bool sendBroadcast(uint16_t port, const uint8_t *buf, size_t n) override;
    int recvFrom(uint8_t *buf, size_t n, char *fromIp, size_t fromIpLen) override;
    bool localAddress(char *out, size_t outLen) override;
    void close() override;

  private:
    int fd_ = -1;
};

class SteadyClock : public Clock {
  public:
    uint32_t nowMs() override;
};

}  // namespace tftcore::posix
