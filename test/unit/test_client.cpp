#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "tftcore/client.h"
#include "tftcore/digest.h"

using namespace tftcore;

namespace {

class FakeClock : public Clock {
  public:
    uint32_t nowMs() override { return now += 1; }   // time moves on, so a missing answer times out
    uint32_t now = 1000;
};

class NoUdp : public UdpSocket {
  public:
    bool open() override { return false; }
    bool sendTo(const char *, uint16_t, const uint8_t *, size_t) override { return false; }
    bool sendBroadcast(uint16_t, const uint8_t *, size_t) override { return false; }
    int recvFrom(uint8_t *, size_t, char *, size_t) override { return -1; }
    void close() override {}
};

struct Request {
    std::string name, body;
    int id;
};

// A server at the far end of the stream: it answers the WebSocket handshake, unmasks what the
// client sends, and answers the registry and the subscriptions the way a Core does.
class FakeCore : public ByteStream {
  public:
    bool connect(const char *, uint16_t, uint32_t) override {
        open_ = true;
        handshakeDone_ = false;
        fromClient_.clear();
        toClient_.clear();
        readPos_ = 0;
        connects++;
        return true;
    }
    bool connected() const override { return open_; }
    int write(const uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        fromClient_.insert(fromClient_.end(), buf, buf + n);
        return (int)n;
    }
    int read(uint8_t *buf, size_t n) override {
        if (!open_) return -1;
        serve();
        if (readPos_ >= toClient_.size()) return 0;
        size_t take = toClient_.size() - readPos_;
        if (take > n) take = n;
        memcpy(buf, toClient_.data() + readPos_, take);
        readPos_ += take;
        return (int)take;
    }
    void close() override { open_ = false; }

    // A message the server sends on its own: a subscription update.
    void push(const char *verb, const char *name, int requestId, const std::string &body) {
        std::string m = "MOO/1 " + std::string(verb) + " " + name + "\nRequest-Id: " + std::to_string(requestId) + "\n";
        if (!body.empty())
            m += "Content-Length: " + std::to_string(body.size()) + "\nContent-Type: application/json\n";
        m += "\n" + body;
        frame(m);
    }

    // The last request of that name; id -1 when there was none.
    Request last(const char *name) const {
        for (size_t i = requests.size(); i-- > 0;)
            if (requests[i].name == name) return requests[i];
        return {"", "", -1};
    }
    int count(const char *name, size_t since = 0) const {
        int n = 0;
        for (size_t i = since; i < requests.size(); i++) n += requests[i].name == name;
        return n;
    }

