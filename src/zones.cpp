#include "tftcore/zones.h"

#include <cstring>

namespace tftcore {
namespace {

// Roon gives the track as one, two or three display lines, best first.
void readLines(JsonVariantConst np, NowPlaying &out) {
    JsonVariantConst three = np["three_line"];
    JsonVariantConst two = np["two_line"];
    JsonVariantConst one = np["one_line"];
    if (!three.isNull()) {
        out.title = three["line1"] | "";
        out.artist = three["line2"] | "";
        out.album = three["line3"] | "";
    } else if (!two.isNull()) {
        out.title = two["line1"] | "";
        out.artist = two["line2"] | "";
    } else if (!one.isNull()) {
        out.title = one["line1"] | "";
    }
}

}  // namespace

void ZoneList::readZone(JsonVariantConst j, Zone &z) {
    z = Zone{};
    z.id = j["zone_id"] | "";
    z.name = j["display_name"] | "";
    z.state = j["state"] | "";
    z.canPlay = j["is_play_allowed"] | false;
    z.canPause = j["is_pause_allowed"] | false;
    z.canNext = j["is_next_allowed"] | false;
    z.canPrevious = j["is_previous_allowed"] | false;
    z.canSeek = j["is_seek_allowed"] | false;

    JsonVariantConst np = j["now_playing"];
    if (!np.isNull()) {
        z.nowPlaying.present = true;
        readLines(np, z.nowPlaying);
        z.nowPlaying.imageKey = np["image_key"] | "";
        z.nowPlaying.lengthSec = np["length"] | 0u;
        z.nowPlaying.seekSec = np["seek_position"].isNull() ? -1 : (int32_t)(np["seek_position"] | 0);
    }

    for (JsonVariantConst o : j["outputs"].as<JsonArrayConst>()) {
        Output out;
        out.id = o["output_id"] | "";
        out.name = o["display_name"] | "";
        JsonVariantConst v = o["volume"];
        if (!v.isNull()) {
            out.volume.present = true;
            out.volume.type = v["type"] | "";
            out.volume.min = v["min"] | 0.0f;
            out.volume.max = v["max"] | 0.0f;
            out.volume.value = v["value"] | 0.0f;
            out.volume.step = v["step"] | 1.0f;
            out.volume.muted = v["is_muted"] | false;
        }
        z.outputs.push_back(std::move(out));
    }
}

const Zone *ZoneList::find(const char *zoneId) const {
    if (!zoneId || !*zoneId) return nullptr;
    for (const Zone &z : zones_)
        if (z.id == zoneId) return &z;
    return nullptr;
}

Zone *ZoneList::findMutable(const char *zoneId) {
    if (!zoneId || !*zoneId) return nullptr;
    for (Zone &z : zones_)
        if (z.id == zoneId) return &z;
    return nullptr;
}

const Zone *ZoneList::pick(const char *preferredId) const {
    if (const Zone *p = find(preferredId)) return p;
    for (const Zone &z : zones_)
        if (z.playing()) return &z;
    for (const Zone &z : zones_)
        if (z.nowPlaying.present) return &z;
    return zones_.empty() ? nullptr : &zones_.front();
}

void ZoneList::applySubscribed(JsonVariantConst body, ZoneDelta &delta) {
    delta = ZoneDelta{};
    delta.full = true;
    delta.listChanged = true;
    zones_.clear();
    for (JsonVariantConst j : body["zones"].as<JsonArrayConst>()) {
        Zone z;
        readZone(j, z);
        if (!z.id.empty()) zones_.push_back(std::move(z));
    }
}

void ZoneList::applyChanged(JsonVariantConst body, ZoneDelta &delta) {
    delta = ZoneDelta{};

    for (JsonVariantConst id : body["zones_removed"].as<JsonArrayConst>()) {
        const char *zid = id.as<const char *>();
        if (!zid) continue;
        for (size_t i = 0; i < zones_.size(); i++)
            if (zones_[i].id == zid) {
                zones_.erase(zones_.begin() + (long)i);
                delta.listChanged = true;
                break;
            }
    }

    // Added and changed carry a whole zone each; a zone we do not know yet is appended
    // either way (a regrouped output arrives as "added" on some Cores and "changed" on others).
    for (const char *key : {"zones_added", "zones_changed"}) {
        for (JsonVariantConst j : body[key].as<JsonArrayConst>()) {
            Zone z;
            readZone(j, z);
            if (z.id.empty()) continue;
            delta.listChanged = true;
            if (Zone *existing = findMutable(z.id.c_str())) *existing = std::move(z);
            else zones_.push_back(std::move(z));
        }
    }

    bool seek = false;
    for (JsonVariantConst s : body["zones_seek_changed"].as<JsonArrayConst>()) {
        const char *zid = s["zone_id"] | "";
        Zone *z = findMutable(zid);
        if (!z) continue;
        seek = true;
        if (!s["seek_position"].isNull() && z->nowPlaying.present)
            z->nowPlaying.seekSec = (int32_t)(s["seek_position"] | 0);
    }
    delta.seekOnly = seek && !delta.listChanged;
}

}  // namespace tftcore
