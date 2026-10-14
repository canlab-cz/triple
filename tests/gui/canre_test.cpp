// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the reverse engineering analysis (gui/savvycan/re/canre.h)
// on a synthetic capture with known contents.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "canre.h"

using namespace canre;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t crc8j1850(const uint8_t *p, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? uint8_t((c << 1) ^ 0x1D) : uint8_t(c << 1);
    }
    return uint8_t(c ^ 0xFF);
}

static const Field *findField(const IdAnalysis &a, FieldKind k, int start)
{
    for (const Field &f : a.fields)
        if (f.kind == k && f.startBit == start) return &f;
    return nullptr;
}

int main()
{
    Capture cap;
    srand(1);
    const uint64_t t0 = 5000000;                          /* capture starts at 5 s on the adapter clock */
    for (int i = 0; i < 2000; i++)                        /* 20 s, 10 ms period */
    {
        uint64_t us = t0 + uint64_t(i) * 10000;
        double t = i * 0.01;
        uint8_t d[8];
        d[0] = uint8_t(0xA0 | (i & 0x0F));                /* constant high nibble, 4 bit counter */
        unsigned sine = unsigned(2000 + 1999 * std::sin(t * 0.7));   /* 12 bit, Motorola, MSB byte 1 */
        d[1] = uint8_t(sine >> 8);
        d[2] = uint8_t(sine);
        unsigned ramp = unsigned(i * 7);                  /* 14 bit, Intel, LSB byte 3 */
        d[3] = uint8_t(ramp);
        d[4] = uint8_t(ramp >> 8);
        d[5] = uint8_t(0x08 | ((i / 300) & 1));           /* bit 3 always set, bit 0 toggles every 3 s */
        d[6] = 0x55;
        d[7] = crc8j1850(d, 7);
        cap.add(us, 0, 0x100, false, 8, d);

        if (i % 2 == 0)                                   /* 20 ms: counter, noise, XOR checksum */
        {
            uint8_t e[4] = { uint8_t(i / 2), uint8_t(rand()), uint8_t(rand()), 0 };
            e[3] = uint8_t(e[0] ^ e[1] ^ e[2] ^ 0x5A);
            cap.add(us + 3000, 0, 0x18FF1234, true, 4, e);
        }
        if (i % 5 == 0)                                   /* 50 ms: a switch at 10 s and a noisy bit */
        {
            uint8_t s = uint8_t((t >= 10 ? 0x04 : 0) | (rand() & 1));
            cap.add(us + 5000, 1, 0x300, false, 1, &s);
        }
        if (t >= 10 && i % 10 == 0)                       /* appears only after 10 s */
        {
            uint8_t z[2] = { 1, 2 };
            cap.add(us + 7000, 0, 0x400, false, 2, z);
        }
    }

    /* bit extraction and DBC conventions */
    const uint8_t x[2] = { 0x12, 0x34 };
    CHECK(extractBits(x, 2, 7, 16, true) == 0x1234);
    CHECK(extractBits(x, 2, 3, 12, true) == 0x234);
    CHECK(extractBits(x, 2, 0, 16, false) == 0x3412);
    CHECK(extractBits(x, 2, 4, 8, false) == 0x41);
    bool ok;
    extractBits(x, 2, 7, 24, true, &ok);
    CHECK(!ok);
    CHECK(motorolaStartFromLsb(8, 16) == 7 && motorolaStartFromLsb(8, 12) == 3 && motorolaStartFromLsb(0, 4) == 3);

    /* overview */
    std::vector<IdInfo> ids = listIds(cap);
    CHECK(ids.size() == 4);
    for (const IdInfo &i : ids)
    {
        if (i.key.id == 0x100)
        {
            CHECK(i.count == 2000 && std::fabs(i.periodMs - 10.0) < 0.01 && i.jitterMs < 0.01);
            CHECK(i.changingBytes == 7 && i.lengths.size() == 1 && *i.lengths.begin() == 8);
        }
        if (i.key.id == 0x18FF1234) CHECK(i.key.ext && std::fabs(i.periodMs - 20.0) < 0.01);
        if (i.key.id == 0x300) CHECK(i.key.bus == 1);
        if (i.key.id == 0x400) CHECK(i.firstS >= 10.0);
    }
    CHECK(std::fabs(cap.spanSeconds() - 19.99) < 0.001);         /* last frame added: 0x100 at 19.99 s */

    /* ID 0x100 */
    FrameKey k100; k100.id = 0x100;
    IdAnalysis a = analyzeId(cap, k100);
    std::printf("%s", describe(a).c_str());
    CHECK(a.frames == 2000 && a.length == 8);
    const Field *f = findField(a, FieldKind::Counter, 0);
    CHECK(f && f->length == 4 && f->unique == 16);
    f = findField(a, FieldKind::Checksum, 56);
    CHECK(f && f->note.find("J1850") != std::string::npos);
    f = findField(a, FieldKind::Signal, 11);                /* Motorola, MSB = value bit 11 = byte 1 bit 3 */
    CHECK(f && f->bigEndian && f->length == 12);
    if (f) CHECK(f->minRaw >= 1 && f->maxRaw <= 3999 && f->unique > 1000);
    f = findField(a, FieldKind::Signal, 24);                /* Intel, LSB = byte 3 bit 0 */
    CHECK(f && !f->bigEndian && f->length == 14 && f->maxRaw == 1999 * 7);
    f = findField(a, FieldKind::Flag, 40);
    CHECK(f && f->length == 1);
    f = findField(a, FieldKind::Constant, 48);
    CHECK(f && f->minRaw == 0x55);
    CHECK(std::fabs(a.flipRate[0] - 1.0) < 1e-9);           /* counter LSB flips every frame */
    CHECK(a.flipRate[5 * 8 + 3] == 0 && a.oneRate[5 * 8 + 3] == 1.0);
    CHECK(a.bytes[6].unique == 1 && a.bytes[6].entropy == 0);

    /* ID 0x18FF1234: 8 bit counter, XOR checksum with constant */
    FrameKey kx; kx.id = 0x18FF1234; kx.ext = true;
    IdAnalysis b = analyzeId(cap, kx);
    std::printf("%s", describe(b).c_str());
    f = findField(b, FieldKind::Counter, 0);
    CHECK(f && f->length == 8);
    f = findField(b, FieldKind::Checksum, 24);
    CHECK(f && f->note.find("XOR") != std::string::npos && f->note.find("0x5A") != std::string::npos);

    /* time window restricts the analysis */
    IdAnalysis c = analyzeId(cap, k100, 0, 4.995);
    CHECK(c.frames == 500);

    /* what changed at 10 s */
    WindowDiff w = diffWindows(cap, 0, 9.9, 10.1, 20);
    std::printf("%s", describe(w).c_str());
    CHECK(w.onlyB.size() == 1 && w.onlyB[0].id == 0x400 && w.onlyA.empty());
    CHECK(!w.bits.empty() && w.bits[0].key.id == 0x300 && w.bits[0].byte == 0 && w.bits[0].bit == 2);
    if (!w.bits.empty()) CHECK(w.bits[0].oneA == 0 && w.bits[0].oneB == 1);
    for (const BitChange &bc : w.bits)                       /* the noisy bit is not a change */
        CHECK(!(bc.key.id == 0x300 && bc.bit == 0));

    /* DBC export */
    DbcMessage m;
    m.key = k100; m.name = "Engine 1"; m.length = 8;
    DbcSignal s1; s1.name = "rpm"; s1.startBit = 11; s1.length = 12; s1.bigEndian = true; s1.factor = 0.5; s1.maxV = 2000; s1.unit = "rpm";
    s1.comment = "found by \"READ\"";
    DbcSignal s2; s2.name = "ramp"; s2.startBit = 24; s2.length = 14; s2.isSigned = true;
    m.signals_ = { s1, s2 };
    DbcMessage mx; mx.key = kx; mx.name = "Ext"; mx.length = 4;
    std::string dbc = toDbc({ m, mx });
    std::printf("%s", dbc.c_str());
    CHECK(dbc.find("BO_ 256 Engine_1: 8 Vector__XXX") != std::string::npos);
    CHECK(dbc.find(" SG_ rpm : 11|12@0+ (0.5,0) [0|2000] \"rpm\" Vector__XXX") != std::string::npos);
    CHECK(dbc.find(" SG_ ramp : 24|14@1- (1,0) [0|0] \"\" Vector__XXX") != std::string::npos);
    CHECK(dbc.find("BO_ 2566853172 Ext: 4") != std::string::npos);            /* 0x18FF1234 | 0x80000000 */
    CHECK(dbc.find("CM_ SG_ 256 rpm \"found by 'READ'\";") != std::string::npos);

    std::printf("%s (%d failures)\n", fails ? "CANRE TEST FAILED" : "CANRE TEST PASSED", fails);
    return fails ? 1 : 0;
}
