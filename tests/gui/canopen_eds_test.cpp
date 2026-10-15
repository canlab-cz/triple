// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the EDS/DCF reader and PDO decoder (gui/savvycan/re/canopen_eds.h)
// against the simulated node's EDS (tools/canopen_sim.eds) and frames
// captured from the simulator.
#include <cstdio>
#include <fstream>
#include <sstream>
#include "canopen_eds.h"

using namespace canopen;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "../../tools/canopen_sim.eds";
    std::ifstream f(path);
    if (!f)
    {
        std::printf("cannot open %s\n", path);
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    Eds eds;
    eds.parse(ss.str());

    CHECK(eds.productName == "Triple CANopen test node");
    const Object *o = eds.find(0x1008, 0);
    CHECK(o && o->name == "Manufacturer device name" && o->dataType == VISIBLE_STRING);
    CHECK(eds.find(0x1018, 0) && eds.find(0x1018, 4));
    uint64_t v;
    CHECK(eds.value(0x1018, 4, 5, v) && v == 2604002);
    CHECK(eds.value(0x1800, 1, 5, v) && v == 0x185);             /* $NODEID+0x180 */
    CHECK(eds.value(0x1400, 1, 0x7F, v) && v == 0x27F);
    CHECK(evalValue("0x200 + $NODEID", 3, v) && v == 0x203);
    CHECK(evalValue("$nodeid+0x80", 1, v) && v == 0x81);
    CHECK(!evalValue("abc", 1, v));
    CHECK(eds.name(0x6401, 1) == "Analogue input 1");
    CHECK(eds.name(0x1234, 5) == "1234:05");

    std::vector<Pdo> pdos = buildPdos(eds, 5);
    CHECK(pdos.size() == 3);                                       /* TPDO1, TPDO2, RPDO1 */
    const Pdo *t1 = nullptr, *t2 = nullptr, *r1 = nullptr;
    for (const Pdo &p : pdos)
    {
        if (p.tx && p.number == 1) t1 = &p;
        if (p.tx && p.number == 2) t2 = &p;
        if (!p.tx && p.number == 1) r1 = &p;
    }
    CHECK(t1 && t1->cobId == 0x185 && t1->valid && t1->entries.size() == 2);
    CHECK(t2 && t2->cobId == 0x285 && t2->entries.size() == 2);
    CHECK(r1 && r1->cobId == 0x205 && r1->entries.size() == 1);
    if (t1 && t1->entries.size() == 2)
    {
        CHECK(t1->entries[0].index == 0x6000 && t1->entries[0].sub == 1 && t1->entries[0].bits == 8 && t1->entries[0].bitOffset == 0);
        CHECK(t1->entries[1].index == 0x6401 && t1->entries[1].bits == 16 && t1->entries[1].bitOffset == 8);
        CHECK(t1->entries[1].dataType == INTEGER16 && t1->entries[1].name == "Analogue input 1");
        /* frame captured from the simulator: 185 [3] 02 3A 01 */
        const uint8_t d[] = { 0x02, 0x3A, 0x01 };
        CHECK(formatEntry(t1->entries[0], d, 3) == "2 (0x02)");
        CHECK(formatEntry(t1->entries[1], d, 3) == "314");
        CHECK(formatEntry(t1->entries[1], d, 2) == "-");            /* frame too short */
    }
    if (t2 && t2->entries.size() == 2)
    {
        /* 285 [6] B8 FF 01 00 00 00: analogue input 2 = -72, SYNC counter = 1 */
        const uint8_t d[] = { 0xB8, 0xFF, 0x01, 0x00, 0x00, 0x00 };
        CHECK(formatEntry(t2->entries[0], d, 6) == "-72");
        CHECK(formatEntry(t2->entries[1], d, 6) == "1 (0x00000001)");
        CHECK(t2->entries[1].name == "SYNC counter");
    }

    /* mapping read from a node (no EDS names) and odd bit layouts */
    std::vector<PdoEntry> m = mapEntries({ 0x20000001, 0x20010003, 0x2002000C }, nullptr);
    CHECK(m.size() == 3 && m[1].bitOffset == 1 && m[2].bitOffset == 4 && m[2].name == "2002:00");
    const uint8_t b[] = { 0xAB, 0xCD };                             /* 1010 1011 1100 1101 */
    uint64_t raw;
    CHECK(extract(m[0], b, 2, raw) && raw == 1);
    CHECK(extract(m[1], b, 2, raw) && raw == 5);                   /* bits 1..3 of 0xAB */
    CHECK(extract(m[2], b, 2, raw) && raw == 0xCDA);               /* bits 4..15 */

    PdoEntry real;
    real.bits = 32;
    real.dataType = REAL32;
    const uint8_t pi[] = { 0xDB, 0x0F, 0x49, 0x40 };                /* 3.14159274f */
    CHECK(formatEntry(real, pi, 4) == "3.14159");

    /* DCF ParameterValue overrides DefaultValue */
    Eds dcf;
    dcf.parse("[1800sub1]\nParameterName=COB-ID\nDataType=0x0007\nDefaultValue=$NODEID+0x180\nParameterValue=0x80000190\n");
    CHECK(dcf.value(0x1800, 1, 5, v) && v == 0x80000190);
    std::vector<Pdo> dp = buildPdos(dcf, 5);
    CHECK(dp.size() == 1 && !dp[0].valid && dp[0].cobId == 0x190);

    std::printf("%s (%d failures)\n", fails ? "CANOPEN EDS TEST FAILED" : "CANOPEN EDS TEST PASSED", fails);
    return fails ? 1 : 0;
}
