#include <cstdlib>
#include <string>

#include "check.h"
#include "tftcore/queue.h"

using namespace tftcore;

namespace {

JsonDocument parse(const char *json) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    CHECK(!e);
    return doc;
}

// The queue as "id,id,id", so a test reads the expected order at a glance.
std::string ids(const QueueList &q) {
    std::string s;
    for (const QueueItem &it : q) {
        if (!s.empty()) s += ",";
        s += std::to_string(it.id);
    }
    return s;
}

bool changed(QueueList &q, const char *json) {
    JsonDocument doc = parse(json);
    return q.applyChanged(doc.as<JsonVariantConst>());
}

void subscribed(QueueList &q, const char *json) {
    JsonDocument doc = parse(json);
    q.applySubscribed(doc.as<JsonVariantConst>());
}

struct CountingAlloc : Alloc {
    void *alloc(size_t n) override {
        allocs++;
        bytes += n;
        return malloc(n);
    }
    void *realloc(void *p, size_t n) override { return ::realloc(p, n); }
    void release(void *p) override {
        releases++;
        free(p);
    }
    int allocs = 0, releases = 0;
    size_t bytes = 0;
};

const char *SUBSCRIBED = R"({"items":[
  {"queue_item_id":1,"length":215,"image_key":"img1",
   "three_line":{"line1":"A song","line2":"A band","line3":"An album"}},
  {"queue_item_id":2,"length":100,"two_line":{"line1":"Another","line2":"Someone"}},
  {"queue_item_id":3,"one_line":{"line1":"Just a title"}}
]})";

}  // namespace

