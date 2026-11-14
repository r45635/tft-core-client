// Where the library takes its memory from.
#pragma once

#include <cstddef>

namespace tftcore {

// On an ESP32 with PSRAM, internal RAM is the scarce resource: the Wi-Fi stack needs it and
// every small malloc lands there. A caller that has PSRAM passes an allocator that uses it,
// and every buffer and JSON document in this library follows.
struct Alloc {
    virtual ~Alloc() = default;
    virtual void *alloc(size_t n) = 0;
    virtual void *realloc(void *p, size_t newSize) = 0;
    virtual void release(void *p) = 0;
};

Alloc &defaultAlloc();   // malloc / realloc / free

}  // namespace tftcore
