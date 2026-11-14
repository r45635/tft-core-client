// Pairs with a music server on the local network, follows one zone and prints the position as
// the server pushes it. Serial console: p play/pause, n next, b previous, s stats, f forget.
//
// Measurements it prints, for sizing the library on a small board: internal RAM free at each
// step, the largest message seen, and how regularly the position arrives.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>

#include <tftcore/arduino_io.h>
#ifndef NO_CLIENT
#include "tftcore/client.h"
#endif

namespace {

tftcore::arduino::MillisClock g_clock;
#ifndef NO_CLIENT
tftcore::arduino::TcpStream g_stream;
tftcore::arduino::Udp g_udp;
tftcore::arduino::PsramAlloc g_psram;
tftcore::Client g_client(g_stream, g_udp, g_clock, g_psram);

Preferences g_prefs;
String g_zoneId, g_trackKey;
uint32_t g_lastSeekAt = 0;
int32_t g_lastSeek = -1;
int g_seekTicks = 0, g_minGap = 1 << 30, g_maxGap = 0;
long g_gapSum = 0;
bool g_sawZones = false;
#endif

void printMemory(const char *when) {
    Serial.printf("[mem] %-22s internal %6u B free (min %6u, largest block %6u), psram %u KB\n", when,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Reads one line from the serial console, echoing stars for a secret. Blocks until Enter.
// The question is asked again every few seconds for as long as nothing has been typed: a console
// opened after the board started would otherwise show an empty screen, and typing into an
// invisible prompt is how the wrong thing ends up in the wrong field.
String askLine(const char *prompt, bool secret) {
    Serial.printf("\n%s: ", prompt);
    String line;
    uint32_t askedAt = millis();
    while (true) {
        while (!Serial.available()) {
            delay(10);
            if (line.length() || (int32_t)(millis() - askedAt) < 5000) continue;
            askedAt = millis();
            Serial.printf("\n%s: ", prompt);
        }
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            Serial.println();
            line.trim();
            return line;
        }
        if ((c == 8 || c == 127) && line.length()) {   // backspace
            line.remove(line.length() - 1);
            Serial.print("\b \b");
            continue;
        }
        if (c < 32) continue;
        line += c;
        Serial.print(secret ? '*' : c);
    }
}

// Drops whatever Wi-Fi credentials this example stored (not the panel firmware's own).
void forgetWifi() {
    Preferences own;
    if (own.begin("tftcore", false)) {
        own.remove("ssid");
        own.remove("pass");
        own.end();
    }
}

// Where the Wi-Fi credentials come from, in order: the TFT Panel firmware's own NVS (a board
// that has already run it needs nothing), then ours from a previous run, then typed once on this
// console. They are never a build flag, so no password reaches the repository or the build log.
bool wifiCredentials(String &ssid, String &pass) {
    Preferences panel;
    if (panel.begin("tftpanel", true)) {
        ssid = panel.getString("ssid", "");
        pass = panel.getString("pass", "");
        panel.end();
    }
    if (ssid.length()) return true;

    Preferences own;
    if (own.begin("tftcore", true)) {
        ssid = own.getString("ssid", "");
        pass = own.getString("pass", "");
        own.end();
    }
    if (ssid.length()) return true;

    Serial.println();
    Serial.println("No Wi-Fi credentials on this board yet. Two questions, in this order.");
    ssid = askLine("1 of 2  name of the Wi-Fi network (not the password)", false);
    if (!ssid.length()) return false;
    char prompt[96];
    snprintf(prompt, sizeof prompt, "2 of 2  password for \"%s\" (hidden)", ssid.c_str());
    pass = askLine(prompt, true);
    if (own.begin("tftcore", false)) {
        own.putString("ssid", ssid);
        own.putString("pass", pass);
        own.end();
        Serial.println("Saved. The next boot joins on its own.");
    }
    return true;
}

#ifndef NO_CLIENT
void onState(void *, tftcore::State s, const char *detail) {
    Serial.printf("[state] %s%s%s\n", tftcore::stateName(s), detail && *detail ? ": " : "", detail ? detail : "");
    if (s == tftcore::State::Paired) printMemory("paired");
}

void onToken(void *, const char *serverId, const char *token) {
    g_prefs.putString("srv", serverId);
    g_prefs.putString("tok", token);
    Serial.printf("[pair] saved the pairing for %s\n", serverId);
}

void onZones(void *, const tftcore::ZoneList &zones, const tftcore::ZoneDelta &delta) {
    if (delta.full) {
        Serial.printf("[zones] %u zone(s)\n", (unsigned)zones.zones().size());
        for (const tftcore::Zone &z : zones.zones())
            Serial.printf("        %-22s %-8s %s\n", z.name.c_str(), z.state.c_str(),
                          z.nowPlaying.present ? z.nowPlaying.title.c_str() : "(nothing loaded)");
        if (!g_sawZones) {
            g_sawZones = true;
            printMemory("zone list applied");
            Serial.printf("[wire] the zone list was %u bytes\n", (unsigned)g_client.stats().subscribedLen);
        }
    }

    const tftcore::Zone *z = zones.pick(g_zoneId.c_str());
    if (!z) return;
    if (g_zoneId != z->id.c_str()) {
        g_zoneId = z->id.c_str();
        Serial.printf("[zone] following \"%s\"\n", z->name.c_str());
    }

    String key = String(z->nowPlaying.title.c_str()) + "\x1f" + z->nowPlaying.artist.c_str();
    if (z->nowPlaying.present && key != g_trackKey) {
        g_trackKey = key;
        Serial.printf("[track] %s - %s (%u s)\n", z->nowPlaying.artist.c_str(), z->nowPlaying.title.c_str(),
                      (unsigned)z->nowPlaying.lengthSec);
    }

    if (delta.seekOnly && z->nowPlaying.seekSec != g_lastSeek) {
        uint32_t now = millis();
        if (g_lastSeekAt) {
            int gap = (int)(now - g_lastSeekAt);
            g_seekTicks++;
            g_gapSum += gap;
            if (gap < g_minGap) g_minGap = gap;
            if (gap > g_maxGap) g_maxGap = gap;
        }
        g_lastSeekAt = now;
        g_lastSeek = z->nowPlaying.seekSec;
        Serial.printf("[pos] %3d s / %u s  %s\n", (int)z->nowPlaying.seekSec, (unsigned)z->nowPlaying.lengthSec,
                      z->state.c_str());
    }
}

void printStats() {
    const tftcore::Client::Stats &st = g_client.stats();
    Serial.printf("[wire] %u messages, %llu bytes, largest %u, zone list %u\n", st.messages,
                  (unsigned long long)st.totalBytes, (unsigned)st.maxLen, (unsigned)st.subscribedLen);
    if (g_seekTicks)
        Serial.printf("[wire] %d position pushes, gap min %d ms, mean %ld ms, max %d ms\n", g_seekTicks, g_minGap,
                      g_gapSum / g_seekTicks, g_maxGap);
    printMemory("now");
}

void console() {
    if (!Serial.available()) return;
    char c = (char)Serial.read();
    if (c == 'w') {
        forgetWifi();
        Serial.println("[wifi] forgotten, restarting to ask again");
        delay(500);
        ESP.restart();
    } else if (c == 's') {
        printStats();
    } else if (c == 'f') {
        g_prefs.remove("srv");
        g_prefs.remove("tok");
        g_client.forget();
        Serial.println("[pair] forgotten");
    } else if (c == 'p' || c == 'n' || c == 'b') {
        if (!g_client.paired() || !g_zoneId.length()) {
            Serial.println("[cmd] not following a zone yet");
            return;
        }
        const char *action = c == 'p' ? "playpause" : (c == 'n' ? "next" : "previous");
        tftcore::Client::Reply r = g_client.control(g_zoneId.c_str(), action);
        Serial.printf("[cmd] %s: %s\n", action, r.ok ? "ok" : (*r.name ? r.name : r.error));
    }
}
#endif  // NO_CLIENT

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("tft-core-client example: zones");
    printMemory("boot");

    // A scan first: it needs no credentials and proves the radio answers. On an ESP32-P4 the
    // radio is a separate ESP32-C6 over SDIO, so a scan that finds nothing means the link to it
    // is wrong (pins or firmware) and no password would have helped.
    WiFi.mode(WIFI_STA);
    Serial.println("Scanning for networks (the radio has to answer for anything else to work)...");
    int found = WiFi.scanNetworks();
    if (found <= 0) {
        Serial.printf("The radio found no network (result %d). On a P4 that usually means the "
                      "link to the Wi-Fi co-processor is not working on this board.\n", found);
    } else {
        int strongest = -200;
        for (int i = 0; i < found; i++)
            if (WiFi.RSSI(i) > strongest) strongest = WiFi.RSSI(i);
        // The names are left out on purpose: the count is what proves the radio works.
        Serial.printf("The radio works: %d network(s) heard, strongest %d dBm.\n", found, strongest);
    }
    WiFi.scanDelete();

    String ssid, pass;
    if (!wifiCredentials(ssid, pass)) {
        Serial.println("No Wi-Fi name given; nothing to join. Reset to try again.");
        return;
    }
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Joining \"%s\"", ssid.c_str());
    uint32_t until = millis() + 30000;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - until) < 0) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        // The name is deliberately not printed back: if the two answers were given the wrong way
        // round, printing it would put a password in a log that gets read and pasted around.
        Serial.println("Could not join that network. Forgetting it so the next boot asks again.");
        forgetWifi();
        delay(2000);
        ESP.restart();
    }
    Serial.printf("Wi-Fi up, ip %s, rssi %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    printMemory("wifi up");

#ifndef NO_CLIENT
    g_prefs.begin("tftcore", false);
    String serverId = g_prefs.getString("srv", "");
    String token = g_prefs.getString("tok", "");
    if (token.length()) {
        g_client.setToken(serverId.c_str(), token.c_str());
        Serial.printf("[pair] reusing the pairing saved for %s\n", serverId.c_str());
    }

    static tftcore::ExtensionInfo info;
    info.extensionId = "com.textfromtrack.panel-poc";
    static String name = String("Zones example ") + String((uint32_t)ESP.getEfuseMac(), HEX);
    info.displayName = name.c_str();
    info.displayVersion = "0.1.0";
    info.publisher = "Vincent Cruvellier";
    info.email = "none@example.com";

    g_client.onState(onState, nullptr);
    g_client.onToken(onToken, nullptr);
    g_client.onZones(onZones, nullptr);
    g_client.begin(info);
    Serial.println("Console: p play/pause, n next, b previous, s stats, f forget pairing");
#else
    // The baseline links what the host firmware already has (ArduinoJson, WiFiUDP) but not the
    // client, so the difference between the two images is the library's own code and nothing else.
    {
        JsonDocument doc;
        deserializeJson(doc, "{\"zones\":[{\"zone_id\":\"z\",\"state\":\"playing\"}]}");
        JsonVariantConst z = doc["zones"][0];
        Serial.printf("Baseline build (no client). json: %s %s\n", (const char *)(z["zone_id"] | "?"),
                      (const char *)(z["state"] | "?"));
        char out[64];
        serializeJson(doc, out, sizeof out);
        Serial.println(out);
        WiFiUDP udp;
        udp.begin(0);
        udp.beginPacket(WiFi.broadcastIP(), 9003);
        udp.write((const uint8_t *)"SOOD", 4);
        Serial.printf("udp: %d\n", udp.endPacket());
        udp.stop();
    }
#endif
}

void loop() {
#ifndef NO_CLIENT
    g_client.tick();
    console();
#endif
    delay(5);
}
