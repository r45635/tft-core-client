// SOOD discovery (docs/PROTOCOL.md section 1).
#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer.h"

namespace tftcore {

constexpr const char *SOOD_SERVICE_ID = "00720724-5143-4a9b-abac-0e50cba674bb";
constexpr const char *SOOD_MULTICAST = "239.255.90.90";
constexpr uint16_t SOOD_PORT = 9003;

struct SoodReply {
    char ip[46] = {0};            // filled by the caller from the UDP source address
    uint16_t httpPort = 0;        // Roon 2.x: 9330
    char uniqueId[72] = {0};
    char name[72] = {0};
    char displayVersion[40] = {0};
};

// Query for the music server service. `tid` is echoed back in the reply.
bool soodBuildQuery(Buffer &out, const char *tid);
// Parses a reply packet. false = not a SOOD reply for our service, or malformed.
bool soodParseReply(const uint8_t *buf, size_t len, SoodReply &r);

// How good a server address looks from where we stand. A server with several interfaces
// answers from each of them and some of those addresses lead nowhere from here, so the
// highest score of a discovery window wins. `localIp` may be empty when we do not know ours.
//   5 same /24 as us   4 same /16   3 another routable address   2 loopback   1 link-local
// A string that is not an IPv4 address scores 0.
int soodAddressScore(const char *ip, const char *localIp);

}  // namespace tftcore
