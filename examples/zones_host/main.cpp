// Pairs with a music server on the local network, follows one zone and its queue, and prints
// what arrives.
// Read-only unless you press a key: it is meant to be run while nobody is listening.
//
//   make zones-host && ./build/zones-host [--host 192.168.1.10] [--port 9330] [--seconds 60]
//
// The pairing token is kept in ./zones-host.token, so the extension only has to be enabled once.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <string>

#include "posix_io.h"
#include "tftcore/client.h"

using namespace tftcore;

namespace {

const char *TOKEN_FILE = "zones-host.token";

struct Follow {
    std::string zoneId;        // the zone we print
    std::string trackKey;
    uint32_t lastSeekAt = 0;   // to measure how regularly the server pushes the position
    int32_t lastSeek = -1;
    int seekTicks = 0;
    int minGap = 1 << 30, maxGap = 0;
    long gapSum = 0;
    posix::SteadyClock clock;
};

void saveToken(void *, const char *serverId, const char *token) {
    FILE *f = fopen(TOKEN_FILE, "w");
    if (!f) {
        printf("! could not write %s\n", TOKEN_FILE);
        return;
    }
    fprintf(f, "%s\n%s\n", serverId, token);
    fclose(f);
    printf("+ pairing saved to %s\n", TOKEN_FILE);
}

bool loadToken(std::string &serverId, std::string &token) {
    FILE *f = fopen(TOKEN_FILE, "r");
    if (!f) return false;
    char id[128] = {0}, tok[512] = {0};
    bool ok = fgets(id, sizeof id, f) && fgets(tok, sizeof tok, f);
    fclose(f);
    if (!ok) return false;
    for (char *p : {id, tok})
        for (char *c = p; *c; c++)
            if (*c == '\n' || *c == '\r') *c = 0;
    serverId = id;
    token = tok;
    return *id && *tok;
}

void onState(void *, State s, const char *detail) {
    printf("= %s%s%s\n", stateName(s), detail && *detail ? ": " : "", detail ? detail : "");
}

void onZones(void *ctx, const ZoneList &zones, const ZoneDelta &delta) {
    auto *f = static_cast<Follow *>(ctx);

    if (delta.full) {
        printf("= %zu zone(s):\n", zones.zones().size());
        for (const Zone &z : zones.zones()) {
            printf("    %-22s %-8s %s\n", z.name.c_str(), z.state.c_str(),
                   z.nowPlaying.present ? z.nowPlaying.title.c_str() : "(nothing loaded)");
            for (const Output &o : z.outputs) {
                const std::string &ids = o.canGroupWith;
                size_t n = ids.empty() ? 0 : (size_t)std::count(ids.begin(), ids.end(), ' ') + 1;
                printf("      output %-15s can be grouped with %zu output(s)\n", o.name.c_str(), n);
            }
        }
    }

    const Zone *z = zones.pick(f->zoneId.c_str());
    if (!z) return;
    if (f->zoneId != z->id) {
        f->zoneId = z->id;
        printf("= following \"%s\"\n", z->name.c_str());
    }

    std::string key = z->nowPlaying.title + "\x1f" + z->nowPlaying.artist;
    if (z->nowPlaying.present && key != f->trackKey) {
        f->trackKey = key;
        printf("* %s - %s [%s] (%u s)\n", z->nowPlaying.artist.c_str(), z->nowPlaying.title.c_str(),
               z->nowPlaying.album.c_str(), (unsigned)z->nowPlaying.lengthSec);
    }

    if (delta.seekOnly && z->nowPlaying.seekSec != f->lastSeek) {
        uint32_t now = f->clock.nowMs();
        if (f->lastSeekAt) {
            int gap = (int)(now - f->lastSeekAt);
            f->seekTicks++;
            f->gapSum += gap;
            if (gap < f->minGap) f->minGap = gap;
            if (gap > f->maxGap) f->maxGap = gap;
        }
        f->lastSeekAt = now;
        f->lastSeek = z->nowPlaying.seekSec;
        printf("  %3d s / %u s  %s\n", (int)z->nowPlaying.seekSec, (unsigned)z->nowPlaying.lengthSec,
               z->state.c_str());
    }
}

void onQueue(void *, const char *zoneOrOutputId, const QueueList &queue) {
    printf("= queue of %s: %zu item(s)\n", zoneOrOutputId, queue.size());
    for (const QueueItem &it : queue)
        printf("    #%-10u %s - %s (%u s)\n", (unsigned)it.id, it.artist, it.title, (unsigned)it.lengthSec);
}

bool keyPressed(char &c) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval tv{0, 0};
    if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) <= 0) return false;
    return ::read(STDIN_FILENO, &c, 1) == 1;
}

}  // namespace

