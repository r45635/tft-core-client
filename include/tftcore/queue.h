// A zone's play queue and the merge rules of a queue subscription (docs/PROTOCOL.md section 5).
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "alloc.h"

namespace tftcore {

// Fixed size, so a whole queue is one block in the memory the caller chose: no per-item
// allocation lands in internal RAM. Longer text is cut at a character boundary.
struct QueueItem {
    static constexpr size_t TEXT_LEN = 128;   // bytes per display line, terminator included
    static constexpr size_t KEY_LEN = 48;     // an image key is 32 characters today

    uint32_t id = 0;          // queue_item_id: what playFromHere() takes
    uint32_t lengthSec = 0;   // 0 = not reported
    char title[TEXT_LEN] = {0};
    char artist[TEXT_LEN] = {0};
    char album[TEXT_LEN] = {0};
    char imageKey[KEY_LEN] = {0};   // "" when absent, or too long to be a key we can use
};

class QueueList {
  public:
    explicit QueueList(Alloc &alloc = defaultAlloc()) : alloc_(alloc) {}
    ~QueueList();
    QueueList(const QueueList &) = delete;
    QueueList &operator=(const QueueList &) = delete;

    // Room for `maxItems`, taken once; the list never grows past it. Drops the current items.
    bool reserve(size_t maxItems);

    // `Subscribed` body: {"items": [...]}. Items past capacity() are dropped.
    void applySubscribed(JsonVariantConst body);
    // `Changed` body: {"changes": [{operation: "insert", index, items} | {operation: "remove",
    // index, count}]}, applied in order. false = a change did not fit the list we hold, so it no
    // longer matches the server's: subscribe again.
    bool applyChanged(JsonVariantConst body);
    void clear() { size_ = 0; }

    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }
    const QueueItem &operator[](size_t i) const { return items_[i]; }
    const QueueItem *begin() const { return items_; }
    const QueueItem *end() const { return items_ + size_; }
    const QueueItem *find(uint32_t id) const;

    // Reads one queue item object into `it` (used by the merge and by tests).
    static void readItem(JsonVariantConst j, QueueItem &it);

  private:
    bool insert(size_t index, JsonArrayConst items);
    bool remove(size_t index, size_t count);

    Alloc &alloc_;
    QueueItem *items_ = nullptr;
    size_t size_ = 0, cap_ = 0;
};

}  // namespace tftcore
