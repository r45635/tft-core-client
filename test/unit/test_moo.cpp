#include <string>

#include "check.h"
#include "tftcore/moo.h"

using namespace tftcore;

static std::string frame(const std::string &s) { return s; }

void testMoo() {
    // A reply with a JSON body.
    {
        std::string f = frame("MOO/1 COMPLETE Success\nRequest-Id: 7\nContent-Length: 13\n"
                              "Content-Type: application/json\n\n{\"zones\":[1]}");
        MooHeader h;
        CHECK(mooParse((const uint8_t *)f.data(), f.size(), h));
        CHECK(h.verb == MooVerb::Complete);
        CHECK_STR(h.name, "Success");
        CHECK_INT(h.requestId, 7);
        CHECK_INT(h.contentLength, 13);
        CHECK(h.json);
        CHECK_STR(std::string(f.data() + h.bodyOffset, h.contentLength).c_str(), "{\"zones\":[1]}");
    }
    // A subscription update.
    {
        std::string f = frame("MOO/1 CONTINUE Changed\nRequest-Id: 3\nContent-Length: 2\n"
                              "Content-Type: application/json\n\n{}");
        MooHeader h;
        CHECK(mooParse((const uint8_t *)f.data(), f.size(), h));
        CHECK(h.verb == MooVerb::Continue);
        CHECK(h.is("Changed"));
        CHECK_INT(h.requestId, 3);
    }
    // A request from the server: the name splits into service and method.
    {
        std::string f = frame("MOO/1 REQUEST com.roonlabs.ping:1/ping\nRequest-Id: 11\n\n");
        MooHeader h;
        CHECK(mooParse((const uint8_t *)f.data(), f.size(), h));
        CHECK(h.verb == MooVerb::Request);
        CHECK_STR(h.service, "com.roonlabs.ping:1");
        CHECK_STR(h.name, "ping");
        CHECK_INT(h.requestId, 11);
        CHECK_INT(h.contentLength, 0);
    }
    // A binary body is not JSON.
    {
        std::string f = frame("MOO/1 COMPLETE Success\nRequest-Id: 1\nContent-Length: 3\n"
                              "Content-Type: image/jpeg\n\nabc");
        MooHeader h;
        CHECK(mooParse((const uint8_t *)f.data(), f.size(), h));
        CHECK(!h.json);
        CHECK_INT(h.contentLength, 3);
    }
    // Malformed: no Request-Id, length without type, body without length, wrong magic.
    {
        MooHeader h;
        std::string a = frame("MOO/1 COMPLETE Success\n\n");
        CHECK(!mooParse((const uint8_t *)a.data(), a.size(), h));
        std::string b = frame("MOO/1 COMPLETE Success\nRequest-Id: 1\nContent-Length: 2\n\n{}");
        CHECK(!mooParse((const uint8_t *)b.data(), b.size(), h));
        std::string c = frame("MOO/1 COMPLETE Success\nRequest-Id: 1\n\n{}");
        CHECK(!mooParse((const uint8_t *)c.data(), c.size(), h));
        std::string d = frame("HTTP/1.1 200 OK\nRequest-Id: 1\n\n");
        CHECK(!mooParse((const uint8_t *)d.data(), d.size(), h));
    }
    // Build, then parse back.
    {
        Buffer out;
        CHECK(mooBuildRequest(out, "com.roonlabs.transport:2/control", 42, "{\"control\":\"playpause\"}"));
        MooHeader h;
        CHECK(mooParse(out.data(), out.size(), h));
        CHECK(h.verb == MooVerb::Request);
        CHECK_STR(h.service, "com.roonlabs.transport:2");
        CHECK_STR(h.name, "control");
        CHECK_INT(h.requestId, 42);
        CHECK_INT(h.contentLength, 23);
        CHECK(h.json);
        CHECK_STR(std::string((const char *)out.data() + h.bodyOffset, h.contentLength).c_str(),
                  "{\"control\":\"playpause\"}");
    }
    // A reply with no body at all.
    {
        Buffer out;
        CHECK(mooBuildComplete(out, "Success", 5, nullptr));
        MooHeader h;
        CHECK(mooParse(out.data(), out.size(), h));
        CHECK(h.verb == MooVerb::Complete);
        CHECK_INT(h.contentLength, 0);
        CHECK(!h.json);
    }
}