int main(int argc, char **argv) {
    // Line buffered even when the output is a file: this is a tool you watch while it runs, and
    // a redirected run must not look dead for minutes.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const char *host = nullptr;
    uint16_t port = 9330;
    int seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else {
            printf("usage: %s [--host H] [--port P] [--seconds N]\n", argv[0]);
            return 2;
        }
    }

    posix::TcpStream stream;
    posix::Udp udp;
    posix::SteadyClock clock;
    Client client(stream, udp, clock);
    Follow follow;

    ExtensionInfo info;
    info.extensionId = "com.textfromtrack.zones-host";
    info.displayName = "Zones (host example)";
    info.displayVersion = "0.1.0";
    info.publisher = "Vincent Cruvellier";
    info.email = "none@example.com";

    std::string serverId, token;
    if (loadToken(serverId, token)) {
        client.setToken(serverId.c_str(), token.c_str());
        printf("= using the pairing saved for %s\n", serverId.c_str());
    }
    client.onState(onState, nullptr);
    client.onZones(onZones, &follow);
    client.onToken(saveToken, nullptr);
    client.onQueue(onQueue, nullptr);
    if (host) {
        client.setServer(host, port);
        printf("= server fixed at %s:%u\n", host, (unsigned)port);
    }
    client.begin(info);

    printf("keys: p play/pause, n next, b previous, s stats, q quit\n");
    uint32_t start = clock.nowMs();
    while (true) {
        client.tick();
        client.followQueue(follow.zoneId.c_str(), 10);   // nothing to do while it stays the same

        char c;
        if (keyPressed(c)) {
            if (c == 'q') break;
            if (c == 's' || c == 'p' || c == 'n' || c == 'b') {
                const Client::Stats &st = client.stats();
                if (c == 's') {
                    printf("= %u messages, %llu bytes, largest %zu, zone list %zu\n", st.messages,
                           (unsigned long long)st.totalBytes, st.maxLen, st.subscribedLen);
                    if (follow.seekTicks)
                        printf("= %d position pushes, gap min %d ms, mean %ld ms, max %d ms\n", follow.seekTicks,
                               follow.minGap, follow.gapSum / follow.seekTicks, follow.maxGap);
                } else if (!client.paired() || follow.zoneId.empty()) {
                    printf("! not following a zone yet\n");
                } else {
                    const char *action = c == 'p' ? "playpause" : (c == 'n' ? "next" : "previous");
                    Client::Reply r = client.control(follow.zoneId.c_str(), action);
                    printf("%s %s: %s\n", r.ok ? ">" : "!", action, r.ok ? "ok" : (*r.name ? r.name : r.error));
                }
            }
        }

        if (seconds && (int32_t)(clock.nowMs() - start) > seconds * 1000) break;
        usleep(5000);
    }

    const Client::Stats &st = client.stats();
    printf("\n= %s, %u messages, %llu bytes, largest %zu, zone list %zu\n", stateName(client.state()), st.messages,
           (unsigned long long)st.totalBytes, st.maxLen, st.subscribedLen);
    if (follow.seekTicks)
        printf("= %d position pushes, gap min %d ms, mean %ld ms, max %d ms\n", follow.seekTicks, follow.minGap,
               follow.gapSum / follow.seekTicks, follow.maxGap);
    return client.paired() ? 0 : 1;
}
