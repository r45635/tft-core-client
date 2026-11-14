#include "tftcore/buffer.h"

#include <cstdlib>
#include <cstring>

namespace tftcore {
namespace {

struct MallocAlloc : Alloc {
    void *alloc(size_t n) override { return ::malloc(n); }
    void *realloc(void *p, size_t n) override { return ::realloc(p, n); }
    void release(void *p) override { ::free(p); }
};

}  // namespace

Alloc &defaultAlloc() {
    static MallocAlloc a;
    return a;
}

Buffer::~Buffer() { shrink(); }

void Buffer::shrink() {
    if (data_) alloc_.release(data_);
    data_ = nullptr;
    len_ = 0;
    cap_ = 0;
}

bool Buffer::reserve(size_t want) {
    if (want <= cap_) return true;
    size_t cap = cap_ ? cap_ : 128;
    while (cap < want) {
        if (cap > (size_t)1 << 27) return false;   // 128 MB: a bug, not a message
        cap *= 2;
    }
    void *p = data_ ? alloc_.realloc(data_, cap) : alloc_.alloc(cap);
    if (!p) return false;
    data_ = static_cast<uint8_t *>(p);
    cap_ = cap;
    return true;
}

bool Buffer::append(const uint8_t *p, size_t n) {
    if (!n) return true;
    if (!reserve(len_ + n)) return false;
    memcpy(data_ + len_, p, n);
    len_ += n;
    return true;
}

bool Buffer::append(const char *s) {
    return s ? append(reinterpret_cast<const uint8_t *>(s), strlen(s)) : true;
}

bool Buffer::setSize(size_t n) {
    if (!reserve(n)) return false;
    len_ = n;
    return true;
}

void Buffer::consume(size_t n) {
    if (n >= len_) {
        len_ = 0;
        return;
    }
    memmove(data_, data_ + n, len_ - n);
    len_ -= n;
}

const char *Buffer::cstr() {
    if (!reserve(len_ + 1)) return "";
    data_[len_] = 0;
    return reinterpret_cast<const char *>(data_);
}

}  // namespace tftcore
