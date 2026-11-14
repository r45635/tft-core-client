#include "tftcore/sood.h"

#include <cstdlib>
#include <cstring>

namespace tftcore {
namespace {

bool appendProp(Buffer &b, const char *name, const char *value) {
    size_t nameLen = strlen(name), valueLen = strlen(value);
    if (nameLen > 255 || valueLen > 0xfffe) return false;
    if (!b.appendByte((uint8_t)nameLen) || !b.append(name)) return false;
    if (!b.appendByte((uint8_t)(valueLen >> 8)) || !b.appendByte((uint8_t)(valueLen & 0xff))) return false;
    return b.append(value);
}

}  // namespace

bool soodBuildQuery(Buffer &out, const char *tid) {
    out.clear();
    if (!out.append("SOOD")) return false;
    if (!out.appendByte(0x02) || !out.appendByte('Q')) return false;
    if (!appendProp(out, "query_service_id", SOOD_SERVICE_ID)) return false;
    if (tid && *tid && !appendProp(out, "_tid", tid)) return false;
    return true;
}

bool soodParseReply(const uint8_t *buf, size_t len, SoodReply &out) {
    if (len < 6 || memcmp(buf, "SOOD", 4) != 0) return false;
    if (buf[4] != 0x02 || buf[5] != 'R') return false;

    // Filled only on success: a caller must never see half a reply from a packet we rejected.
    SoodReply r;
    bool serviceOk = false;
    size_t p = 6;
    while (p < len) {
        size_t nameLen = buf[p++];
        if (!nameLen || p + nameLen + 2 > len) return false;
        const char *name = reinterpret_cast<const char *>(buf + p);
        p += nameLen;
        size_t valueLen = ((size_t)buf[p] << 8) | buf[p + 1];
        p += 2;
        if (valueLen == 0xffff) continue;            // null property
        if (p + valueLen > len) return false;
        const char *value = reinterpret_cast<const char *>(buf + p);
        p += valueLen;

        auto named = [&](const char *want) {
            return nameLen == strlen(want) && memcmp(name, want, nameLen) == 0;
        };
        auto copy = [&](char *dst, size_t dstLen) {
            size_t n = valueLen < dstLen - 1 ? valueLen : dstLen - 1;
            memcpy(dst, value, n);
            dst[n] = 0;
        };

        if (named("service_id")) {
            serviceOk = valueLen == strlen(SOOD_SERVICE_ID) && memcmp(value, SOOD_SERVICE_ID, valueLen) == 0;
        } else if (named("unique_id")) {
            copy(r.uniqueId, sizeof r.uniqueId);
        } else if (named("http_port")) {
            char port[8];
            copy(port, sizeof port);
            r.httpPort = (uint16_t)strtoul(port, nullptr, 10);
        } else if (named("name")) {
            copy(r.name, sizeof r.name);
        } else if (named("display_version")) {
            copy(r.displayVersion, sizeof r.displayVersion);
        }
    }
    if (!serviceOk || !r.uniqueId[0] || r.httpPort == 0) return false;
    out = r;
    return true;
}

namespace {

// Written out rather than left to sscanf: the scanf family costs tens of kilobytes of flash,
// which a 4 MB board cannot spare for four numbers.
bool parseIpv4(const char *s, unsigned out[4]) {
    if (!s) return false;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3) return false;
        }
        if (v > 255) return false;
        out[i] = v;
        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    return *s == 0;
}

}  // namespace

int soodAddressScore(const char *ip, const char *localIp) {
    unsigned a[4];
    if (!parseIpv4(ip, a)) return 0;
    if (a[0] == 169 && a[1] == 254) return 1;   // 169.254/16: a link-local address goes nowhere
    if (a[0] == 127) return 2;                  // the server runs on this machine
    unsigned l[4];
    if (localIp && *localIp && parseIpv4(localIp, l)) {
        if (l[0] == a[0] && l[1] == a[1] && l[2] == a[2]) return 5;
        if (l[0] == a[0] && l[1] == a[1]) return 4;
    }
    return 3;
}

}  // namespace tftcore
