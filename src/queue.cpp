#include "tftcore/queue.h"

#include <cstring>
#include <new>

#include "tftcore/zones.h"

namespace tftcore {
namespace {

// Copies at most dstLen - 1 bytes without splitting a UTF-8 character.
void copyText(char *dst, size_t dstLen, const char *src) {
    size_t n = strlen(src);
    if (n > dstLen - 1) {
        n = dstLen - 1;
        while (n && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // back off to a lead byte
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

}  // namespace

QueueList::~QueueList() {
    if (items_) alloc_.release(items_);
}

bool QueueList::reserve(size_t maxItems) {
    size_ = 0;
    if (maxItems == cap_ && items_) return true;
    if (items_) alloc_.release(items_);
    items_ = nullptr;
    cap_ = 0;
    if (!maxItems) return true;
    void *p = alloc_.alloc(maxItems * sizeof(QueueItem));
    if (!p) return false;
    items_ = static_cast<QueueItem *>(p);
    for (size_t i = 0; i < maxItems; i++) new (items_ + i) QueueItem();   // raw memory from the allocator
    cap_ = maxItems;
    return true;
}

void QueueList::readItem(JsonVariantConst j, QueueItem &it) {
    it = QueueItem{};
    it.id = j["queue_item_id"] | 0u;
    it.lengthSec = j["length"] | 0u;
    const char *title, *artist, *album;
    readTrackLines(j, title, artist, album);
    copyText(it.title, sizeof it.title, title);
    copyText(it.artist, sizeof it.artist, artist);
    copyText(it.album, sizeof it.album, album);
    const char *key = j["image_key"] | "";
    if (strlen(key) < sizeof it.imageKey) strcpy(it.imageKey, key);   // a cut key names nothing
}

const QueueItem *QueueList::find(uint32_t id) const {
    for (const QueueItem &it : *this)
        if (it.id == id) return &it;
    return nullptr;
}

void QueueList::applySubscribed(JsonVariantConst body) {
    size_ = 0;
    insert(0, body["items"].as<JsonArrayConst>());
}

bool QueueList::applyChanged(JsonVariantConst body) {
    for (JsonVariantConst c : body["changes"].as<JsonArrayConst>()) {
        const char *op = c["operation"] | "";
        JsonVariantConst index = c["index"];
        if (!index.is<uint32_t>()) return false;
        bool ok;
        if (strcmp(op, "insert") == 0) ok = insert(index.as<uint32_t>(), c["items"].as<JsonArrayConst>());
        else if (strcmp(op, "remove") == 0) ok = remove(index.as<uint32_t>(), c["count"] | 0u);
        else ok = false;   // an operation we cannot apply leaves us out of step all the same
        if (!ok) return false;
    }
    return true;
}

// Items pushed past capacity() fall off the end: the server was asked for no more than that.
bool QueueList::insert(size_t index, JsonArrayConst items) {
    if (index > size_) return false;
    if (!cap_) return true;
    size_t add = items.size();
    if (index + add > cap_) add = cap_ - index;
    size_t kept = size_ - index;                       // the items after the insertion point
    if (index + add + kept > cap_) kept = cap_ - index - add;
    memmove(items_ + index + add, items_ + index, kept * sizeof(QueueItem));
    size_t i = 0;
    for (JsonVariantConst j : items) {
        if (i == add) break;
        readItem(j, items_[index + i]);
        i++;
    }
    size_ = index + add + kept;
    return true;
}

bool QueueList::remove(size_t index, size_t count) {
    if (index > size_ || count > size_ - index) return false;
    if (!count) return true;
    memmove(items_ + index, items_ + index + count, (size_ - index - count) * sizeof(QueueItem));
    size_ -= count;
    return true;
}

}  // namespace tftcore
