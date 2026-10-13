// SPDX-License-Identifier: GPL-3.0-or-later
// guest_stick=X:Z[:R[:STEPS]] (party_director test step): a real left-stick walk of the guest to
// (X, Z) within R m (1.0), giving up after STEPS stick steps (40). Header-only for the unit test.
#pragma once

#include <cstdio>
#include <string>

inline bool ParseStickArgs(const std::string& args, float* x, float* z, float* r, int* steps) {
    float px = 0, pz = 0, pr = 1.0f;
    int ps = 40;
    char tail = 0;
    const int n = std::sscanf(args.c_str(), "%f:%f:%f:%d%c", &px, &pz, &pr, &ps, &tail);
    if (n < 2 || n > 4 || pr <= 0.0f || ps < 1 || ps > 2000) {
        return false;
    }
    *x = px;
    *z = pz;
    *r = pr;
    *steps = ps;
    return true;
}
