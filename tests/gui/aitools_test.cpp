// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the AI assistant's tools and API limits (gui/savvycan/re/aitools.h).
//   aitools_test           run the checks
//   aitools_test --defs    print the tool definitions (JSON, checked by the Makefile)
#include <cmath>
#include <cstdio>
#include <cstring>
#include "aitools.h"

using namespace aitools;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool has(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }

int main(int argc, char **argv)
{
    Limits l;
    if (argc > 1 && !strcmp(argv[1], "--defs"))
    {
        l.allowTransmit = true;
        std::printf("%s\n", toolDefinitions(l).c_str());
        return 0;
    }

    /* ---- budget ---- */
    {
        Limits lim;
        lim.sessionTokenBudget = 10000;
        lim.dailyTokenBudget = 15000;
        lim.maxRequestsPerMinute = 3;
        lim.maxOutputTokens = 4096;
        Budget b;
        long long wait;
        CHECK(b.check(lim, 0, "2026-10-14", 1000, wait).empty() && wait == 0);
        CHECK(b.maxTokens(lim, 1000) == 4096);
        b.requestSent(0); b.requestSent(1000); b.requestSent(2000);
        std::string why = b.check(lim, 3000, "2026-10-14", 1000, wait);
        CHECK(has(why, "rate limit") && wait == 57000);               /* oldest request leaves the window at 60 s */
        CHECK(b.check(lim, 60001, "2026-10-14", 1000, wait).empty());
        Usage u; u.input = 6000; u.output = 1500; u.cacheRead = 500; u.requests = 1;
        b.addUsage("2026-10-14", u);
        CHECK(b.session.total() == 8000 && b.today.total() == 8000);
        CHECK(b.maxTokens(lim, 1000) == 1000);                          /* 10000 - 8000 - 1000 */
        why = b.check(lim, 120000, "2026-10-14", 2500, wait);
        CHECK(has(why, "session token budget") && wait == 0);           /* 8000 + 2501 > 10000 */
        /* a new day resets the daily count, not the session */
        b.session = Usage();
        CHECK(b.check(lim, 120000, "2026-10-14", 7500, wait).find("daily") != std::string::npos);
        CHECK(b.check(lim, 120000, "2026-10-15", 7500, wait).empty() && b.today.total() == 0);
        /* 0 = unlimited */
        Limits open; open.sessionTokenBudget = 0; open.dailyTokenBudget = 0; open.maxRequestsPerMinute = 0;
        Budget b2;
        for (int i = 0; i < 100; i++) b2.requestSent(i);
        b2.session.input = 1L << 40;
        CHECK(b2.check(open, 100, "d", 1000000, wait).empty() && b2.maxTokens(open, 1000000) == open.maxOutputTokens);
        /* cost: 1M input at 3, 1M output at 15, cache write 1.25x, read 0.1x */
        Limits p; p.priceInputPerMTok = 3; p.priceOutputPerMTok = 15;
        Usage c; c.input = 1000000; c.output = 1000000; c.cacheWrite = 1000000; c.cacheRead = 1000000;
        CHECK(std::fabs(b.cost(p, c) - (3 + 15 + 3.75 + 0.3)) < 1e-9);
        CHECK(estimateTokens(4000) == 1001);
    }

    /* ---- tool definitions ---- */
    {
        Limits lim;
        std::string d = toolDefinitions(lim);
        CHECK(has(d, "\"analyze_id\"") && has(d, "\"propose_dbc\"") && !has(d, "send_frame"));
        lim.allowTransmit = true;
        CHECK(has(toolDefinitions(lim), "\"send_frame\""));
        CHECK(!systemPrompt().empty());
    }

    /* ---- ID parsing ---- */
    uint32_t id;
    CHECK(parseId("1A3", id) && id == 0x1A3);
    CHECK(parseId("0x18FEF100", id) && id == 0x18FEF100);
    CHECK(!parseId("XYZ", id) && !parseId("", id) && !parseId("3FFFFFFF", id) && !parseId("123456789", id));

    /* ---- tools on a small capture ---- */
    canre::Capture cap;
    for (int i = 0; i < 1000; i++)
    {
        uint64_t us = 1000000 + uint64_t(i) * 10000;
        uint8_t d[4] = { uint8_t(i), uint8_t(i >= 500 ? 1 : 0), uint8_t(100 + (i % 50)), 0x42 };
        cap.add(us, 0, 0x123, false, 4, d);
        if (i % 4 == 0)
        {
            uint8_t e[2] = { 0x10, uint8_t(i / 4) };
            cap.add(us + 2000, 1, 0x18FEF100, true, 2, e);
        }
    }
    Limits lim;
    Args none;
    Result r = run("capture_overview", none, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && has(r.text, "1250 frames") && has(r.text, "2 distinct IDs") && has(r.text, "buses: 0, 1"));

    Args li; li.str["sort"] = "count";
    r = run("list_ids", li, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && r.text.find("123") < r.text.find("18FEF100@1"));   /* by count: 0x123 first */
    li.num["bus"] = 1;
    r = run("list_ids", li, cap, lim);
    CHECK(has(r.text, "18FEF100") && r.text.find("\n123 ") == std::string::npos);
    li.num.erase("bus"); li.num["limit"] = 1;
    r = run("list_ids", li, cap, lim);
    CHECK(has(r.text, "1 more IDs not listed"));

    Args an; an.str["id"] = "123";
    r = run("analyze_id", an, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && has(r.text, "counter") && has(r.text, "1000 frames"));
    an.num["start_s"] = 0; an.num["end_s"] = 4.995;
    r = run("analyze_id", an, cap, lim);
    CHECK(has(r.text, "500 frames"));
    Args mx; mx.str["id"] = "123"; mx.num["mux_start"] = 8; mx.num["mux_length"] = 8; mx.num["mux_value"] = 1;
    r = run("analyze_id", mx, cap, lim);                                  /* byte 1 is 1 from 5 s on */
    CHECK(!r.error && has(r.text, "500 frames") && has(r.text, "multiplexer at bit 8, 8 bits = 1"));
    mx.num.erase("mux_length");
    CHECK(run("analyze_id", mx, cap, lim).error);
    mx.num["mux_length"] = 8; mx.num.erase("mux_value");
    CHECK(run("analyze_id", mx, cap, lim).error);
    Args bad; bad.str["id"] = "7FF";
    r = run("analyze_id", bad, cap, lim);
    CHECK(r.error && has(r.text, "not in the capture"));
    bad.str["id"] = "zz";
    CHECK(run("analyze_id", bad, cap, lim).error);
    Args wrongBus; wrongBus.str["id"] = "123"; wrongBus.num["bus"] = 1;
    CHECK(run("analyze_id", wrongBus, cap, lim).error);

    Args gf; gf.str["id"] = "18fef100"; gf.num["limit"] = 3;
    r = run("get_frames", gf, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && has(r.text, "250 frames of 18FEF100@1 in range, showing 3") && has(r.text, "    0.0020 10 00"));
    lim.maxFramesPerResult = 2;
    gf.num["limit"] = 1000;
    r = run("get_frames", gf, cap, lim);
    CHECK(has(r.text, "showing 2"));
    lim.maxFramesPerResult = 200;

    Args sv; sv.str["id"] = "123"; sv.num["start_bit"] = 16; sv.num["length"] = 8; sv.str["byte_order"] = "intel";
    sv.num["factor"] = 0.5; sv.num["offset"] = -10; sv.num["max_points"] = 5;
    r = run("signal_values", sv, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && has(r.text, "1000 values, min 40 max 64.5") && has(r.text, "50 distinct"));
    CHECK(std::count(r.text.begin(), r.text.end(), '\n') == 2 + 5);
    sv.num["start_bit"] = 30; sv.num["length"] = 16;                        /* runs past the 4 bytes */
    CHECK(run("signal_values", sv, cap, lim).error);
    Args sg; sg.str["id"] = "123"; sg.num["start_bit"] = 0; sg.num["length"] = 8; sg.str["byte_order"] = "intel"; sg.flag["signed"] = true;
    r = run("signal_values", sg, cap, lim);
    CHECK(has(r.text, "min -128 max 127"));
    sv.num["start_bit"] = 16; sv.num["length"] = 8;                         /* only frames whose byte 1 is 1 */
    sv.num["mux_start"] = 8; sv.num["mux_length"] = 8; sv.num["mux_value"] = 1;
    r = run("signal_values", sv, cap, lim);
    CHECK(!r.error && has(r.text, "500 values"));
    Args mbad = sv; mbad.num.erase("mux_length");
    CHECK(run("signal_values", mbad, cap, lim).error);

    Args cw; cw.num["a_start"] = 0; cw.num["a_end"] = 4.9; cw.num["b_start"] = 5.1; cw.num["b_end"] = 9.99;
    r = run("compare_windows", cw, cap, lim);
    std::printf("%s", r.text.c_str());
    CHECK(!r.error && has(r.text, "123  1.0  0.00 -> 1.00"));
    Args ca; ca.num["time_s"] = 5.0; ca.num["before_s"] = 1; ca.num["after_s"] = 1;
    r = run("changes_around", ca, cap, lim);
    CHECK(!r.error && has(r.text, "window A 4.000..5.000 s, window B 5.000..6.000 s") && has(r.text, "123  1.0"));
    Args cbad; cbad.num["a_start"] = 3; cbad.num["a_end"] = 2; cbad.num["b_start"] = 0; cbad.num["b_end"] = 1;
    CHECK(run("compare_windows", cbad, cap, lim).error);

    /* size limit on results */
    lim.maxResultChars = 100;
    r = run("analyze_id", an, cap, lim);
    CHECK(r.text.size() < 220 && has(r.text, "[cut at 100 characters"));
    lim.maxResultChars = 20000;

    CHECK(run("propose_dbc", none, cap, lim).error && run("no_such_tool", none, cap, lim).error);

    std::printf("%s (%d failures)\n", fails ? "AITOOLS TEST FAILED" : "AITOOLS TEST PASSED", fails);
    return fails ? 1 : 0;
}
