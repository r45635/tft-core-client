#include "check.h"
#include "tftcore/zones.h"

using namespace tftcore;

namespace {

JsonDocument parse(const char *json) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    CHECK(!e);
    return doc;
}

const char *SUBSCRIBED = R"({"zones":[
  {"zone_id":"z1","display_name":"Living room","state":"playing",
   "is_play_allowed":false,"is_pause_allowed":true,"is_next_allowed":true,"is_previous_allowed":true,
   "now_playing":{"seek_position":42,"length":215,"image_key":"img1",
                  "three_line":{"line1":"A song","line2":"A band","line3":"An album"}},
   "outputs":[{"output_id":"o1","display_name":"Speaker",
               "volume":{"type":"db","min":-80,"max":0,"value":-34,"step":1,"is_muted":false}},
              {"output_id":"o2","display_name":"Amp"}]},
  {"zone_id":"z2","display_name":"Kitchen","state":"stopped","outputs":[]}
]})";

}  // namespace

void testZones() {
    ZoneList list;
    ZoneDelta delta;

    // A Subscribed replaces everything.
    {
        JsonDocument doc = parse(SUBSCRIBED);
        list.applySubscribed(doc.as<JsonVariantConst>(), delta);
        CHECK(delta.full);
        CHECK(delta.listChanged);
        CHECK_INT(list.zones().size(), 2);
        const Zone *z = list.find("z1");
        CHECK(z != nullptr);
        if (z) {
            CHECK_STR(z->name.c_str(), "Living room");
            CHECK(z->playing());
            CHECK(!z->canPlay);
            CHECK(z->canPause);
            CHECK_STR(z->nowPlaying.title.c_str(), "A song");
            CHECK_STR(z->nowPlaying.artist.c_str(), "A band");
            CHECK_STR(z->nowPlaying.album.c_str(), "An album");
            CHECK_STR(z->nowPlaying.imageKey.c_str(), "img1");
            CHECK_INT(z->nowPlaying.lengthSec, 215);
            CHECK_INT(z->nowPlaying.seekSec, 42);
            CHECK_INT(z->outputs.size(), 2);
            CHECK(z->outputs[0].volume.present);
            CHECK_STR(z->outputs[0].volume.type.c_str(), "db");
            CHECK_INT((int)z->outputs[0].volume.value, -34);
            CHECK(!z->outputs[1].volume.present);   // an output with no volume control
        }
        const Zone *kitchen = list.find("z2");
        CHECK(kitchen != nullptr);
        if (kitchen) CHECK(!kitchen->nowPlaying.present);
    }

    // A seek tick moves the position and nothing else.
    {
        JsonDocument doc = parse(R"({"zones_seek_changed":[{"zone_id":"z1","seek_position":43,
                                     "queue_time_remaining":120}]})");
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK(delta.seekOnly);
        CHECK(!delta.listChanged);
        CHECK(!delta.full);
        CHECK_INT(list.find("z1")->nowPlaying.seekSec, 43);
        CHECK_STR(list.find("z1")->nowPlaying.title.c_str(), "A song");
    }

    // A seek for a zone we do not know is ignored.
    {
        JsonDocument doc = parse(R"({"zones_seek_changed":[{"zone_id":"nope","seek_position":9}]})");
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK(!delta.listChanged);
        CHECK_INT(list.zones().size(), 2);
    }

    // A changed zone replaces the old one in place.
    {
        JsonDocument doc = parse(R"({"zones_changed":[{"zone_id":"z1","display_name":"Living room",
            "state":"paused","now_playing":{"seek_position":0,"length":100,
            "two_line":{"line1":"Another","line2":"Someone"}},"outputs":[]}]})");
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK(delta.listChanged);
        CHECK(!delta.seekOnly);
        CHECK_INT(list.zones().size(), 2);
        const Zone *z = list.find("z1");
        CHECK_STR(z->state.c_str(), "paused");
        CHECK_STR(z->nowPlaying.title.c_str(), "Another");
        CHECK_STR(z->nowPlaying.artist.c_str(), "Someone");
        CHECK_STR(z->nowPlaying.album.c_str(), "");      // two_line carries no album
        CHECK_INT(z->outputs.size(), 0);
    }

    // An added zone appears; a removed one goes.
    {
        JsonDocument doc = parse(R"({"zones_added":[{"zone_id":"z3","display_name":"Study",
                                     "state":"playing","outputs":[]}],"zones_removed":["z2"]})");
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK(delta.listChanged);
        CHECK_INT(list.zones().size(), 2);
        CHECK(list.find("z2") == nullptr);
        CHECK(list.find("z3") != nullptr);
    }

    // A zone sent as "changed" but never seen before is added, not dropped.
    {
        JsonDocument doc = parse(R"({"zones_changed":[{"zone_id":"z4","display_name":"Bedroom",
                                     "state":"stopped","outputs":[]}]})");
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK_INT(list.zones().size(), 3);
        CHECK(list.find("z4") != nullptr);
    }

    // Picking a zone: the pinned one, else the one playing, else one with a track, else the first.
    {
        CHECK_STR(list.pick("z4")->id.c_str(), "z4");          // pinned wins
        CHECK_STR(list.pick("gone")->id.c_str(), "z3");        // z3 is the playing one
        CHECK_STR(list.pick(nullptr)->id.c_str(), "z3");
        JsonDocument doc = parse(R"({"zones":[{"zone_id":"a","state":"stopped","outputs":[]},
            {"zone_id":"b","state":"paused","now_playing":{"length":10,
             "one_line":{"line1":"Held"}},"outputs":[]}]})");
        ZoneList other;
        other.applySubscribed(doc.as<JsonVariantConst>(), delta);
        CHECK_STR(other.pick(nullptr)->id.c_str(), "b");       // nothing playing: the loaded one
        CHECK_STR(other.find("b")->nowPlaying.title.c_str(), "Held");
        JsonDocument none = parse(R"({"zones":[]})");
        ZoneList empty;
        empty.applySubscribed(none.as<JsonVariantConst>(), delta);
        CHECK(empty.pick(nullptr) == nullptr);
    }

    // A missing seek_position leaves the last known position alone.
    {
        JsonDocument doc = parse(R"({"zones_seek_changed":[{"zone_id":"z1"}]})");
        int32_t before = list.find("z1")->nowPlaying.seekSec;
        list.applyChanged(doc.as<JsonVariantConst>(), delta);
        CHECK_INT(list.find("z1")->nowPlaying.seekSec, before);
    }
}
