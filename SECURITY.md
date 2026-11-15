# Security

## Reporting something

Please report a vulnerability privately, through GitHub's
[security advisories](https://github.com/r45635/tft-core-client/security/advisories/new), rather
than in a public issue. I answer as fast as a side project allows, and I will credit you in the
advisory unless you would rather I did not.

## What this library assumes

It speaks an **unauthenticated protocol on a local network**: a music server answers a UDP
discovery query, then a WebSocket carries JSON messages both ways. There is no transport security
in the protocol itself, and this library adds none.

What that means for a program using it:

- **Anything on the local network can impersonate the server.** Pairing returns a token that proves
  *you* to the server, not the server to you. Treat what arrives as untrusted input.
- **Everything the server sends is parsed into caller-owned memory** (`Alloc`), with lengths taken
  from the message and checked against the buffer. The JSON is parsed by ArduinoJson with a filter,
  so a large or unexpected message costs memory, not control. A hostile server can still make you
  allocate: size your allocator, and on a microcontroller keep it in PSRAM.
- **The pairing token is a credential.** Store it where the platform keeps secrets (NVS on ESP32),
  never in source, a build flag or a log. `onToken` hands it to you precisely so that you choose.
- **Discovery answers are ranked, never trusted blindly**: a link-local address a server announces
  is refused, and an address on the querier's own subnet is preferred.
- The host build in `ports/posix` is for tests and development, not for a service exposed to a
  network you do not control.
