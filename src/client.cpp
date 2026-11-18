#include "tftcore/client.h"

#include <cstdio>
#include <cstring>

namespace tftcore {
namespace {

constexpr const char *SVC_REGISTRY = "com.roonlabs.registry:1";
constexpr const char *SVC_TRANSPORT = "com.roonlabs.transport:2";
constexpr const char *SVC_BROWSE = "com.roonlabs.browse:1";
constexpr const char *SVC_PING = "com.roonlabs.ping:1";

constexpr uint32_t INFO_TIMEOUT_MS = 5000;
// A Core with a valid token answers at once; without one the reply only comes when a person
// enables the extension, so we wait a moment and then say so on screen.
constexpr uint32_t REGISTER_WAIT_MS = 2500;

// JSON documents allocate where the caller's allocator says (PSRAM on the panel).
struct JsonAlloc : ArduinoJson::Allocator {
    explicit JsonAlloc(Alloc &a) : a_(a) {}
    void *allocate(size_t n) override { return a_.alloc(n); }
    void deallocate(void *p) override { a_.release(p); }
    void *reallocate(void *p, size_t n) override { return a_.realloc(p, n); }
    Alloc &a_;
};

void copyStr(char *dst, size_t dstLen, const char *src) {
    if (!src) src = "";
    size_t n = strlen(src);
    if (n > dstLen - 1) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

}  // namespace

const char *stateName(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::Discovering: return "discovering";
        case State::Connecting: return "connecting";
        case State::Registering: return "registering";
        case State::NeedsApproval: return "needs approval";
        case State::Paired: return "paired";
        case State::Failed: return "failed";
    }
    return "?";
}

Client::Client(ByteStream &stream, UdpSocket &udp, Clock &clock, Alloc &alloc)
    : udp_(udp), clock_(clock), alloc_(alloc), ws_(stream, clock, alloc),
      queue_(alloc), gotBody_(alloc), reqBuf_(alloc) {}

void Client::begin(const ExtensionInfo &info) {
    info_ = info;
    state_ = State::Discovering;
    lastDiscoveryAt_ = 0;
    lastAttemptAt_ = 0;
}

void Client::setServer(const char *host, uint16_t port) {
    dropConnection();
    if (host && *host) {
        copyStr(host_, sizeof host_, host);
        port_ = port ? port : 9330;
        fixedServer_ = true;
    } else {
        host_[0] = 0;
        port_ = 0;
        fixedServer_ = false;
    }
    lastDiscoveryAt_ = 0;
    lastAttemptAt_ = 0;
}

void Client::setToken(const char *serverId, const char *token) {
    copyStr(tokenServerId_, sizeof tokenServerId_, serverId);
    copyStr(token_, sizeof token_, token);
}

void Client::forget() {
    token_[0] = 0;
    tokenServerId_[0] = 0;
    dropConnection();
    if (!fixedServer_) host_[0] = 0;
    setState(State::Discovering, "pairing forgotten");
}

void Client::setState(State s, const char *detail) {
    if (detail && *detail) {
        copyStr(errBuf_, sizeof errBuf_, detail);
        err_ = errBuf_;
    }
    if (state_ == s) return;
    state_ = s;
    if (stateCb_) stateCb_(stateCtx_, s, detail ? detail : "");
}

void Client::dropConnection() {
    ws_.close();
    subscribed_ = false;
    zonesRequested_ = false;
    waiting_ = false;
    got_ = false;
    registerId_ = zonesId_ = pendingId_ = -1;
    zones_.clear();
    queueRequested_ = false;
    queueSubscribed_ = false;
    queueId_ = -1;
    queueHave_[0] = 0;
    queue_.clear();
}

void Client::disconnect() {
    dropConnection();
    state_ = State::Idle;
}

bool Client::discover() {
    if (!udp_.open()) {
        setState(State::Failed, "cannot open the discovery socket");
        return false;
    }
    Buffer query(alloc_);
    char tid[20];
    snprintf(tid, sizeof tid, "%08x%04x", (unsigned)clock_.nowMs(), (unsigned)(requestId_ & 0xffff));
    if (!soodBuildQuery(query, tid)) {
        udp_.close();
        setState(State::Failed, "cannot build the discovery query");
        return false;
    }
    // Multicast first, then broadcast: plenty of access points drop one or the other.
    udp_.sendTo(SOOD_MULTICAST, SOOD_PORT, query.data(), query.size());
    udp_.sendBroadcast(SOOD_PORT, query.data(), query.size());

    char localIp[46] = {0};
    if (!udp_.localAddress(localIp, sizeof localIp)) localIp[0] = 0;

    // Collect every answer in the window, then keep the best address.
    uint32_t deadline = clock_.nowMs() + DISCOVERY_WINDOW_MS;
    uint8_t buf[1024];
    char fromIp[46];
    int bestScore = -1;
    char bestIp[46] = {0};
    uint16_t bestPort = 0;
    char bestId[72] = {0}, bestName[72] = {0};
    while ((int32_t)(clock_.nowMs() - deadline) < 0) {
        int got = udp_.recvFrom(buf, sizeof buf, fromIp, sizeof fromIp);
        if (got < 0) break;
        if (got == 0) continue;
        SoodReply reply;
        if (!soodParseReply(buf, (size_t)got, reply)) continue;
        int score = soodAddressScore(fromIp, localIp);
        if (score <= bestScore) continue;
        bestScore = score;
        copyStr(bestIp, sizeof bestIp, fromIp);
        bestPort = reply.httpPort;
        copyStr(bestId, sizeof bestId, reply.uniqueId);
        copyStr(bestName, sizeof bestName, reply.name);
    }
    udp_.close();

    if (bestScore < 0) {
        setState(State::Discovering, "no server answered on this network yet");
        return false;
    }
    copyStr(host_, sizeof host_, bestIp);
    port_ = bestPort;
    copyStr(serverId_, sizeof serverId_, bestId);
    copyStr(serverName_, sizeof serverName_, bestName);
    return true;
}

bool Client::sendRequest(const char *method, const char *jsonBody, int &idOut) {
    idOut = nextRequestId();
    if (!mooBuildRequest(reqBuf_, method, idOut, jsonBody)) return false;
    return ws_.send(reqBuf_.data(), reqBuf_.size(), true);
}

void Client::wsMessage(void *ctx, const uint8_t *msg, size_t len, bool) {
    static_cast<Client *>(ctx)->handleMessage(msg, len);
}

void Client::handleMessage(const uint8_t *msg, size_t len) {
    stats_.messages++;
    stats_.lastLen = len;
    stats_.totalBytes += len;
    if (len > stats_.maxLen) stats_.maxLen = len;

    MooHeader h;
    if (!mooParse(msg, len, h)) return;   // nothing sane to do with a malformed frame
    if (h.verb == MooVerb::Request) {
        handleServerRequest(h);
        return;
    }
    handleReply(h, msg + h.bodyOffset, h.contentLength);
}

void Client::handleServerRequest(const MooHeader &h) {
    bool ping = strcmp(h.service, SVC_PING) == 0 && strcmp(h.name, "ping") == 0;
    if (!mooBuildComplete(reqBuf_, ping ? "Success" : "InvalidRequest", h.requestId, nullptr)) return;
    ws_.send(reqBuf_.data(), reqBuf_.size(), true);
}

void Client::handleReply(const MooHeader &h, const uint8_t *body, size_t len) {
    JsonAlloc ja(alloc_);

    if (h.requestId == registerId_ && registerId_ >= 0) {
        if (h.is("Registered")) {
            JsonDocument doc(&ja);
            if (h.json && len && !deserializeJson(doc, body, len)) {
                copyStr(serverId_, sizeof serverId_, doc["core_id"] | serverId_);
                copyStr(serverName_, sizeof serverName_, doc["display_name"] | serverName_);
                const char *token = doc["token"] | "";
                if (*token) {
                    copyStr(token_, sizeof token_, token);
                    copyStr(tokenServerId_, sizeof tokenServerId_, serverId_);
                    if (tokenCb_) tokenCb_(tokenCtx_, serverId_, token_);
                }
            }
            registerId_ = -1;
            setState(State::Paired, serverName_);
        } else if (h.verb == MooVerb::Complete) {
            // The Core refused the registration: a stale token is the usual reason, so drop it
            // and let the next attempt ask the user again.
            registerId_ = -1;
            bool hadToken = token_[0] != 0;
            token_[0] = 0;
            tokenServerId_[0] = 0;
            dropConnection();
            setState(State::Failed, hadToken ? "the saved pairing was refused, asking again" : h.name);
        }
        return;
    }

    if (h.requestId == zonesId_ && zonesId_ >= 0) {
        JsonDocument doc(&ja);
        if (!h.json || !len) return;
        if (deserializeJson(doc, body, len)) {
            setState(state_, "a zone message could not be read");
            return;
        }
        ZoneDelta delta;
        if (h.is("Subscribed")) {
            zones_.applySubscribed(doc.as<JsonVariantConst>(), delta);
            subscribed_ = true;
            stats_.subscribedLen = len;
        } else if (h.is("Changed")) {
            zones_.applyChanged(doc.as<JsonVariantConst>(), delta);
        } else {
            return;   // Unsubscribed or an error: the next tick() re-subscribes
        }
        if (zonesCb_) zonesCb_(zonesCtx_, zones_, delta);
        return;
    }

    if (h.requestId == queueId_ && queueId_ >= 0) {
        char target[sizeof queueHave_];
        copyStr(target, sizeof target, queueHave_);
        bool update = h.is("Subscribed") || h.is("Changed");
        JsonDocument doc(&ja);
        bool read = update && h.json && len && !deserializeJson(doc, body, len);
        if (!update) {
            // Unsubscribed, or refused (an unknown zone): over until another zone is followed or
            // the connection is made again.
            queueId_ = -1;
            queueSubscribed_ = false;
            queue_.clear();
        } else if (read && h.is("Subscribed")) {
            queue_.applySubscribed(doc.as<JsonVariantConst>());
            queueSubscribed_ = true;
        } else if (!read || !queue_.applyChanged(doc.as<JsonVariantConst>())) {
            unsubscribeQueue();
            // A change missed or not applicable: our list is no longer the server's, so tick()
            // subscribes again. A Subscribed we cannot read would fail the same way next time,
            // so it ends there, as a refusal does.
            if (h.is("Changed")) queueRequested_ = false;
        }
        if (queueCb_) queueCb_(queueCtx_, target, queue_);
        return;
    }

    if (waiting_ && h.requestId == pendingId_) {
        gotHeader_ = h;
        gotBody_.clear();
        if (len) gotBody_.append(body, len);
        got_ = true;
    }
}

void Client::pump(uint32_t untilMs) {
    while (true) {
        if (!ws_.poll(wsMessage, this)) {
            const char *why = ws_.lastError();
            dropConnection();
            setState(State::Failed, why && *why ? why : "connection lost");
            return;
        }
        if (got_) return;
        if ((int32_t)(clock_.nowMs() - untilMs) >= 0) return;
    }
}

bool Client::connectAndRegister() {
    setState(State::Connecting, host_);
    if (!ws_.open(host_, port_, "/api")) {
        setState(State::Failed, ws_.lastError());
        return false;
    }
    setState(State::Registering, serverName_);

    // 1. registry/info: the Core identifies itself, so we know which token applies.
    char method[80];
    snprintf(method, sizeof method, "%s/info", SVC_REGISTRY);
    int id;
    if (!sendRequest(method, nullptr, id)) {
        dropConnection();
        setState(State::Failed, "cannot send the registry request");
        return false;
    }
    waiting_ = true;
    got_ = false;
    pendingId_ = id;
    pump(clock_.nowMs() + INFO_TIMEOUT_MS);
    waiting_ = false;
    if (!got_) {
        dropConnection();
        setState(State::Failed, "the server did not answer the registry request");
        return false;
    }
    {
        JsonAlloc ja(alloc_);
        JsonDocument doc(&ja);
        if (gotHeader_.json && gotBody_.size() && !deserializeJson(doc, gotBody_.data(), gotBody_.size())) {
            copyStr(serverId_, sizeof serverId_, doc["core_id"] | serverId_);
            copyStr(serverName_, sizeof serverName_, doc["display_name"] | serverName_);
        }
    }

    // 2. registry/register, with the token when it belongs to this Core.
    JsonAlloc ja(alloc_);
    JsonDocument body(&ja);
    body["extension_id"] = info_.extensionId ? info_.extensionId : "";
    body["display_name"] = info_.displayName ? info_.displayName : "";
    body["display_version"] = info_.displayVersion ? info_.displayVersion : "";
    body["publisher"] = info_.publisher ? info_.publisher : "";
    body["email"] = info_.email ? info_.email : "";
    if (info_.website && *info_.website) body["website"] = info_.website;
    JsonArray required = body["required_services"].to<JsonArray>();
    required.add(SVC_TRANSPORT);
    required.add(SVC_BROWSE);
    body["optional_services"].to<JsonArray>();
    body["provided_services"].to<JsonArray>();
    if (token_[0] && strcmp(tokenServerId_, serverId_) == 0) body["token"] = token_;

    Buffer text(alloc_);
    size_t needed = measureJson(body);
    if (!text.setSize(needed + 1)) {
        dropConnection();
        setState(State::Failed, "out of memory");
        return false;
    }
    serializeJson(body, text.chars(), text.size());
    text.data()[needed] = 0;

    snprintf(method, sizeof method, "%s/register", SVC_REGISTRY);
    if (!sendRequest(method, text.chars(), registerId_)) {
        dropConnection();
        setState(State::Failed, "cannot send the registration");
        return false;
    }
    // The reply arrives at once with a valid token; otherwise only once a person enables the
    // extension. Either way the request stays open and handleReply() finishes the job.
    pump(clock_.nowMs() + REGISTER_WAIT_MS);
    if (state_ == State::Registering) {
        char detail[128];
        snprintf(detail, sizeof detail, "enable \"%s\" in the server's extension settings",
                 info_.displayName ? info_.displayName : "this client");
        setState(State::NeedsApproval, detail);
    }
    return state_ == State::Paired || state_ == State::NeedsApproval;
}

void Client::subscribeZones() {
    char method[80];
    snprintf(method, sizeof method, "%s/subscribe_zones", SVC_TRANSPORT);
    char body[64];
    snprintf(body, sizeof body, "{\"subscription_key\":\"%d\"}", nextRequestId());
    if (!sendRequest(method, body, zonesId_)) {
        dropConnection();
        setState(State::Failed, "cannot subscribe to the zones");
        return;
    }
    zonesRequested_ = true;
}

bool Client::followQueue(const char *zoneOrOutputId, uint16_t maxItems) {
    if (!zoneOrOutputId) zoneOrOutputId = "";
    if (strlen(zoneOrOutputId) >= sizeof queueWant_ || (*zoneOrOutputId && !maxItems)) return false;
    bool same = strcmp(zoneOrOutputId, queueWant_) == 0 && maxItems == queueWantMax_;
    if (same) return true;   // safe to call every loop: a refused zone is not asked again
    copyStr(queueWant_, sizeof queueWant_, zoneOrOutputId);
    queueWantMax_ = maxItems;
    queueRequested_ = false;   // tick() does the rest, outside any callback
    return true;
}

void Client::unsubscribeQueue() {
    if (queueId_ >= 0 && ws_.isOpen()) {
        char method[80], body[48];
        snprintf(method, sizeof method, "%s/unsubscribe_queue", SVC_TRANSPORT);
        snprintf(body, sizeof body, "{\"subscription_key\":\"%s\"}", queueKey_);
        int id;
        sendRequest(method, body, id);   // its answer is not waited for
    }
    queueId_ = -1;
    queueSubscribed_ = false;
    queueHave_[0] = 0;
    queue_.clear();
}

void Client::syncQueue() {
    queueRequested_ = true;
    unsubscribeQueue();
    if (!queueWant_[0]) {
        queue_.reserve(0);   // nothing followed: give the block back
        return;
    }
    if (!queue_.reserve(queueWantMax_)) {
        setState(state_, "not enough memory for the queue");
        return;
    }
    char method[80], body[192];
    snprintf(method, sizeof method, "%s/subscribe_queue", SVC_TRANSPORT);
    snprintf(queueKey_, sizeof queueKey_, "%d", nextRequestId());
    snprintf(body, sizeof body, "{\"zone_or_output_id\":\"%s\",\"max_item_count\":%u,\"subscription_key\":\"%s\"}",
             queueWant_, (unsigned)queueWantMax_, queueKey_);
    if (!sendRequest(method, body, queueId_)) {
        queueId_ = -1;
        dropConnection();
        setState(State::Failed, "cannot subscribe to the queue");
        return;
    }
    copyStr(queueHave_, sizeof queueHave_, queueWant_);
}

void Client::tick() {
    if (state_ == State::Idle) return;

    if (ws_.isOpen()) {
        if (!ws_.poll(wsMessage, this)) {
            const char *why = ws_.lastError();
            dropConnection();
            setState(State::Failed, why && *why ? why : "connection lost");
            return;
        }
        if (state_ == State::Paired && !zonesRequested_) subscribeZones();
        if (state_ == State::Paired && zonesRequested_ && !queueRequested_) syncQueue();
        return;
    }

    uint32_t now = clock_.nowMs();
    if (!host_[0]) {
        if (lastDiscoveryAt_ && (int32_t)(now - lastDiscoveryAt_) < (int32_t)DISCOVERY_PERIOD_MS) return;
        lastDiscoveryAt_ = now;
        if (!discover()) return;
    }
    if (lastAttemptAt_ && (int32_t)(now - lastAttemptAt_) < (int32_t)RETRY_PERIOD_MS) return;
    lastAttemptAt_ = now;
    if (!connectAndRegister() && !fixedServer_) host_[0] = 0;   // a discovered address can go stale
}

Client::Reply Client::call(const char *method, const char *jsonBody, JsonDocument *resp, uint32_t timeoutMs,
                           JsonDocument *filter) {
    Reply rep;
    if (!ws_.isOpen()) {
        rep.error = "not connected";
        return rep;
    }
    int id;
    if (!sendRequest(method, jsonBody, id)) {
        rep.error = "could not send the request";
        return rep;
    }
    waiting_ = true;
    got_ = false;
    pendingId_ = id;
    gotBody_.clear();
    pump(clock_.nowMs() + timeoutMs);
    waiting_ = false;
    if (!got_) {
        rep.error = ws_.isOpen() ? "no answer from the server" : "connection lost";
        return rep;
    }
    copyStr(rep.name, sizeof rep.name, gotHeader_.name);
    rep.ok = gotHeader_.verb == MooVerb::Complete && gotHeader_.is("Success");
    if (resp && gotHeader_.json && gotBody_.size()) {
        DeserializationError e =
            filter ? deserializeJson(*resp, gotBody_.data(), gotBody_.size(), DeserializationOption::Filter(*filter))
                   : deserializeJson(*resp, gotBody_.data(), gotBody_.size());
        if (e) {
            rep.ok = false;
            rep.error = "the answer could not be read";
        }
    }
    return rep;
}

Client::Reply Client::control(const char *zoneOrOutputId, const char *control) {
    char method[80], body[256];
    snprintf(method, sizeof method, "%s/control", SVC_TRANSPORT);
    snprintf(body, sizeof body, "{\"zone_or_output_id\":\"%s\",\"control\":\"%s\"}", zoneOrOutputId, control);
    return call(method, body, nullptr, 5000);
}

Client::Reply Client::changeVolume(const char *outputId, const char *how, float value) {
    char method[80], body[256];
    snprintf(method, sizeof method, "%s/change_volume", SVC_TRANSPORT);
    // Formatted by hand: a float conversion in snprintf drags in a large part of newlib, and a
    // volume never needs more than two decimals.
    long whole = (long)value;
    long hundredths = (long)((value - (float)whole) * 100.0f);
    if (hundredths < 0) hundredths = -hundredths;
    if (hundredths)
        snprintf(body, sizeof body, "{\"output_id\":\"%s\",\"how\":\"%s\",\"value\":%ld.%02ld}", outputId, how,
                 whole, hundredths);
    else
        snprintf(body, sizeof body, "{\"output_id\":\"%s\",\"how\":\"%s\",\"value\":%ld}", outputId, how, whole);
    return call(method, body, nullptr, 5000);
}

Client::Reply Client::mute(const char *outputId, bool on) {
    char method[80], body[256];
    snprintf(method, sizeof method, "%s/mute", SVC_TRANSPORT);
    snprintf(body, sizeof body, "{\"output_id\":\"%s\",\"how\":\"%s\"}", outputId, on ? "mute" : "unmute");
    return call(method, body, nullptr, 5000);
}

Client::Reply Client::seek(const char *zoneOrOutputId, const char *how, int32_t seconds) {
    char method[80], body[256];
    snprintf(method, sizeof method, "%s/seek", SVC_TRANSPORT);
    snprintf(body, sizeof body, "{\"zone_or_output_id\":\"%s\",\"how\":\"%s\",\"seconds\":%d}", zoneOrOutputId, how,
             (int)seconds);
    return call(method, body, nullptr, 5000);
}

Client::Reply Client::playFromHere(const char *zoneOrOutputId, uint32_t queueItemId) {
    char method[80], body[192];
    snprintf(method, sizeof method, "%s/play_from_here", SVC_TRANSPORT);
    snprintf(body, sizeof body, "{\"zone_or_output_id\":\"%s\",\"queue_item_id\":%lu}", zoneOrOutputId,
             (unsigned long)queueItemId);
    return call(method, body, nullptr, 5000);
}

Client::Reply Client::groupOutputs(const char *const *outputIds, size_t count) {
    return outputsCall("group_outputs", outputIds, count);
}

Client::Reply Client::ungroupOutputs(const char *const *outputIds, size_t count) {
    return outputsCall("ungroup_outputs", outputIds, count);
}

Client::Reply Client::outputsCall(const char *what, const char *const *outputIds, size_t count) {
    Reply rep;
    if (!outputIds || !count) {
        rep.error = "no outputs given";
        return rep;
    }
    JsonAlloc ja(alloc_);
    JsonDocument body(&ja);
    JsonArray ids = body["output_ids"].to<JsonArray>();
    for (size_t i = 0; i < count; i++) ids.add(outputIds[i] ? outputIds[i] : "");
    Buffer text(alloc_);
    size_t needed = measureJson(body);
    if (!text.setSize(needed + 1)) {
        rep.error = "out of memory";
        return rep;
    }
    serializeJson(body, text.chars(), text.size());
    text.data()[needed] = 0;
    char method[80];
    snprintf(method, sizeof method, "%s/%s", SVC_TRANSPORT, what);
    return call(method, text.chars(), nullptr, 5000);
}

Client::Reply Client::browse(const char *jsonBody, JsonDocument &resp, uint32_t timeoutMs) {
    char method[80];
    snprintf(method, sizeof method, "%s/browse", SVC_BROWSE);
    return call(method, jsonBody, &resp, timeoutMs);
}

Client::Reply Client::browseLoad(const char *jsonBody, JsonDocument &resp, uint32_t timeoutMs) {
    char method[80];
    snprintf(method, sizeof method, "%s/load", SVC_BROWSE);
    return call(method, jsonBody, &resp, timeoutMs);
}

bool Client::imageUrl(const char *imageKey, int width, int height, char *out, size_t outLen) const {
    if (!imageKey || !*imageKey || !host_[0] || !port_) return false;
    int n = snprintf(out, outLen, "http://%s:%u/api/image/%s?scale=fit&width=%d&height=%d&format=image/jpeg", host_,
                     (unsigned)port_, imageKey, width, height);
    return n > 0 && (size_t)n < outLen;
}

}  // namespace tftcore
