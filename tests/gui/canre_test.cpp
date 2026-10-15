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

static bool has(const std::string &s, const char *w) { return s.find(w) != std::string::npos; }

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
        {                                                 /* multiplexed: byte 0 page 0..3 (cycles, counter-like) */
            uint8_t m[8] = { uint8_t(i % 4), 0, 0, 0, 0, 0, 0, 0 };
            int k = i / 4;
            if (i % 4 == 0) { for (int b = 1; b < 8; b++) m[b] = uint8_t(0x11 * b); }
            if (i % 4 == 1) { unsigned v = unsigned(1000 + 800 * std::sin(k * 0.05)); m[1] = uint8_t(v); m[2] = uint8_t(v >> 8); }
            if (i % 4 == 2) { m[3] = uint8_t(40 + (k / 100) % 5); for (int b = 4; b < 8; b++) m[b] = 0xFF; }
            cap.add(us + 1000, 0, 0x500, false, 8, m);
        }
        {                                                 /* ramp +5 per frame, Intel bytes 0-1: its low bits count +1 but it is no counter */
            unsigned rv = unsigned(i * 5);
            uint8_t rr[2] = { uint8_t(rv), uint8_t(rv >> 8) };
            cap.add(us + 1700, 0, 0x520, false, 2, rr);
        }
        {                                                 /* counter + checksum over constant data: not a multiplexer */
            uint8_t q[4] = { uint8_t(i & 0x0F), 0x12, 0x34, 0 };
            q[3] = uint8_t(q[0] ^ q[1] ^ q[2]);
            cap.add(us + 1500, 0, 0x510, false, 4, q);
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
    CHECK(ids.size() == 7);
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
    CHECK(cap.spanSeconds() > 19.99 && cap.spanSeconds() < 19.995); /* last frame added: 0x510 at 19.9915 s */

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

    /* multiplexer */
    FrameKey k500; k500.id = 0x500;
    IdAnalysis mux = analyzeId(cap, k500);
    std::printf("%s", describe(mux).c_str());
    f = findField(mux, FieldKind::Multiplexer, 0);
    CHECK(f && (f->length == 8 || f->length == 4) && f->unique == 4);
    CHECK(mux.muxStart == 0 && mux.muxValues.size() == 4);
    CHECK(!findField(mux, FieldKind::Counter, 0));               /* the page selector is not reported as a counter */
    IdAnalysis p1 = analyzeId(cap, k500, -1, -1, mux.muxStart, mux.muxLength, 1);
    std::printf("%s", describe(p1).c_str());
    CHECK(p1.frames == 500 && p1.muxFilter == 1 && has(describe(p1), "only frames with the multiplexer"));
    f = findField(p1, FieldKind::Signal, 8);                    /* page 1: 16 bit Intel sine at bytes 1-2 */
    CHECK(f && !f->bigEndian && f->length >= 10 && f->minRaw >= 200 && f->maxRaw <= 1800);
    IdAnalysis p0 = analyzeId(cap, k500, -1, -1, mux.muxStart, mux.muxLength, 0);
    CHECK(p0.frames == 500 && findField(p0, FieldKind::Constant, 8) && findField(p0, FieldKind::Constant, 56));

    FrameKey k510; k510.id = 0x510;
    IdAnalysis cc = analyzeId(cap, k510);
    std::printf("%s", describe(cc).c_str());
    CHECK(cc.muxStart < 0 && findField(cc, FieldKind::Counter, 0) && findField(cc, FieldKind::Checksum, 24));

    FrameKey k520; k520.id = 0x520;
    IdAnalysis rp = analyzeId(cap, k520);
    std::printf("%s", describe(rp).c_str());
    f = findField(rp, FieldKind::Signal, 0);
    CHECK(f && !f->bigEndian && f->length == 14 && f->maxRaw == 1999 * 5);
    CHECK(!findField(rp, FieldKind::Counter, 0));

    /* byte order from carries: a Motorola value with a noisy low byte (byte 1)
     * next to an unrelated slow byte (byte 2). Low byte + byte 2 in Intel
     * order look as good by flip rates alone; only byte 0 carries with it. */
    {
        Capture c2;
        for (int i = 0; i < 3000; i++)
        {
            double x = i / 3000.0;
            int v = int(3200 + 9000 * x + (rand() % 121) - 60);
            uint8_t d[4] = { uint8_t(v >> 8), uint8_t(v), uint8_t(80 * std::sin(x * 6) + 100), 0 };
            c2.add(uint64_t(i) * 10000, 0, 0x530, false, 4, d);
        }
        FrameKey k530; k530.id = 0x530;
        IdAnalysis mo = analyzeId(c2, k530);
        std::printf("%s", describe(mo).c_str());
        bool found = false;
        for (const Field &fl : mo.fields)
            if (fl.kind == FieldKind::Signal && fl.bigEndian && fl.startBit / 8 == 0 && fl.length == fl.startBit + 1 + 8)  /* to the end of byte 1 */
                found = fl.minRaw >= 3100 && fl.maxRaw <= 12300;
        CHECK(found);
        CHECK(findField(mo, FieldKind::Signal, 16) && findField(mo, FieldKind::Signal, 16)->length == 8);
    }

    /* multiplexer whose pages put different values into one byte (byte 4:
     * the high byte of a noisy current on page 0, a temperature on page 1,
     * constants on pages 2 and 3). Frames with the same byte 4 look even
     * more alike than frames of the same page, but byte 4 only refines the
     * pages: the selector is byte 0. */
    {
        Capture c3;
        for (int i = 0; i < 2400; i++)
        {
            int page = i % 4;
            uint8_t d[8] = { uint8_t(page), 0, 0, 0, 0, 0, 0, 0 };
            if (page == 0)
            {
                int cur = -50 - (i / 4) % 1500 + rand() % 21 - 10;   /* 0.1 A, negative */
                int volt = 3960 + rand() % 5 - 2;
                d[1] = uint8_t(volt); d[2] = uint8_t(volt >> 8);
                d[3] = uint8_t(cur); d[4] = uint8_t(cur >> 8);
            }
            else if (page == 1)
                for (int k = 0; k < 4; k++) d[1 + k] = uint8_t(65 + k + i / 800);
            else if (page == 2)
                d[1] = uint8_t(160 - i / 300);
            else
            {
                d[1] = 0x12; d[2] = 0x34; d[3] = 0x56; d[4] = 0x78;
            }
            c3.add(uint64_t(i) * 25000, 0, 0x540, false, 8, d);
        }
        FrameKey k540; k540.id = 0x540;
        IdAnalysis bm = analyzeId(c3, k540);
        std::printf("%s", describe(bm).c_str());
        CHECK(bm.muxStart == 0 && bm.muxValues.size() == 4);
    }

    /* a signed 16-bit value around zero is reported signed, with its range */
    {
        Capture c5;
        for (int i = 0; i < 1000; i++)
        {
            int v = int(-1000 * std::sin(i / 100.0) + 300);              /* -700 .. 1300 */
            uint8_t d[3] = { uint8_t(v), uint8_t(v >> 8), 0x11 };
            c5.add(uint64_t(i) * 10000, 0, 0x560, false, 3, d);
        }
        FrameKey k560; k560.id = 0x560;
        IdAnalysis sg = analyzeId(c5, k560);
        std::printf("%s", describe(sg).c_str());
        bool okSigned = false;
        for (const Field &fl : sg.fields)
            if (fl.kind == FieldKind::Signal && fl.isSigned && int64_t(fl.minRaw) >= -700 && int64_t(fl.minRaw) <= -690 &&
                int64_t(fl.maxRaw) >= 1290 && int64_t(fl.maxRaw) <= 1300)
                okSigned = true;
        CHECK(okSigned && has(describe(sg), "signed"));
        CHECK(signExtend(0xFFFF, 16) == -1 && signExtend(0x7FFF, 16) == 32767 && signExtend(0x8, 4) == -8);
    }

    /* window comparison: a lamp that starts blinking (toggles every 5th
     * frame) ranks above the bits of a value that drifts in both windows */
    {
        Capture c4;
        for (int i = 0; i < 800; i++)
        {
            int temp = 60 + i / 12;                                  /* drifts slowly */
            uint8_t lamp = (i >= 400 && (i / 5) % 2) ? 0x20 : 0;
            uint8_t d[2] = { lamp, uint8_t(temp) };
            c4.add(uint64_t(i) * 10000, 0, 0x550, false, 2, d);
        }
        WindowDiff wb = diffWindows(c4, 0, 3.99, 4.0, 7.99);
        std::printf("%s", describe(wb).c_str());
        CHECK(!wb.bits.empty() && wb.bits[0].byte == 0 && wb.bits[0].bit == 5 && wb.bits[0].score < 1.0);
    }

    /* DBC export */
    DbcMessage m;
    m.key = k100; m.name = "Engine 1"; m.length = 8;
    DbcSignal s1; s1.name = "rpm"; s1.startBit = 11; s1.length = 12; s1.bigEndian = true; s1.factor = 0.5; s1.maxV = 2000; s1.unit = "rpm";
    s1.comment = "found by \"READ\"";
    DbcSignal s2; s2.name = "ramp"; s2.startBit = 24; s2.length = 14; s2.isSigned = true;
    m.signals_ = { s1, s2 };
    DbcMessage mm; mm.key = k500; mm.name = "Pages"; mm.length = 8;
    DbcSignal sel; sel.name = "page"; sel.startBit = 0; sel.length = 8; sel.muxRole = 1;
    DbcSignal pg1; pg1.name = "level"; pg1.startBit = 8; pg1.length = 16; pg1.muxRole = 2; pg1.muxValue = 1;
    mm.signals_ = { sel, pg1 };
    DbcMessage mx; mx.key = kx; mx.name = "Ext"; mx.length = 4;
    std::string dbc = toDbc({ m, mx, mm });
    std::printf("%s", dbc.c_str());
    CHECK(dbc.find("BO_ 256 Engine_1: 8 Vector__XXX") != std::string::npos);
    CHECK(dbc.find(" SG_ rpm : 11|12@0+ (0.5,0) [0|2000] \"rpm\" Vector__XXX") != std::string::npos);
    CHECK(dbc.find(" SG_ ramp : 24|14@1- (1,0) [0|0] \"\" Vector__XXX") != std::string::npos);
    CHECK(dbc.find("BO_ 2566853172 Ext: 4") != std::string::npos);            /* 0x18FF1234 | 0x80000000 */
    CHECK(dbc.find("CM_ SG_ 256 rpm \"found by 'READ'\";") != std::string::npos);
    CHECK(dbc.find(" SG_ page M : 0|8@1+") != std::string::npos && dbc.find(" SG_ level m1 : 8|16@1+") != std::string::npos);

    std::printf("%s (%d failures)\n", fails ? "CANRE TEST FAILED" : "CANRE TEST PASSED", fails);
    return fails ? 1 : 0;
}
