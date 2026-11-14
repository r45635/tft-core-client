// ESP32 (Arduino core 3.x) implementations of the three interfaces, plus a PSRAM allocator.
// Header-only on purpose: PlatformIO compiles only src/, so nothing here reaches a host build.
#pragma once

#if defined(ARDUINO) || defined(ESP_PLATFORM)

#include <Arduino.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>

#include "tftcore/alloc.h"
#include "tftcore/io.h"

namespace tftcore::arduino {

class TcpStream : public ByteStream {
  public:
    bool connect(const char *host, uint16_t port, uint32_t timeoutMs) override {
        close();
        client_.setTimeout(timeoutMs / 1000 ? timeoutMs / 1000 : 1);
        if (!client_.connect(host, port, timeoutMs)) return false;
        client_.setNoDelay(true);
        return true;
    }
    bool connected() const override { return const_cast<NetworkClient &>(client_).connected(); }
    int read(uint8_t *buf, size_t n) override {
        int available = client_.available();
        if (available <= 0) return client_.connected() ? 0 : -1;
        size_t take = (size_t)available < n ? (size_t)available : n;
        int got = client_.read(buf, take);
        return got < 0 ? -1 : got;
    }
    int write(const uint8_t *buf, size_t n) override {
        size_t sent = client_.write(buf, n);
        return sent == n ? (int)sent : -1;
    }
    void close() override { client_.stop(); }

  private:
    NetworkClient client_;
};

class Udp : public UdpSocket {
  public:
    bool open() override {
        close();
        return udp_.begin(0) == 1;
    }
    bool sendTo(const char *ip, uint16_t port, const uint8_t *buf, size_t n) override {
        IPAddress addr;
        if (!addr.fromString(ip)) return false;
        if (!udp_.beginPacket(addr, port)) return false;
        udp_.write(buf, n);
        return udp_.endPacket() == 1;
    }
    bool sendBroadcast(uint16_t port, const uint8_t *buf, size_t n) override {
        IPAddress broadcast = WiFi.broadcastIP();
        if (!udp_.beginPacket(broadcast, port)) return false;
        udp_.write(buf, n);
        return udp_.endPacket() == 1;
    }
    int recvFrom(uint8_t *buf, size_t n, char *fromIp, size_t fromIpLen) override {
        int size = udp_.parsePacket();
        if (size <= 0) {
            delay(5);   // yields the core instead of spinning through the discovery window
            return 0;
        }
        int got = udp_.read(buf, n);
        if (got <= 0) return 0;
        if (fromIp && fromIpLen) snprintf(fromIp, fromIpLen, "%s", udp_.remoteIP().toString().c_str());
        return got;
    }
    bool localAddress(char *out, size_t outLen) override {
        if (WiFi.status() != WL_CONNECTED) return false;
        snprintf(out, outLen, "%s", WiFi.localIP().toString().c_str());
        return true;
    }
    void close() override { udp_.stop(); }

  private:
    WiFiUDP udp_;
};

class MillisClock : public Clock {
  public:
    uint32_t nowMs() override { return millis(); }
};

// On a board with PSRAM, internal RAM is what the Wi-Fi stack needs: keep every buffer and
// JSON document of this library out of it.
class PsramAlloc : public Alloc {
  public:
    void *alloc(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    void *realloc(void *p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void release(void *p) override { heap_caps_free(p); }
};

}  // namespace tftcore::arduino

#endif  // ARDUINO
