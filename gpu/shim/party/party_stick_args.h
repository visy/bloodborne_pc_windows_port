// SPDX-License-Identifier: GPL-3.0-or-later
// guest_stick=X:Z[:R[:STEPS]] (party_director test step): a real left-stick walk of the guest to
// (X, Z) within R m (1.0), giving up after STEPS stick steps (40); or guest_stick=obj:ENTITY:DIST:ANGLE
// [:R[:STEPS]] to the point DIST m from a map object. Header-only for the unit test.
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

inline bool ParseStickObjArgs(const std::string& args, int* entity, float* dist, float* angle, float* r, int* steps) {
    if (args.rfind("obj:", 0) != 0) {
        return false;
    }
    int e = 0, ps = 40;
    float d = 0, a = 0, pr = 1.0f;
    char tail = 0;
    const int n = std::sscanf(args.c_str() + 4, "%d:%f:%f:%f:%d%c", &e, &d, &a, &pr, &ps, &tail);
    if (n < 3 || n > 5 || e <= 0 || pr <= 0.0f || ps < 1 || ps > 2000) {
        return false;
    }
    *entity = e;
    *dist = d;
    *angle = a;
    *r = pr;
    *steps = ps;
    return true;
}
