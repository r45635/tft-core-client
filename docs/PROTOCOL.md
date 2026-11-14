# The extension protocol, as observed

Everything this library implements, written down so the code can be read, tested and fixed
without a running Core in front of you. Verified 2026-11-14 against Roon 2.x (Core 2.73) and
against the behaviour of Roon Labs' JavaScript client.

## 1. Discovery: SOOD over UDP

- Port **9003**, multicast group **239.255.90.90**, and the broadcast address of each
  interface. A query goes to both; replies come back to the sending socket.
- Packet layout:

```
offset  content
0..3    "SOOD"            ASCII magic
4       0x02              protocol version
5       'Q' or 'R'        query / reply
6..     properties, repeated to the end of the packet
```

- One property: `1 byte name length`, `name`, `2 bytes value length (big endian)`, `value`.
  A value length of `0xFFFF` means null, `0x0000` means an empty string.
- The query carries `query_service_id` = `00720724-5143-4a9b-abac-0e50cba674bb` and a `_tid`
  the replies echo back.
- A reply carries at least `service_id` (equal to the queried id), `unique_id` (the Core's id)
  and `http_port`. On Roon 2.x the extension port is **9330**, not 9100. Other properties seen:
  `name`, `display_version`, `tid`.
- A client re-queries about every 10 s while it has no Core.

## 2. Transport: a WebSocket

- `ws://<core ip>:<http_port>/api`, plain HTTP upgrade, no TLS.
- Messages are sent in **binary frames, masked** (as RFC 6455 requires of a client).
- Keepalive: a WebSocket ping every 10 s; no pong before the next one means the connection is
  dead and must be closed.

## 3. Message framing: MOO

One MOO message per WebSocket frame. Text header lines, `\n` separated, then a blank line,
then the optional body:

```
MOO/1 REQUEST com.roonlabs.transport:2/control
Request-Id: 7
Content-Length: 58
Content-Type: application/json

{"zone_or_output_id":"16016...","control":"playpause"}
```

- The first line is `MOO/<version> <VERB> <name>`. Verbs: `REQUEST` (either direction),
  `COMPLETE` and `CONTINUE` (replies).
- `Request-Id` is mandatory and ties a reply to its request. The client counts from 0.
- `Content-Length` and `Content-Type` appear together or not at all. `application/json` means
  a JSON body; anything else is binary.
- `COMPLETE` ends a request. `CONTINUE` is an intermediate reply: a subscription's updates all
  arrive as `CONTINUE` on the request id of the subscribe call, and the request stays open.
- The reply's `name` is a status, not a path: `Success`, `Registered`, `Subscribed`,
  `Changed`, `InvalidRequest`, `NotRegistered`...
- The Core sends requests too. `com.roonlabs.ping:1/ping` must be answered
  `MOO/1 COMPLETE Success` on the same request id. An unknown service or method is answered
  `COMPLETE InvalidRequest`.

## 4. Registry: pairing

1. `com.roonlabs.registry:1/info`, no body. The reply body carries the Core's `core_id`,
   `display_name` and `display_version`. A client that already holds a token for that
   `core_id` sends it in step 2.
2. `com.roonlabs.registry:1/register` with:

```json
{ "extension_id": "com.example.thing", "display_name": "Thing", "display_version": "1.0.0",
  "publisher": "Someone", "email": "someone@example.com", "website": "https://example.com",
  "required_services": ["com.roonlabs.transport:2", "com.roonlabs.browse:1"],
  "optional_services": [], "provided_services": [], "token": "<saved token, if any>" }
```

3. The reply is `Registered` with `{core_id, display_name, display_version, token}`. **With no
   valid token the request stays unanswered until a person enables the extension** in
   Roon > Settings > Extensions; the reply then arrives on the same request id. So a client
   shows "enable me" while the register request is pending, and does not retry it.
4. The token must be stored per `core_id` and sent on every later registration, or the user has
   to authorize the extension again after each restart.
5. Pairing (`com.roonlabs.pairing:1`) is a separate, optional service, for extensions that want
   the user to choose one Core among several. A client that follows a zone itself does not
   provide it.

## 5. Transport service `com.roonlabs.transport:2`

- `subscribe_zones` with `{subscription_key: "<any string>"}`.
  - First reply, `Subscribed`: `{zones: [Zone, ...]}` — every zone in full.
  - Then `Changed` replies: `{zones_added: [Zone], zones_changed: [Zone], zones_removed: [id],
    zones_seek_changed: [{zone_id, seek_position, queue_time_remaining}]}`. Each key is
    optional. `zones_seek_changed` arrives about once a second for a playing zone and is the
    only message during steady playback.
- A Zone:

```json
{ "zone_id": "...", "display_name": "Living room", "state": "playing|paused|loading|stopped",
  "is_play_allowed": true, "is_pause_allowed": true, "is_next_allowed": true,
  "is_previous_allowed": true, "is_seek_allowed": true,
  "queue_items_remaining": 12, "queue_time_remaining": 2480,
  "now_playing": { "seek_position": 42, "length": 215, "image_key": "...",
                   "one_line": {"line1": "..."},
                   "two_line": {"line1": "title", "line2": "artist"},
                   "three_line": {"line1": "title", "line2": "artist", "line3": "album"} },
  "outputs": [ { "output_id": "...", "display_name": "...", "zone_id": "...",
                 "volume": { "type": "number|db|incremental", "min": -80, "max": 0,
                             "value": -34, "step": 1, "is_muted": false } } ] }
```

  `now_playing` is absent when nothing is loaded. `volume` is absent when the output has none.
  `seek_position` and `length` are whole seconds. An `incremental` volume carries only its
  type: the control is +/- buttons, with no value to show.
- `control` `{zone_or_output_id, control}` where control is
  `play | pause | playpause | stop | previous | next`.
- `change_volume` `{output_id, how, value}`, how is `absolute | relative | relative_step`.
  An `incremental` output takes `relative_step` with +1 / -1 and nothing else.
- `mute` `{output_id, how}`, how is `mute | unmute`.
- `seek` `{zone_or_output_id, how, seconds}`, how is `relative | absolute`.
- Replies are `Success` or an error name; a `control` on an idle zone fails.

## 6. Browse service `com.roonlabs.browse:1`

- `browse` `{hierarchy, item_key?, input?, pop_all?, pop_levels?, refresh_list?,
  set_display_offset?, zone_or_output_id?, multi_session_key?}`.
  Hierarchies: `browse`, `search`, `albums`, `artists`, `playlists`, `settings`...
  The reply body is `{action: "list|message|none|replace_item|remove_item", list?, item?,
  message?, is_error?}` and `list` is `{title, count, level, subtitle?, image_key?, hint?,
  display_offset?}`.
- `load` `{hierarchy, level?, offset, count, set_display_offset?, multi_session_key?}` →
  `{items: [Item], offset, list}`. An Item is `{title, subtitle?, image_key?, item_key?,
  hint?, input_prompt?}`, hint being `null | action | action_list | list | header`.
- `multi_session_key` gives a client its own browse stack: without it, every client shares
  one cursor and they move each other's position.
- `item_key` and `pop_all` must not be sent together. An expired `item_key` fails and the
  hierarchy has to be reopened.

## 7. Images: plain HTTP, no MOO

```
GET http://<core ip>:<http_port>/api/image/<image_key>?scale=fit&width=384&height=384&format=image/jpeg
```

The Core scales and encodes, so a client never resizes a full-size cover itself. There is also
a MOO `com.roonlabs.image:1/get_image` carrying the bytes in a binary body; the HTTP form is
simpler and is what this library documents for clients.
