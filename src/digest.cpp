#include "tftcore/digest.h"

#include <cstring>

namespace tftcore {
namespace {

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1Block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) sha1Block(h, data + i * 64);

    uint8_t tail[128];
    size_t rest = len - full * 64;
    memcpy(tail, data + full * 64, rest);
    tail[rest++] = 0x80;
    size_t tailLen = (rest <= 56) ? 64 : 128;
    memset(tail + rest, 0, tailLen - rest);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tailLen - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1Block(h, tail);
    if (tailLen == 128) sha1Block(h, tail + 64);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

size_t base64Encode(const uint8_t *data, size_t len, char *out, size_t outLen) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4;
    if (outLen < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

}  // namespace tftcore
