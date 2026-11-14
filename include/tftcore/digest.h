// SHA-1 and base64, needed only for the WebSocket handshake.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tftcore {

void sha1(const uint8_t *data, size_t len, uint8_t out[20]);
// Writes at most outLen bytes including the terminator; returns the text length, 0 if it did not fit.
size_t base64Encode(const uint8_t *data, size_t len, char *out, size_t outLen);

}  // namespace tftcore
