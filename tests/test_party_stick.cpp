// SPDX-License-Identifier: GPL-3.0-or-later
// guest_stick=X:Z[:R[:STEPS]] argument parsing (gpu/shim/party/party_stick_args.h).
// Build and run: ninja -C out/gpu party-stick-test && out/gpu/party-stick-test.exe
#include "party/party_stick_args.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

int main() {
    float x = 0, z = 0, r = 0;
    int steps = 0;
    CHECK(ParseStickArgs("-66.5:122", &x, &z, &r, &steps));
    CHECK(x == -66.5f && z == 122.0f && r == 1.0f && steps == 40);
    CHECK(ParseStickArgs("10:20:2.5", &x, &z, &r, &steps));
    CHECK(r == 2.5f && steps == 40);
    CHECK(ParseStickArgs("10:20:2:300", &x, &z, &r, &steps));
    CHECK(steps == 300);
    CHECK(!ParseStickArgs("", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:0", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:0", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:5000", &x, &z, &r, &steps));
    CHECK(!ParseStickArgs("10:20:1:5x", &x, &z, &r, &steps));
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-stick-test: ok\n");
    return 0;
}
