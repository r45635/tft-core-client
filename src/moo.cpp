#include "tftcore/moo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tftcore {
namespace {

// Copies a field without its surrounding spaces, always terminated.
void copyField(char *dst, size_t dstLen, const char *src, size_t n) {
    while (n && (*src == ' ' || *src == '\t')) { src++; n--; }
    while (n && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r')) n--;
    if (n > dstLen - 1) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool equalsNoCase(const char *a, size_t aLen, const char *b) {
    size_t bLen = strlen(b);
    if (aLen != bLen) return false;
    for (size_t i = 0; i < aLen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

}  // namespace

bool MooHeader::is(const char *n) const { return n && strcmp(name, n) == 0; }

bool mooParse(const uint8_t *frame, size_t len, MooHeader &h) {
    h = MooHeader{};
    const char *p = reinterpret_cast<const char *>(frame);
    const char *end = p + len;

    // First line: MOO/<version> <VERB> <name>
    const char *nl = static_cast<const char *>(memchr(p, '\n', len));
    if (!nl) return false;
    if (nl - p < 10 || memcmp(p, "MOO/", 4) != 0) return false;
    const char *sp1 = static_cast<const char *>(memchr(p + 4, ' ', nl - (p + 4)));
    if (!sp1) return false;
    const char *verb = sp1 + 1;
    const char *sp2 = static_cast<const char *>(memchr(verb, ' ', nl - verb));
    if (!sp2) return false;
    size_t verbLen = sp2 - verb;
    if (equalsNoCase(verb, verbLen, "REQUEST")) h.verb = MooVerb::Request;
    else if (equalsNoCase(verb, verbLen, "COMPLETE")) h.verb = MooVerb::Complete;
    else if (equalsNoCase(verb, verbLen, "CONTINUE")) h.verb = MooVerb::Continue;
    else h.verb = MooVerb::Unknown;

    const char *name = sp2 + 1;
    size_t nameLen = nl - name;
    if (nameLen && name[nameLen - 1] == '\r') nameLen--;
    if (h.verb == MooVerb::Request) {
        // "com.roonlabs.ping:1/ping" -> service + method
        const char *slash = static_cast<const char *>(memchr(name, '/', nameLen));
        if (slash) {
            copyField(h.service, sizeof h.service, name, slash - name);
            copyField(h.name, sizeof h.name, slash + 1, nameLen - (slash - name) - 1);
        } else {
            copyField(h.name, sizeof h.name, name, nameLen);
        }
    } else {
        copyField(h.name, sizeof h.name, name, nameLen);
    }

    // Header lines until an empty one.
    bool haveLength = false, haveType = false, haveId = false;
    p = nl + 1;
    while (p < end) {
        nl = static_cast<const char *>(memchr(p, '\n', end - p));
        size_t lineLen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        size_t bare = lineLen;
        if (bare && p[bare - 1] == '\r') bare--;
        if (bare == 0) {                       // blank line: the body follows
            h.bodyOffset = nl ? (size_t)(nl + 1 - reinterpret_cast<const char *>(frame)) : len;
            break;
        }
        const char *colon = static_cast<const char *>(memchr(p, ':', bare));
        if (!colon) return false;
        size_t keyLen = colon - p;
        const char *val = colon + 1;
        size_t valLen = bare - keyLen - 1;
        if (equalsNoCase(p, keyLen, "Request-Id")) {
            char buf[16];
            copyField(buf, sizeof buf, val, valLen);
            h.requestId = (int)strtol(buf, nullptr, 10);
            haveId = true;
        } else if (equalsNoCase(p, keyLen, "Content-Length")) {
            char buf[24];
            copyField(buf, sizeof buf, val, valLen);
            h.contentLength = (size_t)strtoul(buf, nullptr, 10);
            haveLength = true;
        } else if (equalsNoCase(p, keyLen, "Content-Type")) {
            copyField(h.contentType, sizeof h.contentType, val, valLen);
            haveType = true;
        }
        if (!nl) {
            h.bodyOffset = len;
            break;
        }
        p = nl + 1;
    }

    if (!haveId) return false;                      // every message carries one
    if (haveLength != haveType) return false;       // they come as a pair
    if (!h.bodyOffset) h.bodyOffset = len;
    if (!haveLength && h.bodyOffset < len) return false;   // body with no length
    if (haveLength && h.bodyOffset + h.contentLength > len) return false;
    h.json = haveType && strncmp(h.contentType, "application/json", 16) == 0;
    return true;
}

namespace {

bool build(Buffer &out, const char *verb, const char *name, int requestId, const char *jsonBody) {
    out.clear();
    char head[128];
    int n = snprintf(head, sizeof head, "MOO/1 %s %s\nRequest-Id: %d\n", verb, name ? name : "", requestId);
    if (n <= 0 || (size_t)n >= sizeof head) return false;
    if (!out.append(head)) return false;
    if (jsonBody) {
        size_t len = strlen(jsonBody);
        char ct[80];
        n = snprintf(ct, sizeof ct, "Content-Length: %zu\nContent-Type: application/json\n", len);
        if (n <= 0 || (size_t)n >= sizeof ct) return false;
        if (!out.append(ct) || !out.append("\n") || !out.append(jsonBody)) return false;
    } else {
        if (!out.append("\n")) return false;
    }
    return true;
}

}  // namespace

bool mooBuildRequest(Buffer &out, const char *name, int requestId, const char *jsonBody) {
    return build(out, "REQUEST", name, requestId, jsonBody);
}

bool mooBuildComplete(Buffer &out, const char *name, int requestId, const char *jsonBody) {
    return build(out, "COMPLETE", name, requestId, jsonBody);
}

}  // namespace tftcore
