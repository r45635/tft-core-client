#include "tftcore/ws.h"

#include <cstdio>
#include <cstring>

#include "tftcore/digest.h"

namespace tftcore {
namespace {

constexpr const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum : uint8_t { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BIN = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xa };

// Enough entropy for a handshake key and frame masks; this is not a security boundary
// (the connection is plain ws:// on the local network).
uint32_t nextRandom(uint32_t seed) {
    static uint32_t s = 0;
    if (!s) s = seed ? seed : 0x9e3779b9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

const char *findHeaderValue(const char *resp, const char *name) {
    size_t nameLen = strlen(name);
    for (const char *p = resp; *p; p++) {
        if ((p == resp || p[-1] == '\n')) {
            size_t i = 0;
            while (i < nameLen) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
                i++;
            }
            if (i == nameLen && p[i] == ':') {
                p += i + 1;
                while (*p == ' ' || *p == '\t') p++;
                return p;
            }
        }
    }
    return nullptr;
}

}  // namespace

WsClient::WsClient(ByteStream &stream, Clock &clock, Alloc &alloc)
    : stream_(stream), clock_(clock), in_(alloc), message_(alloc) {}

void WsClient::fail(const char *why) {
    err_ = why;
    open_ = false;
}

void WsClient::close() {
    if (open_) sendFrame(OP_CLOSE, nullptr, 0);
    open_ = false;
    inMessage_ = false;
    awaitingPong_ = false;
    in_.clear();
    message_.clear();
    message_.shrink();
    stream_.close();
}

bool WsClient::open(const char *host, uint16_t port, const char *path, uint32_t timeoutMs) {
    close();
    err_ = "";
    uint32_t deadline = clock_.nowMs() + timeoutMs;
    if (!stream_.connect(host, port, timeoutMs)) {
        fail("cannot connect");
        return false;
    }
    if (!handshake(host, port, path, deadline)) {
        stream_.close();
        return false;
    }
    open_ = true;
    lastPongAt_ = clock_.nowMs();
    awaitingPong_ = false;
    return true;
}

bool WsClient::handshake(const char *host, uint16_t port, const char *path, uint32_t deadline) {
    uint8_t key[16];
    uint32_t seed = clock_.nowMs() ^ (uint32_t)(uintptr_t)this;
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = nextRandom(seed);
        memcpy(key + i, &r, 4);
    }
    char keyB64[32];
    if (!base64Encode(key, sizeof key, keyB64, sizeof keyB64)) {
        fail("key encoding");
        return false;
    }

    char req[384];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     path, host, (unsigned)port, keyB64);
    if (n <= 0 || (size_t)n >= sizeof req) {
        fail("request too long");
        return false;
    }
    if (stream_.write(reinterpret_cast<const uint8_t *>(req), (size_t)n) < 0) {
        fail("write failed");
        return false;
    }

    // Read until the end of the header block.
    in_.clear();
    while (true) {
        if ((int32_t)(clock_.nowMs() - deadline) >= 0) {
            fail("handshake timeout");
            return false;
        }
        uint8_t chunk[256];
        int got = stream_.read(chunk, sizeof chunk);
        if (got < 0) {
            fail("connection closed during handshake");
            return false;
        }
        if (got > 0 && !in_.append(chunk, (size_t)got)) {
            fail("out of memory");
            return false;
        }
        const char *text = in_.cstr();
        const char *endOfHeaders = strstr(text, "\r\n\r\n");
        if (!endOfHeaders) {
            if (in_.size() > 4096) {
                fail("handshake response too long");
                return false;
            }
            continue;
        }
        if (strncmp(text, "HTTP/1.1 101", 12) != 0 && strncmp(text, "HTTP/1.0 101", 12) != 0) {
            fail("server did not upgrade (wrong port or path?)");
            return false;
        }
        const char *accept = findHeaderValue(text, "sec-websocket-accept");
        if (!accept) {
            fail("no Sec-WebSocket-Accept");
            return false;
        }
        char expectIn[64];
        snprintf(expectIn, sizeof expectIn, "%s%s", keyB64, WS_GUID);
        uint8_t digest[20];
        sha1(reinterpret_cast<const uint8_t *>(expectIn), strlen(expectIn), digest);
        char expect[32];
        base64Encode(digest, sizeof digest, expect, sizeof expect);
        if (strncmp(accept, expect, strlen(expect)) != 0) {
            fail("Sec-WebSocket-Accept mismatch");
            return false;
        }
        // Anything after the headers is already WebSocket data.
        size_t used = (size_t)(endOfHeaders + 4 - text);
        in_.consume(used);
        return true;
    }
}

