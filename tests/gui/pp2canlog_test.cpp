// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of the PP2CAN log format (gui/savvycan/pp2canlogfmt.h).
// The Ext lines are taken verbatim from a PP2CAN capture; formatting a
// parsed line must give back exactly the same bytes.
#include <cstdio>
#include <cstring>
#include <string>
#include "pp2canlogfmt.h"

using namespace pp2canlog;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool roundtrip(const char *line)
{
    Frame f;
    if (!parseLine(line, strlen(line), f))
        return false;
    std::string out = formatLine(f) + "\r\n";
    if (out != line)
        std::printf("  got      %s  expected %s", out.c_str(), line);
    return out == line;
}

/* Optional: every data line of a real PP2CAN log must parse and format back
 * to the identical bytes. Usage: pp2canlog_test file.log */
static void checkFile(const char *path)
{
    FILE *fp = std::fopen(path, "rb");
    if (!fp)
    {
        std::printf("  cannot open %s\n", path);
        fails++;
        return;
    }
    char line[512];
    long n = 0, data = 0, bad = 0, mismatch = 0;
    while (std::fgets(line, sizeof(line), fp))
    {
        n++;
        if (n <= 5)
            continue;                                   /* header */
        Frame f;
        data++;
        if (!parseLine(line, strlen(line), f))
        {
            if (bad++ < 3) std::printf("  unparsed line %ld: %s", n, line);
            continue;
        }
        if (formatLine(f) + "\r\n" != line && mismatch++ < 3)
            std::printf("  round trip differs, line %ld: %s", n, line);
    }
    std::fclose(fp);
    std::printf("file: %ld data lines, %ld unparsed, %ld round-trip differences\n", data, bad, mismatch);
    if (bad || mismatch)
        fails++;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        checkFile(argv[1]);

    const char *l1 = ">\t53114\t   9:53:44.184:0.00\tExt\t828\t515\t8\t192\t0\t0\t255\t255\t175\t17\t255\r\n";
    const char *l2 = ">\t53762\t   9:53:44.960:0.00\tExt\t1594\t131075\t3\t230\t254\t0\t \t \t \t \t \r\n";
    const char *l3 = ">\t1060345\t   10:13:53.676:0.00\tExt\t1599\t205056\t8\t32\t17\t80\t0\t0\t0\t0\t90\r\n";

    Frame f;
    CHECK(parseLine(l1, strlen(l1), f));
    CHECK(f.rx && f.seq == 53114 && f.ext && !f.rtr && f.dlc == 8);
    CHECK(f.id == 0x0CF00203);                       /* (828 << 18) | 515, J1939 PGN F002 SA 03 */
    CHECK(f.timeUs == ((9 * 60 + 53) * 60 + 44) * 1000000ll + 184000);
    CHECK(f.data[0] == 192 && f.data[7] == 255);

    CHECK(parseLine(l2, strlen(l2), f));
    CHECK(f.dlc == 3 && f.data[0] == 230 && f.data[1] == 254 && f.data[2] == 0);
    CHECK(f.id == ((1594u << 18) | 131075u));

    CHECK(parseLine(l3, strlen(l3), f) && f.id == 0x18FF2100 && f.seq == 1060345);

    /* unpadded milliseconds: 45.4 is 45 s + 4 ms, 45.11 is 45 s + 11 ms */
    const char *l4 = ">\t53797\t   9:53:45.4:0.00\tExt\t63\t197167\t8\t255\t255\t255\t255\t255\t255\t255\t255\r\n";
    const char *l5 = ">\t53805\t   9:53:45.11:0.00\tExt\t63\t197167\t8\t255\t255\t255\t255\t255\t255\t255\t255\r\n";
    CHECK(parseLine(l4, strlen(l4), f) && f.timeUs == ((9 * 60 + 53) * 60 + 45) * 1000000ll + 4000);
    CHECK(parseLine(l5, strlen(l5), f) && f.timeUs == ((9 * 60 + 53) * 60 + 45) * 1000000ll + 11000);
    CHECK(roundtrip(l4));
    CHECK(roundtrip(l5));

    CHECK(roundtrip(l1));
    CHECK(roundtrip(l2));
    CHECK(roundtrip(l3));

    /* standard and sent frames (layout assumed, not in the sample) */
    Frame s;
    s.rx = false; s.seq = 7; s.timeUs = 3723004000ll; s.ext = false; s.id = 0x123; s.dlc = 2; s.data[0] = 1; s.data[1] = 2;
    std::string sl = formatLine(s);
    CHECK(sl == "<\t7\t   1:2:3.4:0.00\tStd\t291\t \t2\t1\t2\t \t \t \t \t \t ");
    CHECK(parseLine(sl.c_str(), sl.size(), f) && !f.rx && !f.ext && f.id == 0x123 && f.dlc == 2 && f.data[1] == 2);

    /* header and garbage */
    CHECK(isHeader("Received messages", "PP2CAN log file - CANLAB s.r.o. - www.canlab.cz"));
    CHECK(!isHeader("Received messages", "something else"));
    CHECK(header(true, "3-7-2014  10-14", "250k") ==
          "Received messages\r\nPP2CAN log file - CANLAB s.r.o. - www.canlab.cz\r\n3-7-2014  10-14\r\nCAN speed: 250k\r\n\t\r\n");
    const char *bad[] = { "\t\r\n", "Received messages\r\n", "CAN speed: 250k\r\n",
                          ">\t1\t   9:53:44.184:0.00\tExt\t2048\t0\t8\t1\t2\t3\t4\t5\t6\t7\t8",   /* base > 0x7FF */
                          ">\t1\t   9:53:44.184:0.00\tExt\t1\t0\t9\t1\t2\t3\t4\t5\t6\t7\t8",      /* dlc 9 */
                          ">\t1\t   9:53:44.184:0.00\tExt\t1\t0\t8\t1\t2\t3",                      /* too few bytes */
                          ">\t1\t   9:53:44.184:0.00\tExt\t1\t0\t1\t256\t \t \t \t \t \t \t " };     /* byte > 255 */
    for (const char *b : bad)
        CHECK(!parseLine(b, strlen(b), f));

    std::printf("%s (%d failures)\n", fails ? "PP2CANLOG TEST FAILED" : "PP2CANLOG TEST PASSED", fails);
    return fails ? 1 : 0;
}