    std::vector<Request> requests;
    std::string queueItems = R"([{"queue_item_id":1,"one_line":{"line1":"One"}},
                                 {"queue_item_id":2,"one_line":{"line1":"Two"}}])";
    const char *queueAnswer = "Subscribed";   // or an error name, as for an unknown zone
    int connects = 0;

  private:
    void frame(const std::string &payload) {
        toClient_.push_back(0x82);
        if (payload.size() < 126) {
            toClient_.push_back((uint8_t)payload.size());
        } else {
            toClient_.push_back(126);
            toClient_.push_back((uint8_t)(payload.size() >> 8));
            toClient_.push_back((uint8_t)(payload.size() & 0xff));
        }
        toClient_.insert(toClient_.end(), payload.begin(), payload.end());
    }

    void serve() {
        if (!handshakeDone_) {
            std::string req((const char *)fromClient_.data(), fromClient_.size());
            size_t end = req.find("\r\n\r\n");
            if (end == std::string::npos) return;
            size_t at = req.find("Sec-WebSocket-Key: ") + strlen("Sec-WebSocket-Key: ");
            std::string key = req.substr(at, req.find("\r\n", at) - at) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            uint8_t digest[20];
            sha1((const uint8_t *)key.data(), key.size(), digest);
            char accept[32];
            base64Encode(digest, sizeof digest, accept, sizeof accept);
            std::string reply = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n") +
                                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
            toClient_.insert(toClient_.end(), reply.begin(), reply.end());
            fromClient_.erase(fromClient_.begin(), fromClient_.begin() + (long)(end + 4));
            handshakeDone_ = true;
        }
        std::string payload;
        while (takeFrame(payload)) answer(payload);
    }

    bool takeFrame(std::string &payload) {
        if (fromClient_.size() < 2) return false;
        size_t len = fromClient_[1] & 0x7f, p = 2;
        if (len == 126) {
            if (fromClient_.size() < 4) return false;
            len = ((size_t)fromClient_[2] << 8) | fromClient_[3];
            p = 4;
        }
        if (fromClient_.size() < p + 4 + len) return false;
        const uint8_t *mask = &fromClient_[p];
        payload.clear();
        for (size_t i = 0; i < len; i++) payload.push_back((char)(fromClient_[p + 4 + i] ^ mask[i & 3]));
        fromClient_.erase(fromClient_.begin(), fromClient_.begin() + (long)(p + 4 + len));
        return true;
    }

    void answer(const std::string &msg) {
        MooHeader h;
        if (!mooParse((const uint8_t *)msg.data(), msg.size(), h) || h.verb != MooVerb::Request) return;
        std::string name = std::string(h.service) + "/" + h.name;
        requests.push_back({name, msg.substr(h.bodyOffset, h.contentLength), h.requestId});
        int id = h.requestId;
        if (name == "com.roonlabs.registry:1/info")
            push("COMPLETE", "Success", id, R"({"core_id":"c1","display_name":"Core"})");
        else if (name == "com.roonlabs.registry:1/register")
            push("COMPLETE", "Registered", id, R"({"core_id":"c1","display_name":"Core","token":"t"})");
        else if (name == "com.roonlabs.transport:2/subscribe_zones")
            push("CONTINUE", "Subscribed", id, R"({"zones":[]})");
        else if (name == "com.roonlabs.transport:2/subscribe_queue")
            push(strcmp(queueAnswer, "Subscribed") ? "COMPLETE" : "CONTINUE", queueAnswer, id,
                 strcmp(queueAnswer, "Subscribed") ? "" : R"({"items":)" + queueItems + "}");
        else if (name == "com.roonlabs.transport:2/unsubscribe_queue")
            push("COMPLETE", "Success", id, "");
        else
            push("COMPLETE", "Success", id, "");
    }

    std::vector<uint8_t> fromClient_, toClient_;
    size_t readPos_ = 0;
    bool open_ = false, handshakeDone_ = false;
};

struct Seen {
    int calls = 0;
    std::string target, ids;
};
void queueChanged(void *ctx, const char *zoneOrOutputId, const QueueList &q) {
    auto *s = static_cast<Seen *>(ctx);
    s->calls++;
    s->target = zoneOrOutputId;
    s->ids.clear();
    for (const QueueItem &it : q) s->ids += (s->ids.empty() ? "" : ",") + std::to_string(it.id);
}

ExtensionInfo info() {
    ExtensionInfo i;
    i.extensionId = "com.example.test";
    i.displayName = "Test";
    i.displayVersion = "1";
    i.publisher = "Me";
    i.email = "me@example.com";
    return i;
}

void ticks(Client &c, int n = 5) {
    for (int i = 0; i < n; i++) c.tick();
}

// The subscription key a request carried, to address its updates.
std::string keyOf(const Request &r) {
    if (r.id < 0) return "";
    JsonDocument doc;
    deserializeJson(doc, r.body);
    return doc["subscription_key"] | "";
}

}  // namespace

