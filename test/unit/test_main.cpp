#include "check.h"

int main() {
    struct { const char *name; void (*run)(); } suites[] = {
        {"moo framing", testMoo},
        {"sood discovery", testSood},
        {"websocket", testWs},
        {"zone merge", testZones},
    };
    for (auto &s : suites) {
        int before = check::failures;
        printf("%s\n", s.name);
        s.run();
        if (check::failures == before) printf("  ok\n");
    }
    printf("\n%d checks, %d failed\n", check::checks, check::failures);
    return check::failures ? 1 : 0;
}
