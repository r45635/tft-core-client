// MOO message framing (docs/PROTOCOL.md section 3).
#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer.h"

namespace tftcore {

enum class MooVerb : uint8_t { Unknown, Request, Complete, Continue };

struct MooHeader {
    MooVerb verb = MooVerb::Unknown;
    // REQUEST: the name is split, "com.roonlabs.ping:1" + "ping".
    // COMPLETE / CONTINUE: `name` is the status ("Success", "Registered", "Changed"...)
    // and `service` is empty.
    char service[56] = {0};
    char name[72] = {0};
    int requestId = -1;
    char contentType[48] = {0};
    size_t contentLength = 0;
    size_t bodyOffset = 0;   // first body byte in the frame
    bool json = false;       // content type is application/json

    bool is(const char *n) const;
};

// Parses one frame's header block. false = malformed (and nothing may be trusted).
bool mooParse(const uint8_t *frame, size_t len, MooHeader &h);

// `jsonBody` may be null (no body at all). Replaces whatever `out` held.
bool mooBuildRequest(Buffer &out, const char *name, int requestId, const char *jsonBody);
bool mooBuildComplete(Buffer &out, const char *name, int requestId, const char *jsonBody);

}  // namespace tftcore
