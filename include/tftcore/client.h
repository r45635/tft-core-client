// The whole client: discovery, WebSocket, pairing, the zone subscription, and synchronous
// calls for everything a user action needs (controls, volume, browse).
#pragma once

#include <ArduinoJson.h>

#include <cstdint>

#include "buffer.h"
#include "io.h"
#include "moo.h"
#include "sood.h"
#include "ws.h"
#include "zones.h"

namespace tftcore {

// What a client tells the server about itself. Every pointer must outlive the Client.
struct ExtensionInfo {
    const char *extensionId = nullptr;    // "com.example.thing", stable per product
    const char *displayName = nullptr;    // what the user sees in the extensions list
    const char *displayVersion = nullptr;
    const char *publisher = nullptr;
    const char *email = nullptr;
    const char *website = nullptr;        // optional
};

enum class State : uint8_t {
    Idle,            // nothing started
    Discovering,     // looking for a server (or waiting to retry a fixed address)
    Connecting,      // TCP + WebSocket handshake
    Registering,     // registry info / register sent, token accepted
    NeedsApproval,   // register is pending: a person must enable the extension
    Paired,          // registered, subscribing or subscribed
    Failed,          // the last attempt failed; tick() retries after a backoff
};

const char *stateName(State s);

class Client {
  public:
    // `stream` and `udp` are owned by the caller and reused across reconnections.
    Client(ByteStream &stream, UdpSocket &udp, Clock &clock, Alloc &alloc = defaultAlloc());

    void begin(const ExtensionInfo &info);
    // Fixed server address, skipping discovery (a network that drops both multicast and
    // broadcast, or a server on another subnet). Empty host = discover again.
    void setServer(const char *host, uint16_t port);
    // The token kept from a previous pairing, and the id of the server it belongs to.
    void setToken(const char *serverId, const char *token);
    void forget();                       // drops the token and disconnects

    // Drives the whole state machine. Call it often (every loop); it never blocks for long
    // except during the WebSocket handshake.
    void tick();
    void disconnect();

    State state() const { return state_; }
    bool paired() const { return state_ == State::Paired; }
    bool subscribed() const { return subscribed_; }
    const char *serverName() const { return serverName_; }
    const char *serverId() const { return serverId_; }
    const char *serverHost() const { return host_; }
    uint16_t serverHttpPort() const { return port_; }
    const char *lastError() const { return err_; }
    const ZoneList &zones() const { return zones_; }

    // --- Events. Set them before begin(); they are called from tick(). ---
    using ZonesCb = void (*)(void *ctx, const ZoneList &zones, const ZoneDelta &delta);
    using TokenCb = void (*)(void *ctx, const char *serverId, const char *token);
    using StateCb = void (*)(void *ctx, State state, const char *detail);
    void onZones(ZonesCb cb, void *ctx) { zonesCb_ = cb; zonesCtx_ = ctx; }
    // A new or refreshed token: persist it, or the user authorizes again after every restart.
    void onToken(TokenCb cb, void *ctx) { tokenCb_ = cb; tokenCtx_ = ctx; }
    void onState(StateCb cb, void *ctx) { stateCb_ = cb; stateCtx_ = ctx; }

    // --- Requests. They pump the socket until the reply arrives, so a zone push can be
    // delivered while one is in flight; they never run before paired(). ---
    struct Reply {
        bool ok = false;              // a COMPLETE whose name is "Success" (or `expect`)
        char name[72] = {0};          // the reply's status name, or "" on a transport failure
        const char *error = "";       // transport-level reason when name is empty
    };
    // `body` is JSON text. `resp` receives the reply body, parsed with `filter` when given.
    Reply call(const char *method, const char *jsonBody, JsonDocument *resp = nullptr,
               uint32_t timeoutMs = 10000, JsonDocument *filter = nullptr);

    Reply control(const char *zoneOrOutputId, const char *control);
    Reply changeVolume(const char *outputId, const char *how, float value);
    Reply mute(const char *outputId, bool on);
    Reply seek(const char *zoneOrOutputId, const char *how, int32_t seconds);
    // Browse and load pass the caller's body through unchanged (hierarchy, multi_session_key
    // and the rest are the caller's business: see docs/PROTOCOL.md section 6).
    Reply browse(const char *jsonBody, JsonDocument &resp, uint32_t timeoutMs = 10000);
    Reply browseLoad(const char *jsonBody, JsonDocument &resp, uint32_t timeoutMs = 10000);

    // The cover art URL on the server's own HTTP port (no MOO involved).
    bool imageUrl(const char *imageKey, int width, int height, char *out, size_t outLen) const;

    // Sizes seen on the wire, for sizing buffers on a small board (see README).
    struct Stats {
        uint32_t messages = 0;
        size_t lastLen = 0;
        size_t maxLen = 0;
        size_t subscribedLen = 0;   // the first full zone list: the biggest message by far
        uint64_t totalBytes = 0;
    };
    const Stats &stats() const { return stats_; }

    static constexpr uint32_t DISCOVERY_PERIOD_MS = 10000;
    // How long one query listens for answers before choosing between them.
    static constexpr uint32_t DISCOVERY_WINDOW_MS = 1500;
    static constexpr uint32_t RETRY_PERIOD_MS = 5000;

  private:
    void setState(State s, const char *detail);
    bool discover();
    bool connectAndRegister();
    void subscribeZones();
    void dropConnection();
    void handleMessage(const uint8_t *msg, size_t len);
    void handleReply(const MooHeader &h, const uint8_t *body, size_t len);
    void handleServerRequest(const MooHeader &h);
    void pump(uint32_t untilMs);
    int nextRequestId() { return requestId_++; }
    bool sendRequest(const char *method, const char *jsonBody, int &idOut);

    static void wsMessage(void *ctx, const uint8_t *msg, size_t len, bool binary);

    UdpSocket &udp_;
    Clock &clock_;
    Alloc &alloc_;
    WsClient ws_;
    ZoneList zones_;
    ExtensionInfo info_{};
    Stats stats_{};

    State state_ = State::Idle;
    bool subscribed_ = false;
    bool zonesRequested_ = false;
    bool fixedServer_ = false;
    char host_[64] = {0};
    uint16_t port_ = 0;
    char serverId_[72] = {0};
    char serverName_[72] = {0};
    char token_[256] = {0};
    char tokenServerId_[72] = {0};
    const char *err_ = "";
    char errBuf_[96] = {0};

    uint32_t lastDiscoveryAt_ = 0, lastAttemptAt_ = 0;
    int requestId_ = 0;
    int registerId_ = -1, zonesId_ = -1, pendingId_ = -1;

    // The reply the running call() waits for.
    bool waiting_ = false;
    bool got_ = false;
    MooHeader gotHeader_{};
    Buffer gotBody_;
    Buffer reqBuf_;

    ZonesCb zonesCb_ = nullptr;   void *zonesCtx_ = nullptr;
    TokenCb tokenCb_ = nullptr;   void *tokenCtx_ = nullptr;
    StateCb stateCb_ = nullptr;   void *stateCtx_ = nullptr;
};

}  // namespace tftcore
