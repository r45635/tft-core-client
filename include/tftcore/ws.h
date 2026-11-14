// Minimal RFC 6455 client: binary masked frames, fragment reassembly, ping/pong keepalive.
// No TLS (the protocol it carries is plain ws:// on the local network).
#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer.h"
#include "io.h"

namespace tftcore {

class WsClient {
  public:
    WsClient(ByteStream &stream, Clock &clock, Alloc &alloc = defaultAlloc());

    // Connects and completes the upgrade handshake (blocking, with a deadline). The
    // Sec-WebSocket-Accept value is verified, so a wrong port fails here instead of later.
    bool open(const char *host, uint16_t port, const char *path, uint32_t timeoutMs = 5000);
    bool isOpen() const { return open_; }
    void close();

    bool send(const uint8_t *p, size_t n, bool binary = true);

    // One complete message. `len` can be 0. The bytes belong to the client and are only valid
    // during the call.
    using MessageCb = void (*)(void *ctx, const uint8_t *msg, size_t len, bool binary);
    // Reads what is available, calls `cb` for each complete message, sends the keepalive ping
    // and enforces the pong deadline. false = the connection is gone (call close()).
    bool poll(MessageCb cb, void *ctx);

    // Largest message accepted; a bigger one closes the connection instead of eating the heap.
    void setMaxMessage(size_t bytes) { maxMessage_ = bytes; }
    const char *lastError() const { return err_; }

    static constexpr uint32_t PING_PERIOD_MS = 10000;
    static constexpr uint32_t PONG_TIMEOUT_MS = 10000;

  private:
    bool handshake(const char *host, uint16_t port, const char *path, uint32_t deadline);
    bool sendFrame(uint8_t opcode, const uint8_t *p, size_t n);
    bool readMore();
    bool takeFrame(MessageCb cb, void *ctx, bool &again);
    void fail(const char *why);

    ByteStream &stream_;
    Clock &clock_;
    Buffer in_;          // bytes read, not yet parsed into frames
    Buffer message_;     // current message, reassembled across fragments
    bool open_ = false;
    bool msgBinary_ = false;
    bool inMessage_ = false;
    size_t maxMessage_ = 512 * 1024;
    uint32_t pingSentAt_ = 0, lastPongAt_ = 0;
    bool awaitingPong_ = false;
    const char *err_ = "";
};

}  // namespace tftcore
