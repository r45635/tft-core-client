#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "tftcore/sood.h"

using namespace tftcore;

namespace {

// Builds a reply packet the way a server does, so the parser is tested against the layout
// and not against our own writer.
std::vector<uint8_t> reply(const std::vector<std::pair<std::string, std::string>> &props, char type = 'R',
                           uint8_t version = 0x02) {
    std::vector<uint8_t> p = {'S', 'O', 'O', 'D', version, (uint8_t)type};
    for (auto &kv : props) {
        p.push_back((uint8_t)kv.first.size());
        p.insert(p.end(), kv.first.begin(), kv.first.end());
        p.push_back((uint8_t)(kv.second.size() >> 8));
        p.push_back((uint8_t)(kv.second.size() & 0xff));
        p.insert(p.end(), kv.second.begin(), kv.second.end());
    }
    return p;
}

}  // namespace

void testSood() {
    // The query carries the service id and the echo id, in the documented layout.
    {
        Buffer q;
        CHECK(soodBuildQuery(q, "abc123"));
        CHECK(q.size() > 6);
        CHECK(memcmp(q.data(), "SOOD", 4) == 0);
        CHECK_INT(q.data()[4], 0x02);
        CHECK_INT(q.data()[5], 'Q');
        size_t p = 6;
        uint8_t nameLen = q.data()[p++];
        CHECK_STR(std::string((const char *)q.data() + p, nameLen).c_str(), "query_service_id");
        p += nameLen;
        size_t valueLen = ((size_t)q.data()[p] << 8) | q.data()[p + 1];
        p += 2;
        CHECK_STR(std::string((const char *)q.data() + p, valueLen).c_str(), SOOD_SERVICE_ID);
        p += valueLen;
        nameLen = q.data()[p++];
        CHECK_STR(std::string((const char *)q.data() + p, nameLen).c_str(), "_tid");
    }
    // A good reply.
    {
        auto p = reply({{"service_id", SOOD_SERVICE_ID},
                        {"unique_id", "e4b5f0c1"},
                        {"http_port", "9330"},
                        {"name", "Mac Studio"},
                        {"display_version", "2.73"}});
        SoodReply r;
        CHECK(soodParseReply(p.data(), p.size(), r));
        CHECK_STR(r.uniqueId, "e4b5f0c1");
        CHECK_INT(r.httpPort, 9330);
        CHECK_STR(r.name, "Mac Studio");
        CHECK_STR(r.displayVersion, "2.73");
    }
    // A null property (0xFFFF) is skipped, not read as a value.
    {
        std::vector<uint8_t> p = {'S', 'O', 'O', 'D', 0x02, 'R'};
        const char *n = "name";
        p.push_back((uint8_t)strlen(n));
        p.insert(p.end(), n, n + strlen(n));
        p.push_back(0xff);
        p.push_back(0xff);
        auto rest = reply({{"service_id", SOOD_SERVICE_ID}, {"unique_id", "x"}, {"http_port", "9330"}});
        p.insert(p.end(), rest.begin() + 6, rest.end());
        SoodReply r;
        CHECK(soodParseReply(p.data(), p.size(), r));
        CHECK_STR(r.name, "");
        CHECK_INT(r.httpPort, 9330);
    }
    // Rejected: another service, a query, the wrong version, no port, a truncated property.
    {
        SoodReply r;
        auto other = reply({{"service_id", "11111111-2222-3333-4444-555555555555"}, {"unique_id", "x"}, {"http_port", "9330"}});
        CHECK(!soodParseReply(other.data(), other.size(), r));
        auto query = reply({{"service_id", SOOD_SERVICE_ID}, {"unique_id", "x"}, {"http_port", "9330"}}, 'Q');
        CHECK(!soodParseReply(query.data(), query.size(), r));
        auto v3 = reply({{"service_id", SOOD_SERVICE_ID}, {"unique_id", "x"}, {"http_port", "9330"}}, 'R', 0x03);
        CHECK(!soodParseReply(v3.data(), v3.size(), r));
        auto noPort = reply({{"service_id", SOOD_SERVICE_ID}, {"unique_id", "x"}});
        CHECK(!soodParseReply(noPort.data(), noPort.size(), r));
        auto good = reply({{"service_id", SOOD_SERVICE_ID}, {"unique_id", "x"}, {"http_port", "9330"}});
        CHECK(!soodParseReply(good.data(), good.size() - 2, r));
        CHECK(!soodParseReply((const uint8_t *)"SO", 2, r));
    }
    // Choosing between the addresses one server answers from.
    {
        const char *me = "192.168.2.50";
        CHECK_INT(soodAddressScore("192.168.2.9", me), 5);     // our own /24
        CHECK_INT(soodAddressScore("192.168.7.9", me), 4);     // our /16
        CHECK_INT(soodAddressScore("10.0.0.9", me), 3);        // somewhere else, still routable
        CHECK_INT(soodAddressScore("127.0.0.1", me), 2);
        CHECK_INT(soodAddressScore("169.254.205.66", me), 1);  // the trap: reachable from nowhere
        CHECK_INT(soodAddressScore("not an ip", me), 0);
        CHECK_INT(soodAddressScore("1.2.3.999", me), 0);
        // With no address of our own, anything routable still beats link-local and loopback.
        CHECK(soodAddressScore("10.0.0.9", "") > soodAddressScore("127.0.0.1", ""));
        CHECK(soodAddressScore("127.0.0.1", "") > soodAddressScore("169.254.1.1", ""));
    }
}
