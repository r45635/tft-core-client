// Registration timing. A client that already holds a token must not tell its user to enable the
// extension while the server is simply still answering: that instruction was shown at every boot
// of a paired panel (TFT Panel, 2026-11-20), because the wait before it was no wait at all.
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "tftcore/client.h"
#include "tftcore/digest.h"

using namespace tftcore;

namespace {

// Time advances by a millisecond on every reading, so a busy wait inside the client always ends.
class TickClock : public Clock {
  public:
    uint32_t nowMs() override { return now += 1; }
    uint32_t now = 100000;
};

struct NoUdp : UdpSocket {
    bool open() override { return true; }
    bool sendTo(const char *, uint16_t, const uint8_t *, size_t) override { return true; }
    bool sendBroadcast(uint16_t, const uint8_t *, size_t) override { return true; }
    int recvFrom(uint8_t *, size_t, char *, size_t) override { return 0; }
    void close() override {}
};

std::string mooComplete(const char *name, int id, const char *json) {
    std::string s = "MOO/1 COMPLETE ";
    s += name;
    s += "\nRequest-Id: " + std::to_string(id) + "\n";
    if (json) {
        s += "Content-Length: " + std::to_string(strlen(json)) + "\n";
        s += "Content-Type: application/json\n";
    }
    s += "\n";
    if (json) s += json;
    return s;
}

// A server-side frame: text, never masked.
std::vector<uint8_t> serverFrame(const std::string &payload) {
    std::vector<uint8_t> f;
    f.push_back(0x81);
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

// Enough of a Core to pair with: answers the handshake, answers registry/info at once, and
// answers the registration after `registerDelayMs` of its own clock (never, if negative).
class FakeCore : public ByteStream {
  public:
    FakeCore(TickClock &clock) : clock_(clock) {}

    bool connect(const char *, uint16_t, uint32_t) override {
        open_ = true;
        return true;
    }
    bool connected() const override { return open_; }
    int read(uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        serve();
        if (pos_ >= out_.size()) return 0;
        size_t take = out_.size() - pos_;
        if (take > n) take = n;
        memcpy(buf, out_.data() + pos_, take);
        pos_ += take;
        return (int)take;
    }
    int write(const uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        in_.insert(in_.end(), buf, buf + n);
        return (int)n;
    }
    void close() override { open_ = false; }

    int32_t registerDelayMs = 0;
    const char *coreId = "core-1";
    std::string registerBody;      // what the client asked for, for the test to inspect

  private:
    void serve() {
        if (!handshook_) {
            std::string req((const char *)in_.data(), in_.size());
            size_t at = req.find("Sec-WebSocket-Key: ");
            if (at == std::string::npos || req.find("\r\n\r\n") == std::string::npos) return;
            at += strlen("Sec-WebSocket-Key: ");
            std::string key = req.substr(at, req.find("\r\n", at) - at);
            std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            uint8_t digest[20];
            sha1((const uint8_t *)material.data(), material.size(), digest);
            char accept[32];
            base64Encode(digest, sizeof digest, accept, sizeof accept);
            std::string reply =
                std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n") +
                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
            out_.insert(out_.end(), reply.begin(), reply.end());
            handshook_ = true;
            consumed_ = req.find("\r\n\r\n") + 4;
        }
        readRequests();
        for (auto it = later_.begin(); it != later_.end();) {
            if ((int32_t)(clock_.now - it->first) >= 0) {
                out_.insert(out_.end(), it->second.begin(), it->second.end());
                it = later_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Unmasks every complete client frame written since the last call and answers it.
    void readRequests() {
        while (consumed_ + 2 <= in_.size()) {
            size_t p = consumed_;
            size_t len = in_[p + 1] & 0x7f;
            size_t hdr = 2;
            if (len == 126) {
                if (in_.size() < p + 4) return;
                len = ((size_t)in_[p + 2] << 8) | in_[p + 3];
                hdr = 4;
            }
            if ((in_[p + 1] & 0x80) == 0) return;      // a client must mask
            if (in_.size() < p + hdr + 4 + len) return;
            const uint8_t *mask = &in_[p + hdr];
            std::string payload;
            for (size_t i = 0; i < len; i++) payload.push_back((char)(in_[p + hdr + 4 + i] ^ mask[i & 3]));
            consumed_ = p + hdr + 4 + len;
            answer(payload);
        }
    }

    void answer(const std::string &msg) {
        if (msg.compare(0, 4, "MOO/") != 0) return;
        size_t at = msg.find("Request-Id: ");
        if (at == std::string::npos) return;
        int id = atoi(msg.c_str() + at + strlen("Request-Id: "));
        if (msg.find("/info") != std::string::npos) {
            std::string body = std::string("{\"core_id\":\"") + coreId + "\",\"display_name\":\"Fake Core\"}";
            auto f = serverFrame(mooComplete("Success", id, body.c_str()));
            out_.insert(out_.end(), f.begin(), f.end());
        } else if (msg.find("/register") != std::string::npos) {
            size_t body = msg.find("\n\n");
            registerBody = body == std::string::npos ? "" : msg.substr(body + 2);
            if (registerDelayMs < 0) return;           // the user never enables the extension
            std::string json = std::string("{\"core_id\":\"") + coreId +
                               "\",\"token\":\"tok-2\",\"display_name\":\"Fake Core\"}";
            later_.push_back({clock_.now + (uint32_t)registerDelayMs,
                              serverFrame(mooComplete("Registered", id, json.c_str()))});
        }
    }

    TickClock &clock_;
    std::vector<uint8_t> in_, out_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> later_;
    size_t pos_ = 0, consumed_ = 0;
    bool open_ = false, handshook_ = false;
};

struct Seen {
    std::vector<State> states;
};
void onState(void *ctx, State s, const char *) { static_cast<Seen *>(ctx)->states.push_back(s); }
bool sawState(const Seen &seen, State s) {
    for (State x : seen.states)
        if (x == s) return true;
    return false;
}

ExtensionInfo info() {
    ExtensionInfo i;
    i.extensionId = "com.example.test";
    i.displayName = "Test Client";
    i.displayVersion = "v0";
    i.publisher = "test";
    i.email = "test@example.com";
    return i;
}

// Runs the client until it stops changing state, as a firmware loop would.
State settle(Client &c, int ticks = 200) {
    for (int i = 0; i < ticks; i++) {
        c.tick();
        if (c.state() == State::Paired || c.state() == State::NeedsApproval ||
            c.state() == State::Failed)
            break;
    }
    return c.state();
}

}  // namespace

void testClient() {
    ExtensionInfo ext = info();

    // A saved token and a server that answers four seconds later: paired, and the user is never
    // asked for an approval already given. This is the regression: the wait used to be skipped
    // entirely, so the state went to NeedsApproval before the answer could arrive.
    {
        TickClock clock;
        FakeCore core(clock);
        NoUdp udp;
        core.registerDelayMs = 4000;
        Client c(core, udp, clock);
        Seen seen;
        c.onState(onState, &seen);
        c.begin(ext);
        c.setServer("10.0.0.1", 9330);
        c.setToken("core-1", "tok-1");
        CHECK_INT((int)settle(c), (int)State::Paired);
        CHECK(!sawState(seen, State::NeedsApproval));
        CHECK(core.registerBody.find("\"token\":\"tok-1\"") != std::string::npos);
    }

    // No token: a registration that stays pending is a person who has not enabled the extension
    // yet, and saying so after a couple of seconds is right.
    {
        TickClock clock;
        FakeCore core(clock);
        NoUdp udp;
        core.registerDelayMs = -1;
        Client c(core, udp, clock);
        c.begin(ext);
        c.setServer("10.0.0.1", 9330);
        CHECK_INT((int)settle(c), (int)State::NeedsApproval);
        CHECK(core.registerBody.find("token") == std::string::npos);
    }

    // A token the server never answers for: the message comes anyway, just not at once.
    {
        TickClock clock;
        FakeCore core(clock);
        NoUdp udp;
        core.registerDelayMs = -1;
        Client c(core, udp, clock);
        c.begin(ext);
        c.setServer("10.0.0.1", 9330);
        c.setToken("core-1", "tok-1");
        CHECK_INT((int)settle(c), (int)State::NeedsApproval);
    }
}