bool WsClient::sendFrame(uint8_t opcode, const uint8_t *p, size_t n) {
    uint8_t head[14];
    size_t h = 0;
    head[h++] = (uint8_t)(0x80 | opcode);
    if (n < 126) {
        head[h++] = (uint8_t)(0x80 | n);
    } else if (n <= 0xffff) {
        head[h++] = 0x80 | 126;
        head[h++] = (uint8_t)(n >> 8);
        head[h++] = (uint8_t)(n & 0xff);
    } else {
        head[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[h++] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    uint32_t maskWord = nextRandom(clock_.nowMs());
    uint8_t mask[4];
    memcpy(mask, &maskWord, 4);
    memcpy(head + h, mask, 4);
    h += 4;
    if (stream_.write(head, h) < 0) {
        fail("write failed");
        return false;
    }
    if (!n) return true;

    // Masked in chunks, so a big message needs no second copy of itself.
    uint8_t chunk[512];
    size_t done = 0;
    while (done < n) {
        size_t take = n - done;
        if (take > sizeof chunk) take = sizeof chunk;
        for (size_t i = 0; i < take; i++) chunk[i] = p[done + i] ^ mask[(done + i) & 3];
        if (stream_.write(chunk, take) < 0) {
            fail("write failed");
            return false;
        }
        done += take;
    }
    return true;
}

bool WsClient::send(const uint8_t *p, size_t n, bool binary) {
    if (!open_) return false;
    return sendFrame(binary ? OP_BIN : OP_TEXT, p, n);
}

bool WsClient::readMore() {
    uint8_t chunk[1024];
    for (int i = 0; i < 8; i++) {            // bounded, so poll() always returns
        int got = stream_.read(chunk, sizeof chunk);
        if (got < 0) {
            fail("connection closed");
            return false;
        }
        if (got == 0) return true;
        if (!in_.append(chunk, (size_t)got)) {
            fail("out of memory");
            return false;
        }
    }
    return true;
}

// Takes one frame out of `in_` if a whole one is there. `again` = there may be more.
bool WsClient::takeFrame(MessageCb cb, void *ctx, bool &again) {
    again = false;
    if (in_.size() < 2) return true;
    const uint8_t *b = in_.data();
    bool fin = (b[0] & 0x80) != 0;
    uint8_t opcode = b[0] & 0x0f;
    bool masked = (b[1] & 0x80) != 0;
    uint64_t len = b[1] & 0x7f;
    size_t pos = 2;
    if (len == 126) {
        if (in_.size() < 4) return true;
        len = ((uint64_t)b[2] << 8) | b[3];
        pos = 4;
    } else if (len == 127) {
        if (in_.size() < 10) return true;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
        pos = 10;
    }
    if (masked) pos += 4;   // a server must not mask, but skip the key rather than desync
    if (len > maxMessage_) {
        fail("frame larger than the limit");
        return false;
    }
    if (in_.size() < pos + len) return true;   // wait for the rest

    const uint8_t *payload = in_.data() + pos;
    if (opcode == OP_PING) {
        if (!sendFrame(OP_PONG, payload, (size_t)len)) return false;
    } else if (opcode == OP_PONG) {
        lastPongAt_ = clock_.nowMs();
        awaitingPong_ = false;
    } else if (opcode == OP_CLOSE) {
        sendFrame(OP_CLOSE, nullptr, 0);
        fail("server closed the connection");
        in_.consume(pos + (size_t)len);
        return false;
    } else if (opcode == OP_TEXT || opcode == OP_BIN || opcode == OP_CONT) {
        if (opcode != OP_CONT) {
            message_.clear();
            msgBinary_ = (opcode == OP_BIN);
            inMessage_ = true;
        }
        if (!inMessage_) {                      // continuation with no start: desynchronised
            fail("unexpected continuation frame");
            return false;
        }
        if (message_.size() + len > maxMessage_) {
            fail("message larger than the limit");
            return false;
        }
        if (!message_.append(payload, (size_t)len)) {
            fail("out of memory");
            return false;
        }
        if (fin) {
            inMessage_ = false;
            // The callback may send on this socket, so consume before calling it.
            in_.consume(pos + (size_t)len);
            if (cb) cb(ctx, message_.data(), message_.size(), msgBinary_);
            again = true;
            return true;
        }
    }
    in_.consume(pos + (size_t)len);
    again = true;
    return true;
}

bool WsClient::poll(MessageCb cb, void *ctx) {
    if (!open_) return false;
    if (!readMore()) return false;
    bool again = true;
    while (again && open_) {
        if (!takeFrame(cb, ctx, again)) return false;
    }

    // One ping at a time: sending a new one while the last is unanswered would keep moving
    // the deadline, and a dead connection would never be noticed.
    uint32_t now = clock_.nowMs();
    if (awaitingPong_) {
        if ((int32_t)(now - pingSentAt_) > (int32_t)PONG_TIMEOUT_MS) {
            fail("no pong: connection is dead");
            return false;
        }
    } else if ((int32_t)(now - lastPongAt_) >= (int32_t)PING_PERIOD_MS) {
        pingSentAt_ = now;
        awaitingPong_ = true;
        if (!sendFrame(OP_PING, nullptr, 0)) return false;
    }
    return true;
}

}  // namespace tftcore
