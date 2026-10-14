// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the CAN identifier conversions used by the SavvyCAN
// CAN Calculator window (gui/savvycan/re/canidcalc.h).
#include <cstdio>
#include "canidcalc.h"

using namespace canidcalc;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main()
{
    /* 29-bit 0x1ABCDEF0, worked out by hand from the data sheet layouts */
    const uint32_t id = 0x1ABCDEF0;
    CHECK(baseOf(id) == 0x6AF);
    CHECK(extOf(id) == 0x0DEF0);
    CHECK(join(0x6AF, 0x0DEF0) == id);

    Mcp m = toMcp(id, true);
    CHECK(m.sidh == 0xD5 && m.sidl == 0xE8 && m.eid8 == 0xDE && m.eid0 == 0xF0);
    bool ext = false;
    CHECK(fromMcp(m, ext) == id && ext);

    Sja s = toSja(id, true, false);
    CHECK(s.count == 4 && s.id[0] == 0xD5 && s.id[1] == 0xE6 && s.id[2] == 0xF7 && s.id[3] == 0x80);
    bool rtr = true;
    CHECK(fromSja(s, true, rtr) == id && !rtr);
    s = toSja(id, true, true);
    CHECK(s.id[3] == 0x84 && fromSja(s, true, rtr) == id && rtr);

    /* standard 0x123 */
    m = toMcp(0x123, false);
    CHECK(m.sidh == 0x24 && m.sidl == 0x60 && m.eid8 == 0 && m.eid0 == 0);
    CHECK(fromMcp(m, ext) == 0x123 && !ext);
    s = toSja(0x123, false, true);
    CHECK(s.count == 2 && s.id[0] == 0x24 && s.id[1] == 0x70);
    CHECK(fromSja(s, false, rtr) == 0x123 && rtr);

    /* extremes, round trips over a sweep */
    m = toMcp(0x1FFFFFFF, true);
    CHECK(m.sidh == 0xFF && m.sidl == 0xEB && m.eid8 == 0xFF && m.eid0 == 0xFF);
    for (uint32_t v = 0; v < 0x20000000u; v += 0x00123457u)
    {
        CHECK(fromMcp(toMcp(v, true), ext) == v);
        CHECK(fromSja(toSja(v, true, false), true, rtr) == v);
    }
    for (uint32_t v = 0; v < 0x800; v++)
    {
        CHECK(fromMcp(toMcp(v, false), ext) == v);
        CHECK(fromSja(toSja(v, false, false), false, rtr) == v);
    }

    std::printf("%s (%d failures)\n", fails ? "CANIDCALC TEST FAILED" : "CANIDCALC TEST PASSED", fails);
    return fails ? 1 : 0;
}