void testClient() {
    static const ExtensionInfo ext = info();

    // The followed queue is subscribed once paired, delivered, and kept up to date.
    {
        FakeCore core;
        NoUdp udp;
        FakeClock clock;
        Client c(core, udp, clock);
        Seen seen;
        c.onQueue(queueChanged, &seen);
        CHECK(c.followQueue("z1", 10));
        c.setServer("core", 9330);
        c.begin(ext);
        ticks(c);
        CHECK(c.paired());
        Request sub = core.last("com.roonlabs.transport:2/subscribe_queue");
        CHECK(sub.id >= 0);
        if (sub.id < 0) return;
        JsonDocument body;
        deserializeJson(body, sub.body);
        CHECK_STR(body["zone_or_output_id"] | "", "z1");
        CHECK_INT(body["max_item_count"] | 0, 10);
        CHECK(!keyOf(sub).empty());
        CHECK(c.queueSubscribed());
        CHECK_INT(seen.calls, 1);
        CHECK_STR(seen.target.c_str(), "z1");
        CHECK_STR(seen.ids.c_str(), "1,2");
        CHECK_INT(c.queue().capacity(), 10);

        // An update on the subscription changes the list and tells the caller.
        core.push("CONTINUE", "Changed", sub.id, R"({"changes":[{"operation":"remove","index":0,"count":1}]})");
        ticks(c, 1);
        CHECK_INT(seen.calls, 2);
        CHECK_STR(seen.ids.c_str(), "2");
        CHECK_STR(c.queue()[0].title, "Two");

        // Asking for what is already followed sends nothing.
        size_t before = core.requests.size();
        CHECK(c.followQueue("z1", 10));
        ticks(c);
        CHECK_INT(core.requests.size(), before);

        // A change that does not fit: the client subscribes again and gets the queue whole.
        core.queueItems = R"([{"queue_item_id":5}])";
        core.push("CONTINUE", "Changed", sub.id, R"({"changes":[{"operation":"remove","index":4,"count":1}]})");
        int callsBefore = seen.calls;
        ticks(c);
        CHECK_INT(seen.calls, callsBefore + 2);   // emptied, then whole again
        Request unsub = core.last("com.roonlabs.transport:2/unsubscribe_queue");
        CHECK(unsub.id >= 0);
        if (unsub.id >= 0) CHECK_STR(keyOf(unsub).c_str(), keyOf(sub).c_str());
        CHECK_INT(core.count("com.roonlabs.transport:2/subscribe_queue"), 2);
        CHECK_STR(seen.ids.c_str(), "5");
        CHECK(c.queueSubscribed());
        sub = core.last("com.roonlabs.transport:2/subscribe_queue");

        // Following another zone ends the first subscription before the next one starts.
        CHECK(c.followQueue("z2", 10));
        ticks(c);
        unsub = core.last("com.roonlabs.transport:2/unsubscribe_queue");
        CHECK_INT(core.count("com.roonlabs.transport:2/unsubscribe_queue"), 2);
        if (unsub.id >= 0) CHECK_STR(keyOf(unsub).c_str(), keyOf(sub).c_str());
        Request sub2 = core.last("com.roonlabs.transport:2/subscribe_queue");
        JsonDocument body2;
        deserializeJson(body2, sub2.body);
        CHECK_STR(body2["zone_or_output_id"] | "", "z2");
        CHECK(keyOf(sub2) != keyOf(sub));
        CHECK_STR(seen.target.c_str(), "z2");

        // A late update on the old subscription is not mistaken for the new one.
        int calls = seen.calls;
        core.push("CONTINUE", "Changed", sub.id, R"({"changes":[{"operation":"remove","index":0,"count":1}]})");
        ticks(c, 1);
        CHECK_INT(seen.calls, calls);
        CHECK_STR(seen.ids.c_str(), "5");

        // After a reconnection the queue is subscribed again by itself.
        size_t mark = core.requests.size();
        core.close();
        clock.now += Client::RETRY_PERIOD_MS + 1;
        ticks(c, 8);
        CHECK_INT(core.count("com.roonlabs.transport:2/unsubscribe_queue", mark), 0);   // that key died with the connection
        CHECK(c.paired());
        CHECK_INT(core.connects, 2);
        CHECK(c.queueSubscribed());
        Request again = core.last("com.roonlabs.transport:2/subscribe_queue");
        CHECK(again.id >= 0);
        JsonDocument body3;
        if (again.id >= 0) deserializeJson(body3, again.body);
        CHECK_STR(body3["zone_or_output_id"] | "", "z2");

        // Following nothing ends the subscription and empties the queue.
        CHECK(c.followQueue("", 10));
        ticks(c);
        unsub = core.last("com.roonlabs.transport:2/unsubscribe_queue");
        CHECK(unsub.id > again.id);
        CHECK(!c.queueSubscribed());
        CHECK_INT(c.queue().size(), 0);
        CHECK_INT(c.queue().capacity(), 0);   // the block is given back
        CHECK(!c.followQueue("z2", 0));
    }

    // A zone the server does not know: asked once, even by a caller that asks every loop.
    {
        FakeCore core;
        NoUdp udp;
        FakeClock clock;
        Client c(core, udp, clock);
        Seen seen;
        c.onQueue(queueChanged, &seen);
        core.queueAnswer = "InvalidRequest";
        c.followQueue("gone", 5);
        c.setServer("core", 9330);
        c.begin(ext);
        for (int i = 0; i < 10; i++) {
            c.tick();
            CHECK(c.followQueue("gone", 5));
        }
        CHECK_INT(core.count("com.roonlabs.transport:2/subscribe_queue"), 1);
        CHECK(!c.queueSubscribed());
        CHECK_INT(seen.calls, 1);
        CHECK_STR(seen.ids.c_str(), "");

        // Another zone is asked for at once.
        core.queueAnswer = "Subscribed";
        CHECK(c.followQueue("z9", 5));
        ticks(c);
        CHECK_INT(core.count("com.roonlabs.transport:2/subscribe_queue"), 2);
        CHECK(c.queueSubscribed());
        CHECK_STR(seen.target.c_str(), "z9");

        // The server ends a running subscription (its zone went away): the queue empties.
        Request sub = core.last("com.roonlabs.transport:2/subscribe_queue");
        core.push("COMPLETE", "Unsubscribed", sub.id, "");
        ticks(c, 1);
        CHECK(!c.queueSubscribed());
        CHECK_INT(c.queue().size(), 0);
        CHECK_STR(seen.ids.c_str(), "");
        CHECK(!c.followQueue("0123456789012345678901234567890123456789012345678901234567890123456789012", 5));
        CHECK_STR(c.queueTarget(), "z9");
    }

    // A Subscribed that cannot be read ends there, like a refusal, rather than asking forever.
    {
        FakeCore core;
        NoUdp udp;
        FakeClock clock;
        Client c(core, udp, clock);
        Seen seen;
        c.onQueue(queueChanged, &seen);
        core.queueItems = "[not json";
        c.followQueue("z1", 5);
        c.setServer("core", 9330);
        c.begin(ext);
        for (int i = 0; i < 10; i++) {
            c.tick();
            c.followQueue("z1", 5);
        }
        CHECK_INT(core.count("com.roonlabs.transport:2/subscribe_queue"), 1);
        CHECK_INT(core.count("com.roonlabs.transport:2/unsubscribe_queue"), 1);
        CHECK(!c.queueSubscribed());
        CHECK_INT(seen.calls, 1);
        CHECK_STR(seen.target.c_str(), "z1");
    }

    // Play from here, group and ungroup: the bodies the server expects.
    {
        FakeCore core;
        NoUdp udp;
        FakeClock clock;
        Client c(core, udp, clock);
        c.setServer("core", 9330);
        c.begin(ext);
        ticks(c);

        Client::Reply r = c.playFromHere("z1", 4000000000u);
        CHECK(r.ok);
        Request p = core.last("com.roonlabs.transport:2/play_from_here");
        CHECK(p.id >= 0);
        if (p.id >= 0) CHECK_STR(p.body.c_str(), R"({"zone_or_output_id":"z1","queue_item_id":4000000000})");

        const char *ids[] = {"o1", "o2", "o3"};
        r = c.groupOutputs(ids, 3);
        CHECK(r.ok);
        p = core.last("com.roonlabs.transport:2/group_outputs");
        CHECK(p.id >= 0);
        if (p.id >= 0) CHECK_STR(p.body.c_str(), R"({"output_ids":["o1","o2","o3"]})");

        r = c.ungroupOutputs(ids + 1, 2);
        CHECK(r.ok);
        p = core.last("com.roonlabs.transport:2/ungroup_outputs");
        CHECK(p.id >= 0);
        if (p.id >= 0) CHECK_STR(p.body.c_str(), R"({"output_ids":["o2","o3"]})");

        size_t before = core.requests.size();
        r = c.groupOutputs(ids, 0);
        CHECK(!r.ok);
        CHECK_STR(r.error, "no outputs given");
        CHECK_INT(core.requests.size(), before);
    }
}
