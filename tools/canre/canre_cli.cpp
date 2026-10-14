// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Command line front end to the reverse engineering analysis of the SavvyCAN
// Signal Finder and AI Assistant (gui/savvycan/re/canre.h, aitools.h): runs
// one of the assistant's tools on a candump log and prints what the
// assistant would get back.
//
//   canre_cli demo.log capture_overview
//   canre_cli demo.log list_ids sort=changing
//   canre_cli demo.log analyze_id id=0C9
//   canre_cli demo.log signal_values id=1A0 start_bit=0 length=16 byte_order=intel factor=0.01 max_points=12
//   canre_cli demo.log changes_around time_s=52 before_s=3 after_s=3
//
// Arguments are name=value as in the tool definitions; id, sort and
// byte_order are text, true/false are flags, everything else is a number.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "aitools.h"

static bool parseCandumpLine(const std::string &line, canre::Capture &cap)
{
    // (1697000000.123456) can0 123#11223344   or   ... 12345678#...   or  ...  123##1112233 (FD)
    double t;
    char iface[32], frame[300];
    if (sscanf(line.c_str(), " (%lf) %31s %299s", &t, iface, frame) != 3)
        return false;
    const char *hash = strchr(frame, '#');
    if (!hash)
        return false;
    std::string id(frame, size_t(hash - frame));
    uint32_t ident;
    if (!aitools::parseId(id, ident))
        return false;
    const char *p = hash + 1;
    if (*p == '#')           // CAN FD: flags nibble after ##
        p += 2;
    if (*p == 'R' || *p == 'r')
        return false;        // remote frame, no data
    uint8_t data[64];
    int n = 0;
    while (p[0] && p[1] && n < 64)
    {
        if (*p == '.') { p++; continue; }
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1)
            break;
        data[n++] = uint8_t(v);
        p += 2;
    }
    int bus = 0;
    for (const char *c = iface; *c; c++)
        if (*c >= '0' && *c <= '9') { bus = atoi(c); break; }
    cap.add(uint64_t(t * 1e6 + 0.5), uint8_t(bus), ident, id.size() > 3, uint8_t(n), data);
    return true;
}

static void usage()
{
    fprintf(stderr,
            "usage: canre_cli LOG TOOL [name=value ...]\n"
            "LOG is a candump log (candump -l, or tools/re_demo.py).\n"
            "TOOL: capture_overview list_ids analyze_id get_frames signal_values compare_windows changes_around\n"
            "Options: max_chars=N (default 20000), max_frames=N (default 200)\n");
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        usage();
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    canre::Capture cap;
    std::string line;
    while (std::getline(in, line))
        parseCandumpLine(line, cap);
    if (!cap.size())
    {
        fprintf(stderr, "%s: no frames in candump format\n", argv[1]);
        return 1;
    }

    std::string tool = argv[2];
    aitools::Limits limits;
    aitools::Args a;
    for (int i = 3; i < argc; i++)
    {
        std::string kv = argv[i];
        size_t eq = kv.find('=');
        if (eq == std::string::npos)
        {
            fprintf(stderr, "argument '%s' is not name=value\n", argv[i]);
            return 2;
        }
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "max_chars")
            limits.maxResultChars = atoi(v.c_str());
        else if (k == "max_frames")
            limits.maxFramesPerResult = atoi(v.c_str());
        else if (k == "id" || k == "sort" || k == "byte_order")
            a.str[k] = v;
        else if (v == "true" || v == "false")
            a.flag[k] = v == "true";
        else
            a.num[k] = atof(v.c_str());
    }
    aitools::Result r = aitools::run(tool, a, cap, limits);
    std::cout << r.text;
    if (!r.text.empty() && r.text.back() != '\n')
        std::cout << '\n';
    return r.error ? 1 : 0;
}
