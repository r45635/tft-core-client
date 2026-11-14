# tft-core-client

A C++ client for the extension protocol of Roon Labs' music software, for **ESP32 boards and
host builds**. It discovers a Core on the local network, pairs with it, follows the zones as the
Core pushes them, and drives playback, volume and library browsing.

As far as I could find, no such client existed for C or C++: the official one is JavaScript, and
the community ports are Python and Go. This one was written for the
[TFT Panel](https://github.com/r45635/tft-panel) lyrics screens, so it is shaped by what a
microcontroller can afford: no exceptions, no hidden allocations, every buffer in the memory the
caller chooses, and a host build so the protocol can be tested on a PC against a real Core.

"Roon" is a trademark of Roon Labs LLC; this library is not affiliated with or endorsed by them.
The protocol has no public specification: what this library implements is written down in
[docs/PROTOCOL.md](docs/PROTOCOL.md). See [NOTICE](NOTICE) for how that was learned.

## What it does

| | |
|---|---|
| Discovery | SOOD over UDP 9003, multicast **and** per-interface broadcast, with the answers ranked so a server's unreachable interface is never chosen |
| Transport | a small RFC 6455 WebSocket client (binary masked frames, fragment reassembly, ping/pong keepalive) |
| Framing | MOO messages: requests, replies, and subscription updates on an open request |
| Pairing | registry info/register, with the token kept by the caller; tells you when a person must enable the extension |
| Zones | one subscription, merged into a zone list: tracks, state, outputs, volume, and the position pushed once a second |
| Controls | play/pause/next/previous, volume (absolute, relative, step), mute, seek |
| Library | browse and load, passed through so the caller owns the hierarchy and its own browse session |
| Cover art | the URL on the Core's own HTTP port; fetch it with whatever HTTP client you already have |

Not implemented: the pairing service (a client that follows a zone itself does not need it), the
MOO image service (the HTTP form is simpler), queue subscriptions, and settings pages.

## Using it

```cpp
#include "arduino_io.h"          // ports/arduino, header-only
#include "tftcore/client.h"

tftcore::arduino::TcpStream stream;
tftcore::arduino::Udp udp;
tftcore::arduino::MillisClock clock;
tftcore::arduino::PsramAlloc psram;      // keeps every buffer out of internal RAM
tftcore::Client client(stream, udp, clock, psram);

void setup() {
    static tftcore::ExtensionInfo info;
    info.extensionId = "com.example.thing";    // stable per product
    info.displayName = "Thing";                // what the user enables in the extension list
    info.displayVersion = "1.0.0";
    info.publisher = "You";
    info.email = "you@example.com";

    client.setToken(savedServerId, savedToken);   // from NVS, if you have one
    client.onToken(persist, nullptr);             // save it or the user authorizes again every boot
    client.onZones(zonesChanged, nullptr);
    client.begin(info);
}

void loop() { client.tick(); }                    // drives everything
```

`tick()` discovers, connects, pairs, subscribes and reconnects. It blocks only during the
WebSocket handshake and the 1.5 s discovery window. Requests (`control`, `changeVolume`,
`browse`...) are synchronous: they pump the socket until the reply arrives, so a zone update can
still be delivered while one is in flight. **A callback must not call back into the Client.**

Ports: `ports/arduino` (ESP32, header-only) and `ports/posix` (host). A new platform means three
small interfaces: `ByteStream`, `UdpSocket` and `Clock` in `include/tftcore/io.h`.

## Building

```bash
make test          # unit tests on the host (159 checks)
make zones-host    # the command-line example
./build/zones-host --seconds 60            # discovers, pairs, prints the zones and the position
```

The host build needs only a C++17 compiler; the Makefile fetches ArduinoJson (the same 7.4.2 the
firmware pins) as a single header. On ESP32, add the library to `lib_deps` and build with
PlatformIO; `examples/zones_esp32` builds for an ESP32-S3 and an ESP32-P4.

## Measurements

Flash, from `examples/zones_esp32` built with and without the client, Arduino-ESP32 3.3.12 /
IDF 5.5, `-Os`:

| Board | Library's own code |
|---|---|
| ESP32-S3 (CrowPanel 7", 4 MB flash) | 45.6 KiB |
| ESP32-P4 (Waveshare 7B) | 56.3 KiB |

Two choices account for about 18 KiB of that on the S3: no `sscanf` (an IPv4 parser written out)
and no float conversion in `snprintf` (volumes formatted by hand). Keep them if flash is tight.

On the wire, against a Roon 2.73 Core: a registry reply is ~200 bytes. The full zone list and the
per-second position pushes are measured by the examples (`s` on the console) and will be filled
in here once the on-board run is done.

## Licence

MIT, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
