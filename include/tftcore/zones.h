// The zone model and the merge rules of a zone subscription (docs/PROTOCOL.md section 5).
#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tftcore {

struct Volume {
    bool present = false;
    std::string type;    // "number" | "db" | "incremental" ("" = none)
    float min = 0, max = 0, value = 0, step = 1;
    bool muted = false;
};

struct Output {
    std::string id, name;
    Volume volume;
    // The ids of the outputs it can be grouped with, itself included, separated by spaces: one
    // allocation per output rather than one per id, since every output lists most of the others.
    std::string canGroupWith;

    bool groupableWith(const char *outputId) const;
};

struct NowPlaying {
    bool present = false;
    std::string title, artist, album, imageKey;
    uint32_t lengthSec = 0;
    int32_t seekSec = -1;   // -1 = not reported
};

struct Zone {
    std::string id, name, state;   // "playing" | "paused" | "loading" | "stopped"
    bool canPlay = false, canPause = false, canNext = false, canPrevious = false, canSeek = false;
    NowPlaying nowPlaying;
    std::vector<Output> outputs;

    bool playing() const { return state == "playing"; }
};

// What a Changed message touched, so a client can tell a seek tick from a new track.
struct ZoneDelta {
    bool full = false;        // a Subscribed: every zone was replaced
    bool listChanged = false; // a zone was added, removed, renamed or its track/state changed
    bool seekOnly = false;    // nothing but zones_seek_changed
};

// Roon gives a track as one, two or three display lines, best first; the best one present wins.
// Zones and queue items both carry them. The pointers live as long as `j`; "" when absent.
void readTrackLines(JsonVariantConst j, const char *&title, const char *&artist, const char *&album);

class ZoneList {
  public:
    // `Subscribed` body: {"zones": [...]}
    void applySubscribed(JsonVariantConst body, ZoneDelta &delta);
    // `Changed` body: zones_added / zones_changed / zones_removed / zones_seek_changed
    void applyChanged(JsonVariantConst body, ZoneDelta &delta);
    void clear() { zones_.clear(); }

    const std::vector<Zone> &zones() const { return zones_; }
    const Zone *find(const char *zoneId) const;
    // First playing zone, else the first with a track, else the first, else null.
    const Zone *pick(const char *preferredId) const;

    // Reads one zone object into `z` (used by the merge and by tests).
    static void readZone(JsonVariantConst j, Zone &z);

  private:
    Zone *findMutable(const char *zoneId);
    std::vector<Zone> zones_;
};

}  // namespace tftcore
