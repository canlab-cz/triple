// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the frame list print layout (gui/savvycan/logprintlayout.h).
#include <cstdio>
#include <numeric>
#include "logprintlayout.h"

using namespace logprint;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static long total(const std::vector<int> &v) { return std::accumulate(v.begin(), v.end(), 0L); }

int main()
{
    /* fits: unchanged */
    CHECK((fitColumns({ 100, 50, 300 }, 500, 20) == std::vector<int>{ 100, 50, 300 }));

    /* too wide: only the widest column shrinks */
    std::vector<int> w = fitColumns({ 100, 50, 600 }, 500, 20);
    CHECK((w == std::vector<int>{ 100, 50, 350 }));

    /* two wide columns share the cap */
    w = fitColumns({ 400, 40, 400 }, 500, 20);
    CHECK(w[1] == 40 && w[0] == w[2] && total(w) <= 500 && w[0] >= 229);

    /* typical frame list: time, id, ext, dir, bus, len, data (64-byte FD frame) */
    std::vector<int> nat = { 120, 80, 30, 30, 30, 30, 1500 };
    w = fitColumns(nat, 900, 40);
    CHECK(total(w) <= 900 && w[0] == 120 && w[1] == 80 && w[6] == 900 - 320);

    /* narrow columns below minWidth stay as they are */
    w = fitColumns({ 10, 2000 }, 300, 40);
    CHECK(w[0] == 10 && w[1] == 290);

    /* nothing fits even at minWidth: equal shares */
    w = fitColumns({ 500, 500, 500 }, 90, 40);
    CHECK((w == std::vector<int>{ 30, 30, 30 }));
    CHECK(fitColumns({}, 100, 10).empty());

    /* pagination */
    CHECK((paginate({}, 100) == std::vector<int>{ 0 }));
    CHECK((paginate({ 10, 10, 10 }, 100) == std::vector<int>{ 0 }));
    CHECK((paginate(std::vector<int>(25, 10), 100) == std::vector<int>{ 0, 10, 20 }));
    CHECK((paginate({ 60, 50, 40, 70 }, 100) == std::vector<int>{ 0, 1, 3 }));
    /* a row taller than a page gets a page of its own */
    CHECK((paginate({ 10, 250, 10 }, 100) == std::vector<int>{ 0, 1, 2 }));
    CHECK((paginate({ 250, 10 }, 100) == std::vector<int>{ 0, 1 }));

    std::printf("%s (%d failures)\n", fails ? "LOGPRINT TEST FAILED" : "LOGPRINT TEST PASSED", fails);
    return fails ? 1 : 0;
}
