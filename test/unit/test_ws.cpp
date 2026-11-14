#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "tftcore/digest.h"
#include "tftcore/ws.h"

using namespace tftcore;

namespace {

// A stream the test drives from both ends: what the client wrote, and what it will read next.
class FakeStream : public ByteStream {
  public:
    bool connect(const char *, uint16_t, uint32_t) override {
        open_ = true;
        return connectOk;
    }
    bool connected() const override { return open_; }
    int read(uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        if (readPos >= toClient.size()) return 0;
        size_t take = toClient.size() - readPos;
        if (take > n) take = n;
        if (take > chunk) take = chunk;   // delivered in pieces, as a socket would
        memcpy(buf, toClient.data() + readPos, take);
        readPos += take;
        return (int)take;
    }
    int write(const uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        fromClient.insert(fromClient.end(), buf, buf + n);
        return (int)n;
    }
    void close() override { open_ = false; }

    std::vector<uint8_t> toClient, fromClient;
    size_t readPos = 0, chunk = 4096;
    bool connectOk = true;
    bool open_ = false;
};

class FakeClock : public Clock {
  public:
    uint32_t nowMs() override { return now; }
    uint32_t now = 1000;
};

// The 101 answer a server sends, with the accept value computed from the key the client chose.
std::string handshakeReply(const std::vector<uint8_t> &clientWrote, const char *extraHeader = "") {
    std::string req((const char *)clientWrote.data(), clientWrote.size());
    size_t at = req.find("Sec-WebSocket-Key: ");
    if (at == std::string::npos) return "HTTP/1.1 400 Bad Request\r\n\r\n";
    at += strlen("Sec-WebSocket-Key: ");
    size_t end = req.find("\r\n", at);
    std::string key = req.substr(at, end - at);
    std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    sha1((const uint8_t *)material.data(), material.size(), digest);
    char accept[32];
    base64Encode(digest, sizeof digest, accept, sizeof accept);
    return std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n") +
           "Sec-WebSocket-Accept: " + accept + "\r\n" + extraHeader + "\r\n";
}

// A server frame: never masked, opcode and fin as given.
std::vector<uint8_t> serverFrame(uint8_t opcode, const std::string &payload, bool fin = true) {
    std::vector<uint8_t> f;
    f.push_back((uint8_t)((fin ? 0x80 : 0) | opcode));
    if (payload.size() < 126) {
        f.push_back((uint8_t)payload.size());
    } else {
        f.push_back(126);
        f.push_back((uint8_t)(payload.size() >> 8));
        f.push_back((uint8_t)(payload.size() & 0xff));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void append(std::vector<uint8_t> &dst, const std::vector<uint8_t> &src) { dst.insert(dst.end(), src.begin(), src.end()); }
void append(std::vector<uint8_t> &dst, const std::string &src) { dst.insert(dst.end(), src.begin(), src.end()); }

// Answers the handshake as soon as the client has written its request, so the test never has
// to know which random key the client picked.
class LazyStream : public FakeStream {
  public:
    int read(uint8_t *buf, size_t n) override {
        if (!primed && !fromClient.empty()) {
            std::string reply = handshakeReply(fromClient);
            toClient.insert(toClient.end(), reply.begin(), reply.end());
            primed = true;
        }
        return FakeStream::read(buf, n);
    }
    bool primed = false;
};

struct Collected {
    std::vector<std::string> messages;
    std::vector<bool> binary;
};
void collect(void *ctx, const uint8_t *msg, size_t len, bool bin) {
    auto *c = static_cast<Collected *>(ctx);
    c->messages.push_back(std::string((const char *)msg, len));
    c->binary.push_back(bin);
}

// Reads one client frame at `pos`, unmasks it, and returns its opcode and payload.
bool readClientFrame(const std::vector<uint8_t> &buf, size_t &pos, uint8_t &opcode, std::string &payload) {
    if (pos + 2 > buf.size()) return false;
    opcode = buf[pos] & 0x0f;
    bool masked = (buf[pos + 1] & 0x80) != 0;
    size_t len = buf[pos + 1] & 0x7f;
    size_t p = pos + 2;
    if (len == 126) {
        len = ((size_t)buf[p] << 8) | buf[p + 1];
        p += 2;
    }
    if (!masked) return false;   // a client must mask every frame
    uint8_t mask[4] = {buf[p], buf[p + 1], buf[p + 2], buf[p + 3]};
    p += 4;
    if (p + len > buf.size()) return false;
    payload.clear();
    for (size_t i = 0; i < len; i++) payload.push_back((char)(buf[p + i] ^ mask[i & 3]));
    pos = p + len;
    return true;
}

}  // namespace

void testWs() {
    // A handshake, a message, and the masking of what the client sends.
    {
        LazyStream lazy;
        FakeClock c;
        lazy.chunk = 3;   // a few bytes per read: the handshake must not depend on chunk sizes
        WsClient ws(lazy, c);
        CHECK(ws.open("host", 9330, "/api", 1000));
        CHECK(ws.isOpen());
        std::string req((const char *)lazy.fromClient.data(), lazy.fromClient.size());
        CHECK(req.find("GET /api HTTP/1.1") == 0);
        CHECK(req.find("Host: host:9330") != std::string::npos);
        CHECK(req.find("Sec-WebSocket-Version: 13") != std::string::npos);

        // What the client sends is masked, and arrives unchanged once unmasked.
        lazy.fromClient.clear();
        const char *payload = "MOO/1 REQUEST x\nRequest-Id: 1\n\n";
        CHECK(ws.send((const uint8_t *)payload, strlen(payload), true));
        size_t pos = 0;
        uint8_t opcode = 0;
        std::string got;
        CHECK(readClientFrame(lazy.fromClient, pos, opcode, got));
        CHECK_INT(opcode, 0x2);
        CHECK_STR(got.c_str(), payload);

        // Two messages in one read, the second split across frames.
        Collected col;
        lazy.toClient.clear();
        lazy.readPos = 0;
        lazy.chunk = 4096;
        append(lazy.toClient, serverFrame(0x2, "first"));
        append(lazy.toClient, serverFrame(0x2, "se", false));
        append(lazy.toClient, serverFrame(0x0, "cond"));
        CHECK(ws.poll(collect, &col));
        CHECK_INT(col.messages.size(), 2);
        if (col.messages.size() == 2) {
            CHECK_STR(col.messages[0].c_str(), "first");
            CHECK_STR(col.messages[1].c_str(), "second");
            CHECK(col.binary[0]);
        }

        // A server ping is answered with a pong carrying the same bytes.
        lazy.fromClient.clear();
        lazy.toClient.clear();
        lazy.readPos = 0;
        append(lazy.toClient, serverFrame(0x9, "hi"));
        CHECK(ws.poll(collect, &col));
        pos = 0;
        CHECK(readClientFrame(lazy.fromClient, pos, opcode, got));
        CHECK_INT(opcode, 0xa);
        CHECK_STR(got.c_str(), "hi");

        // A close frame ends the connection.
        lazy.toClient.clear();
        lazy.readPos = 0;
        append(lazy.toClient, serverFrame(0x8, ""));
        CHECK(!ws.poll(collect, &col));
        CHECK(!ws.isOpen());
    }

    // A server that does not upgrade fails the handshake instead of sending frames into the void.
    {
        FakeStream s;
        FakeClock c;
        WsClient ws(s, c);
        append(s.toClient, std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"));
        CHECK(!ws.open("host", 80, "/api", 1000));
        CHECK(!ws.isOpen());
        CHECK(strstr(ws.lastError(), "upgrade") != nullptr);
    }

    // A wrong Sec-WebSocket-Accept is refused (wrong port, or something else speaking HTTP).
    {
        FakeStream s;
        FakeClock c;
        WsClient ws(s, c);
        append(s.toClient, std::string("HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: nope\r\n\r\n"));
        CHECK(!ws.open("host", 9330, "/api", 1000));
        CHECK(strstr(ws.lastError(), "mismatch") != nullptr);
    }

    // No pong before the deadline closes the connection.
    {
        LazyStream lazy;
        FakeClock c;
        WsClient ws(lazy, c);
        CHECK(ws.open("host", 9330, "/api", 1000));
        Collected col;
        c.now += WsClient::PING_PERIOD_MS;      // a ping goes out
        CHECK(ws.poll(collect, &col));
        c.now += WsClient::PONG_TIMEOUT_MS + 1; // and is never answered
        CHECK(!ws.poll(collect, &col));
        CHECK(strstr(ws.lastError(), "pong") != nullptr);
    }

    // A message over the limit closes the connection instead of growing the heap.
    {
        LazyStream lazy;
        FakeClock c;
        WsClient ws(lazy, c);
        CHECK(ws.open("host", 9330, "/api", 1000));
        ws.setMaxMessage(100);
        Collected col;
        lazy.toClient.clear();
        lazy.readPos = 0;
        append(lazy.toClient, serverFrame(0x2, std::string(200, 'x')));
        CHECK(!ws.poll(collect, &col));
        CHECK_INT(col.messages.size(), 0);
    }
}