void testQueue() {
    QueueList q;
    CHECK(q.reserve(10));
    CHECK_INT(q.capacity(), 10);

    // A Subscribed replaces everything, each item read from whichever lines it carries.
    {
        subscribed(q, SUBSCRIBED);
        CHECK_STR(ids(q).c_str(), "1,2,3");
        CHECK_STR(q[0].title, "A song");
        CHECK_STR(q[0].artist, "A band");
        CHECK_STR(q[0].album, "An album");
        CHECK_STR(q[0].imageKey, "img1");
        CHECK_INT(q[0].lengthSec, 215);
        CHECK_STR(q[1].title, "Another");
        CHECK_STR(q[1].artist, "Someone");
        CHECK_STR(q[1].album, "");
        CHECK_STR(q[1].imageKey, "");
        CHECK_STR(q[2].title, "Just a title");
        CHECK_INT(q[2].lengthSec, 0);
        CHECK(q.find(2) == &q[1]);
        CHECK(q.find(99) == nullptr);
    }

    // The playing track ends: it leaves the head of the queue.
    {
        CHECK(changed(q, R"({"changes":[{"operation":"remove","index":0,"count":1}]})"));
        CHECK_STR(ids(q).c_str(), "2,3");
        CHECK_STR(q[0].title, "Another");
    }

    // Tracks added in the middle keep their order and push the rest down.
    {
        CHECK(changed(q, R"({"changes":[{"operation":"insert","index":1,"items":[
            {"queue_item_id":10,"one_line":{"line1":"Ten"}},
            {"queue_item_id":11,"one_line":{"line1":"Eleven"}}]}]})"));
        CHECK_STR(ids(q).c_str(), "2,10,11,3");
        CHECK_STR(q[1].title, "Ten");
        CHECK_STR(q[3].title, "Just a title");
    }

    // Several changes in one message apply in order, each on the result of the one before.
    {
        CHECK(changed(q, R"({"changes":[{"operation":"remove","index":0,"count":2},
            {"operation":"insert","index":0,"items":[{"queue_item_id":12}]},
            {"operation":"insert","index":3,"items":[{"queue_item_id":13}]}]})"));
        CHECK_STR(ids(q).c_str(), "12,11,3,13");
    }

    // A change that does not fit the list we hold says so, and the caller subscribes again.
    {
        CHECK(!changed(q, R"({"changes":[{"operation":"remove","index":3,"count":2}]})"));
        CHECK(!changed(q, R"({"changes":[{"operation":"remove","index":5,"count":0}]})"));
        CHECK(!changed(q, R"({"changes":[{"operation":"insert","index":5,"items":[{"queue_item_id":1}]}]})"));
        CHECK(!changed(q, R"({"changes":[{"operation":"move","index":0}]})"));
        CHECK(!changed(q, R"({"changes":[{"operation":"remove","count":1}]})"));
        CHECK(!changed(q, R"({"changes":[{"operation":"remove","index":-1,"count":1}]})"));
        CHECK_STR(ids(q).c_str(), "12,11,3,13");   // nothing above touched it
        CHECK(changed(q, R"({"changes":[{"operation":"insert","index":4,"items":[{"queue_item_id":14}]}]})"));
        CHECK_STR(ids(q).c_str(), "12,11,3,13,14");   // inserting at the very end is fine
        CHECK(changed(q, R"({})"));                    // no changes, nothing to do
        CHECK_STR(ids(q).c_str(), "12,11,3,13,14");
    }

    // A new Subscribed (after a resubscription) starts over rather than adding to what is there.
    {
        subscribed(q, SUBSCRIBED);
        CHECK_STR(ids(q).c_str(), "1,2,3");
    }

    // The list never outgrows what was reserved: what would go past the end is dropped.
    {
        QueueList small;
        CHECK(small.reserve(3));
        subscribed(small, R"({"items":[{"queue_item_id":1},{"queue_item_id":2},{"queue_item_id":3},
                                        {"queue_item_id":4},{"queue_item_id":5}]})");
        CHECK_STR(ids(small).c_str(), "1,2,3");
        CHECK(changed(small, R"({"changes":[{"operation":"insert","index":1,"items":[{"queue_item_id":9}]}]})"));
        CHECK_STR(ids(small).c_str(), "1,9,2");
        CHECK(changed(small, R"({"changes":[{"operation":"insert","index":0,"items":[
            {"queue_item_id":7},{"queue_item_id":8},{"queue_item_id":6},{"queue_item_id":5}]}]})"));
        CHECK_STR(ids(small).c_str(), "7,8,6");
        CHECK(changed(small, R"({"changes":[{"operation":"remove","index":0,"count":3}]})"));
        CHECK_STR(ids(small).c_str(), "");
    }

    // Long text is cut on a character boundary; a key too long to be whole is left out.
    {
        std::string title(126, 'a');
        title += "\xC3\xA9";   // é straddles the last byte: dropped whole
        std::string key(QueueItem::KEY_LEN, 'k');
        std::string json = R"({"queue_item_id":1,"image_key":")" + key + R"(","one_line":{"line1":")" + title + "\"}}";
        JsonDocument doc = parse(json.c_str());
        QueueItem it;
        JsonDocument before = parse(R"({"queue_item_id":7,"image_key":"img7","one_line":{"line1":"x"}})");
        QueueList::readItem(before.as<JsonVariantConst>(), it);
        CHECK_STR(it.imageKey, "img7");
        QueueList::readItem(doc.as<JsonVariantConst>(), it);
        CHECK_STR(it.title, std::string(126, 'a').c_str());
        CHECK_STR(it.imageKey, "");   // not the previous item's

        std::string fits = R"({"one_line":{"line1":")" + std::string(127, 'b') + "\"}}";
        JsonDocument doc2 = parse(fits.c_str());
        QueueList::readItem(doc2.as<JsonVariantConst>(), it);
        CHECK_INT(strlen(it.title), 127);
        CHECK_INT(it.id, 0);
    }

    // One block from the caller's allocator, held while the size stays, released at the end.
    {
        CountingAlloc counting;
        {
            QueueList owned(counting);
            CHECK_INT(counting.allocs, 0);   // nothing until reserve()
            CHECK(owned.reserve(20));
            CHECK(owned.reserve(20));
            subscribed(owned, SUBSCRIBED);
            CHECK(changed(owned, R"({"changes":[{"operation":"remove","index":0,"count":1}]})"));
            CHECK_INT(counting.allocs, 1);
            CHECK_INT(counting.bytes, 20 * sizeof(QueueItem));
            CHECK(owned.reserve(0));
            CHECK_INT(owned.capacity(), 0);
            subscribed(owned, SUBSCRIBED);
            CHECK_INT(owned.size(), 0);
            CHECK(changed(owned, R"({"changes":[{"operation":"remove","index":0,"count":0}]})"));
        }
        CHECK_INT(counting.releases, 1);
    }
}
