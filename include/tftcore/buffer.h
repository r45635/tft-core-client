// Growable byte buffer. No exceptions: a failed growth is a false return.
#pragma once

#include <cstddef>
#include <cstdint>

#include "alloc.h"

namespace tftcore {

class Buffer {
  public:
    explicit Buffer(Alloc &a = defaultAlloc()) : alloc_(a) {}
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    bool reserve(size_t want);                     // capacity >= want
    bool append(const uint8_t *p, size_t n);
    bool append(const char *s);                    // without the terminator
    bool appendByte(uint8_t b) { return append(&b, 1); }
    bool setSize(size_t n);                        // grows, then sets the length (no init)
    void consume(size_t n);                        // drops the first n bytes
    void clear() { len_ = 0; }
    void shrink();                                 // frees the memory (capacity 0)

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }
    char *chars() { return reinterpret_cast<char *>(data_); }
    size_t size() const { return len_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return len_ == 0; }
    // Zero-terminates in place (without counting the 0 in size) so the bytes can be read as text.
    const char *cstr();

  private:
    Alloc &alloc_;
    uint8_t *data_ = nullptr;
    size_t len_ = 0, cap_ = 0;
};

}  // namespace tftcore
