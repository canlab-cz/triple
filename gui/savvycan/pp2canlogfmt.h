// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef PP2CANLOGFMT_H
#define PP2CANLOGFMT_H

/*
 * PP2CAN text log format (CANLAB s.r.o.), header only and Qt free so it can
 * be unit tested on the host (tests/gui/pp2canlog_test.cpp).
 *
 * File layout, CRLF line endings, as written by PP2CAN:
 *
 *   Received messages
 *   PP2CAN log file - CANLAB s.r.o. - www.canlab.cz
 *   3-7-2014  10-14                       (date, time of the export)
 *   CAN speed: 250k
 *   <TAB>
 *   >  53114  9:53:44.184:0.00  Ext  828  515  8  192 0 0 255 255 175 17 255
 *
 * Data lines have 15 TAB separated columns:
 *   0  direction, ">" received (and "<" sent, not seen in samples)
 *   1  sequence number
 *   2  time of day "   H:M:S.ms:0.00", three leading spaces, every part
 *      an integer WITHOUT zero padding: "9:53:45.4" is 45 s + 4 ms, not
 *      45.4 s; the next frames read 45.8, 45.11, 45.15. The ":0.00" suffix
 *      is kept as is, its meaning is not documented.
 *   3  "Ext" or "Std"
 *   4  extended: identifier bits 28..18 in decimal; standard: identifier
 *   5  extended: identifier bits 17..0 in decimal; standard: " "
 *   6  DLC
 *   7..14  data bytes in decimal, unused columns hold a single space
 *
 * Verified against a 1,007,234 line capture from PP2CAN (extended frames,
 * DLC 3 and 8). Standard frames and sent frames are parsed with the layout
 * above but were not present in the sample.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace pp2canlog {

struct Frame {
    bool     rx = true;
    uint32_t seq = 0;
    int64_t  timeUs = 0;          /* time of day in microseconds */
    bool     ext = false;
    bool     rtr = false;
    uint32_t id = 0;
    uint8_t  dlc = 0;
    uint8_t  data[8] = {};
};

namespace detail {
/* splits on TAB without allocating; returns the number of fields */
inline int split(const char *s, size_t n, const char **f, size_t *fl, int maxf)
{
    int k = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n && k < maxf; i++)
    {
        if (i == n || s[i] == '\t')
        {
            f[k] = s + start;
            fl[k] = i - start;
            k++;
            start = i + 1;
        }
    }
    return k;
}

inline bool num(const char *s, size_t n, uint32_t &v)
{
    while (n && (*s == ' ')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (!n || n > 10)
        return false;
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
        x = x * 10 + uint64_t(s[i] - '0');
    }
    if (x > 0xFFFFFFFFull)
        return false;
    v = uint32_t(x);
    return true;
}

inline bool blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\r' && s[i] != '\n')
            return false;
    return true;
}
}

/* True for the first line of a PP2CAN log ("Received messages" or "Sent
 * messages") followed by the PP2CAN signature line. */
inline bool isHeader(const std::string &line1, const std::string &line2)
{
    return (line1.rfind("Received messages", 0) == 0 || line1.rfind("Sent messages", 0) == 0)
           && line2.find("PP2CAN log file") != std::string::npos;
}

/* Parses one data line. Returns false for header, blank or malformed lines. */
inline bool parseLine(const char *line, size_t len, Frame &f)
{
    using namespace detail;
    const char *c[16];
    size_t cl[16];
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        len--;
    int n = split(line, len, c, cl, 16);
    if (n < 7)
        return false;

    if (cl[0] != 1 || (c[0][0] != '>' && c[0][0] != '<'))
        return false;
    f = Frame();
    f.rx = c[0][0] == '>';
    if (!num(c[1], cl[1], f.seq))
        return false;

    /* time "   H:M:S.ms:0.00", all parts unpadded integers */
    unsigned h, m, sec, ms;
    char tbuf[40];
    size_t tl = cl[2] < sizeof(tbuf) - 1 ? cl[2] : sizeof(tbuf) - 1;
    memcpy(tbuf, c[2], tl);
    tbuf[tl] = 0;
    if (sscanf(tbuf, " %u:%u:%u.%u", &h, &m, &sec, &ms) != 4 || m > 59 || sec > 59 || ms > 999)
        return false;
    f.timeUs = ((int64_t(h) * 60 + m) * 60 + sec) * 1000000ll + int64_t(ms) * 1000;

    std::string type(c[3], cl[3]);
    if (type.compare(0, 3, "Ext") == 0)
        f.ext = true;
    else if (type.compare(0, 3, "Std") != 0)
        return false;
    f.rtr = type.find('R') != std::string::npos || type.find('r') != std::string::npos;

    uint32_t a = 0, b = 0, dlc = 0;
    if (!num(c[4], cl[4], a))
        return false;
    if (f.ext)
    {
        if (!num(c[5], cl[5], b) || a > 0x7FF || b > 0x3FFFF)
            return false;
        f.id = (a << 18) | b;
    }
    else
    {
        if (a > 0x7FF)
            return false;
        f.id = a;
    }
    if (!num(c[6], cl[6], dlc) || dlc > 8)
        return false;
    f.dlc = uint8_t(dlc);
    if (!f.rtr)
    {
        if (n < 7 + int(dlc))
            return false;
        for (unsigned i = 0; i < dlc; i++)
        {
            uint32_t v;
            if (!num(c[7 + i], cl[7 + i], v) || v > 255)
                return false;
            f.data[i] = uint8_t(v);
        }
    }
    return true;
}

/* One data line in PP2CAN layout, without the line ending. */
inline std::string formatLine(const Frame &f)
{
    char buf[160];
    int64_t t = f.timeUs < 0 ? 0 : f.timeUs;
    unsigned ms = unsigned((t / 1000) % 1000), sec = unsigned((t / 1000000) % 60);
    unsigned m = unsigned((t / 60000000) % 60), h = unsigned(t / 3600000000ll);
    int k = snprintf(buf, sizeof(buf), "%c\t%u\t   %u:%u:%u.%u:0.00\t%s\t",
                     f.rx ? '>' : '<', f.seq, h, m, sec, ms, f.ext ? "Ext" : "Std");
    std::string s(buf, size_t(k));
    if (f.ext)
        k = snprintf(buf, sizeof(buf), "%u\t%u\t%u", (f.id >> 18) & 0x7FF, f.id & 0x3FFFF, unsigned(f.dlc));
    else
        k = snprintf(buf, sizeof(buf), "%u\t \t%u", f.id & 0x7FF, unsigned(f.dlc));
    s.append(buf, size_t(k));
    for (int i = 0; i < 8; i++)
    {
        if (i < f.dlc && !f.rtr)
            k = snprintf(buf, sizeof(buf), "\t%u", unsigned(f.data[i]));
        else
            k = snprintf(buf, sizeof(buf), "\t ");
        s.append(buf, size_t(k));
    }
    return s;
}

/* The five header lines (without line endings). date like "3-7-2014  10-14",
 * speed like "250k". */
inline std::string header(bool received, const std::string &date, const std::string &speed)
{
    return std::string(received ? "Received messages" : "Sent messages") + "\r\n"
           "PP2CAN log file - CANLAB s.r.o. - www.canlab.cz\r\n" + date + "\r\n"
           "CAN speed: " + speed + "\r\n\t\r\n";
}

} // namespace pp2canlog

#endif // PP2CANLOGFMT_H
