// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef AITOOLS_H
#define AITOOLS_H

/*
 * Tools the AI assistant can call on the captured frames, and the limits on
 * its use of the API. Qt free so both are unit tested on the host
 * (tests/gui/aitools_test.cpp); the window (aiassistantwindow.cpp) only
 * converts JSON and talks HTTP.
 *
 * Every tool returns compact text: tables and summaries rather than raw
 * dumps, cut to the configured size, because tool results are paid input
 * tokens.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "canre.h"

namespace aitools {

/* ------------------------------------------------------------------ limits */

struct Limits {
    int    maxOutputTokens = 4096;      /* max_tokens of each request */
    int    maxToolCalls = 15;           /* per question */
    int    maxFramesPerResult = 200;    /* get_frames rows, signal_values points */
    int    maxResultChars = 20000;      /* any single tool result */
    long   sessionTokenBudget = 500000; /* input + output + cache, 0 = no limit */
    long   dailyTokenBudget = 2000000;  /* same, per calendar day, 0 = no limit */
    int    maxRequestsPerMinute = 10;   /* 0 = no limit */
    bool   allowTransmit = false;       /* offer send_frame at all */
    std::set<uint32_t> transmitAllowlist; /* empty = any ID (each frame is confirmed anyway) */
    double priceInputPerMTok = 0;       /* optional cost display, 0 = off */
    double priceOutputPerMTok = 0;
};

struct Usage {
    long input = 0, output = 0, cacheWrite = 0, cacheRead = 0, requests = 0;
    long total() const { return input + output + cacheWrite + cacheRead; }
    void add(const Usage &u)
    {
        input += u.input; output += u.output; cacheWrite += u.cacheWrite; cacheRead += u.cacheRead; requests += u.requests;
    }
};

/* Accounting against the limits. Times are milliseconds on any monotonic
 * clock, day is any string that changes at midnight (e.g. "2026-10-14"). */
class Budget {
public:
    Usage session, today;
    std::string day;

    /* Empty if a request may be sent now; otherwise why not. waitMs > 0 means
     * only the rate limit is in the way and the request may go after that. */
    std::string check(const Limits &l, long long nowMs, const std::string &d, long estimatedInput, long long &waitMs)
    {
        rollDay(d);
        waitMs = 0;
        long need = estimatedInput + 1;
        if (l.sessionTokenBudget > 0 && session.total() + need > l.sessionTokenBudget)
            return "session token budget used up (" + std::to_string(session.total()) + " of " + std::to_string(l.sessionTokenBudget) + ")";
        if (l.dailyTokenBudget > 0 && today.total() + need > l.dailyTokenBudget)
            return "daily token budget used up (" + std::to_string(today.total()) + " of " + std::to_string(l.dailyTokenBudget) + ")";
        while (!stamps.empty() && nowMs - stamps.front() >= 60000)
            stamps.pop_front();
        if (l.maxRequestsPerMinute > 0 && int(stamps.size()) >= l.maxRequestsPerMinute)
        {
            waitMs = 60000 - (nowMs - stamps.front());
            return "rate limit: " + std::to_string(l.maxRequestsPerMinute) + " requests per minute";
        }
        return std::string();
    }

    /* max_tokens for the next request: the configured maximum, but never
     * more than what is left of the budgets after the estimated input */
    int maxTokens(const Limits &l, long estimatedInput) const
    {
        long m = l.maxOutputTokens;
        if (l.sessionTokenBudget > 0) m = std::min(m, l.sessionTokenBudget - session.total() - estimatedInput);
        if (l.dailyTokenBudget > 0) m = std::min(m, l.dailyTokenBudget - today.total() - estimatedInput);
        return int(std::max(0L, m));
    }

    void requestSent(long long nowMs) { stamps.push_back(nowMs); }

    void addUsage(const std::string &d, const Usage &u)
    {
        rollDay(d);
        session.add(u);
        today.add(u);
    }

    double cost(const Limits &l, const Usage &u) const
    {
        /* cache writes cost 1.25x input, cache reads 0.1x (Anthropic pricing rules) */
        return (u.input + 1.25 * u.cacheWrite + 0.1 * u.cacheRead) * l.priceInputPerMTok / 1e6 + u.output * l.priceOutputPerMTok / 1e6;
    }

private:
    std::deque<long long> stamps;
    void rollDay(const std::string &d)
    {
        if (d != day) { day = d; today = Usage(); }
    }
};

/* Rough input estimate for the budget check before a request: 4 characters
 * per token is typical for English text and JSON. */
inline long estimateTokens(size_t chars) { return long(chars / 4 + 1); }

/* ------------------------------------------------------------------- tools */

struct Args {
    std::map<std::string, std::string> str;
    std::map<std::string, double> num;
    std::map<std::string, bool> flag;

    bool has(const std::string &k) const { return str.count(k) || num.count(k) || flag.count(k); }
    double n(const std::string &k, double def) const { auto it = num.find(k); return it == num.end() ? def : it->second; }
    std::string s(const std::string &k, const std::string &def = "") const { auto it = str.find(k); return it == str.end() ? def : it->second; }
    bool b(const std::string &k, bool def) const { auto it = flag.find(k); return it == flag.end() ? def : it->second; }
};

/* JSON tool definitions for the Messages API (the "tools" array members). */
inline std::string toolDefinitions(const Limits &l)
{
    std::string t = R"([
{"name":"capture_overview","description":"Size of the capture: frame count, time span in seconds (times are relative to the first frame), buses, number of distinct IDs.","input_schema":{"type":"object","properties":{}}},
{"name":"list_ids","description":"One line per CAN ID: bus, count, mean period and jitter in ms, lengths, number of bytes that change, distinct payloads. IDs are hex.","input_schema":{"type":"object","properties":{"bus":{"type":"integer","description":"only this bus"},"sort":{"type":"string","enum":["id","count","changing"]},"limit":{"type":"integer","description":"max lines, default 100"}}}},
{"name":"analyze_id","description":"Statistics of one ID: per byte min/max/distinct/changes/entropy, bit flip rate per bit, and detected fields: rolling counters, checksums (XOR, SUM, CRC-8 J1850/AUTOSAR), constants, flags, a multiplexer (a field whose value selects the layout of the other bytes), and signal candidates segmented by bit flip rates with byte order. Start bits follow DBC: Intel = LSB position, Motorola = MSB position, position = byte*8 + bit. For a multiplexed ID, call again with mux_start/mux_length/mux_value to analyze one page.","input_schema":{"type":"object","properties":{"id":{"type":"string","description":"hex, e.g. \"1A3\" or \"18FEF100\""},"bus":{"type":"integer"},"start_s":{"type":"number"},"end_s":{"type":"number"},"mux_start":{"type":"integer","description":"multiplexer start bit (Intel), from a previous analyze_id"},"mux_length":{"type":"integer"},"mux_value":{"type":"integer","description":"only frames with this multiplexer value"}},"required":["id"]}},
{"name":"get_frames","description":"Raw frames of one ID: time in s and data bytes in hex. Prefer analyze_id and signal_values; use this to look at a few concrete frames.","input_schema":{"type":"object","properties":{"id":{"type":"string"},"bus":{"type":"integer"},"start_s":{"type":"number"},"end_s":{"type":"number"},"limit":{"type":"integer"}},"required":["id"]}},
{"name":"signal_values","description":"Decode a bit field of one ID over time: statistics and an evenly downsampled list of (time s, value). Value = raw * factor + offset. For a signal on one page of a multiplexed ID give mux_start/mux_length/mux_value, otherwise the other pages are decoded too.","input_schema":{"type":"object","properties":{"id":{"type":"string"},"bus":{"type":"integer"},"start_bit":{"type":"integer"},"length":{"type":"integer"},"byte_order":{"type":"string","enum":["intel","motorola"]},"signed":{"type":"boolean"},"factor":{"type":"number"},"offset":{"type":"number"},"start_s":{"type":"number"},"end_s":{"type":"number"},"max_points":{"type":"integer"},"mux_start":{"type":"integer","description":"multiplexed ID: only frames whose multiplexer (Intel bit field at mux_start, mux_length bits) has mux_value"},"mux_length":{"type":"integer"},"mux_value":{"type":"integer"}},"required":["id","start_bit","length","byte_order"]}},
{"name":"compare_windows","description":"What differs between time window A and B: IDs present in only one, changed frame rates, and the bits whose level or toggling differs most (a switch or state shows as a steady bit at a different level).","input_schema":{"type":"object","properties":{"a_start":{"type":"number"},"a_end":{"type":"number"},"b_start":{"type":"number"},"b_end":{"type":"number"},"max_bits":{"type":"integer"}},"required":["a_start","a_end","b_start","b_end"]}},
{"name":"changes_around","description":"compare_windows for the seconds before and after a moment, e.g. when the user pressed a button.","input_schema":{"type":"object","properties":{"time_s":{"type":"number"},"before_s":{"type":"number"},"after_s":{"type":"number"}},"required":["time_s"]}},
{"name":"propose_dbc","description":"Offer decoded messages and signals to the user as a DBC proposal. Nothing is applied automatically; the user reviews and saves it. Use DBC start bit conventions as in analyze_id.","input_schema":{"type":"object","properties":{"messages":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"name":{"type":"string"},"length":{"type":"integer"},"signals":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"start_bit":{"type":"integer"},"length":{"type":"integer"},"byte_order":{"type":"string","enum":["intel","motorola"]},"signed":{"type":"boolean"},"factor":{"type":"number"},"offset":{"type":"number"},"min":{"type":"number"},"max":{"type":"number"},"unit":{"type":"string"},"comment":{"type":"string"},"multiplexer":{"type":"boolean","description":"this signal is the message's multiplexer"},"mux_value":{"type":"integer","description":"signal exists only when the multiplexer has this value"}},"required":["name","start_bit","length","byte_order"]}}},"required":["id","name","signals"]}}},"required":["messages"]}})";
    if (l.allowTransmit)
        t += R"(,
{"name":"send_frame","description":"Transmit one CAN frame on the bus to probe a reaction. The user must confirm every frame and may refuse; explain why you send it. Only when observing alone is not enough.","input_schema":{"type":"object","properties":{"bus":{"type":"integer"},"id":{"type":"string"},"extended":{"type":"boolean"},"data":{"type":"string","description":"hex bytes, e.g. \"01 02 FF\""},"reason":{"type":"string"}},"required":["bus","id","data","reason"]}})";
    t += "\n]";
    return t;
}

inline std::string systemPrompt()
{
    return "You are a CAN bus reverse engineering assistant inside SavvyCAN, a CAN analyzer. The user captured CAN traffic "
           "and wants to understand it: which IDs carry what, where counters, checksums and physical signals sit, what "
           "changes when they do something, and finally a DBC description.\n"
           "Work with the tools, they read the user's capture. Times are seconds from the first frame; IDs are hex; "
           "bit positions follow DBC (position = byte*8 + bit, Intel start = LSB, Motorola start = MSB). Start broad "
           "(capture_overview, list_ids), then analyze_id on interesting IDs, signal_values to check a hypothesis, "
           "compare_windows / changes_around when the user tells you when they did something. When an ID has a "
           "multiplexer, analyze each of its values separately, decode its signals with the same mux parameters in "
           "signal_values, and propose them with mux_value. Tool results are "
           "limited in size; ask for narrower ranges rather than everything. Say how sure you are; a hypothesis is "
           "not a fact until a signal behaves as expected. Offer findings with propose_dbc when they are solid. "
           "Transmitting frames can affect real equipment: only propose it when needed and explain why.";
}

/* Hex CAN ID, with or without 0x; returns false if not a valid ID. */
inline bool parseId(const std::string &text, uint32_t &id)
{
    std::string t = text;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty() || t.size() > 8) return false;
    uint32_t v = 0;
    for (char c : t)
    {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + uint32_t(d);
    }
    if (v > 0x1FFFFFFF) return false;
    id = v;
    return true;
}

/* The capture's key for an ID (and optional bus): prefers an exact bus match,
 * then the ID on any bus; extended if the ID was seen as extended. */
inline bool findKey(const std::vector<canre::IdInfo> &ids, uint32_t id, int bus, canre::FrameKey &out)
{
    for (const canre::IdInfo &i : ids)
        if (i.key.id == id && (bus < 0 || i.key.bus == bus)) { out = i.key; return true; }
    return false;
}

inline std::string cut(const std::string &s, const Limits &l)
{
    if (int(s.size()) <= l.maxResultChars) return s;
    return s.substr(0, size_t(l.maxResultChars)) + "\n[cut at " + std::to_string(l.maxResultChars) +
           " characters by the user's limit; ask for a narrower range]";
}

struct Result {
    std::string text;
    bool        error = false;
};

/* Runs a data tool. propose_dbc and send_frame need the user interface and
 * are handled by the window; for them this returns an error. */
inline Result run(const std::string &name, const Args &a, const canre::Capture &cap, const Limits &l)
{
    char line[256];
    std::vector<canre::IdInfo> ids = canre::listIds(cap);
    Result r;
    auto keyFor = [&](canre::FrameKey &k) -> bool {
        uint32_t id;
        if (!parseId(a.s("id"), id)) { r = { "invalid id '" + a.s("id") + "', expected hex such as 1A3", true }; return false; }
        if (!findKey(ids, id, int(a.n("bus", -1)), k)) { r = { "ID " + a.s("id") + " is not in the capture", true }; return false; }
        return true;
    };
    double t0 = a.n("start_s", -1), t1 = a.n("end_s", -1);

    if (name == "capture_overview")
    {
        std::set<int> buses;
        for (const canre::IdInfo &i : ids) buses.insert(i.key.bus);
        std::string b;
        for (int x : buses) b += (b.empty() ? "" : ", ") + std::to_string(x);
        snprintf(line, sizeof(line), "%zu frames over %.3f s, %zu distinct IDs, buses: %s\n", cap.size(), cap.spanSeconds(), ids.size(),
                 b.empty() ? "-" : b.c_str());
        r.text = line;
    }
    else if (name == "list_ids")
    {
        int bus = int(a.n("bus", -1));
        std::string sort = a.s("sort", "id");
        std::vector<canre::IdInfo> v;
        for (const canre::IdInfo &i : ids) if (bus < 0 || i.key.bus == bus) v.push_back(i);
        if (sort == "count") std::stable_sort(v.begin(), v.end(), [](const canre::IdInfo &x, const canre::IdInfo &y) { return x.count > y.count; });
        if (sort == "changing") std::stable_sort(v.begin(), v.end(), [](const canre::IdInfo &x, const canre::IdInfo &y) { return x.changingBytes > y.changingBytes; });
        size_t limit = size_t(std::max(1.0, a.n("limit", 100)));
        r.text = "id        bus  count  period_ms  jitter_ms  len  changing_bytes  distinct_payloads\n";
        for (size_t i = 0; i < v.size() && i < limit; i++)
        {
            std::string lens;
            for (uint8_t x : v[i].lengths) lens += (lens.empty() ? "" : "/") + std::to_string(x);
            snprintf(line, sizeof(line), "%-9s %3d %6zu %10.1f %10.1f  %-4s %14d %18d\n", canre::keyText(v[i].key).c_str(), v[i].key.bus,
                     v[i].count, v[i].periodMs, v[i].jitterMs, lens.c_str(), v[i].changingBytes, v[i].uniquePayloads);
            r.text += line;
        }
        if (v.size() > limit) r.text += std::to_string(v.size() - limit) + " more IDs not listed\n";
    }
    else if (name == "analyze_id")
    {
        canre::FrameKey k;
        if (!keyFor(k)) return r;
        int ms = int(a.n("mux_start", -1)), ml = int(a.n("mux_length", 0));
        long long mv = (long long)a.n("mux_value", -1);
        if ((ms >= 0) != (mv >= 0) || (ms >= 0 && (ml < 1 || ml > 32)))
            return { "give mux_start, mux_length (1..32) and mux_value together", true };
        r.text = canre::describe(canre::analyzeId(cap, k, t0, t1, ms, ml, mv));
    }
    else if (name == "get_frames")
    {
        canre::FrameKey k;
        if (!keyFor(k)) return r;
        std::vector<size_t> idx = cap.indices(k, t0, t1);
        size_t limit = size_t(std::min<double>(a.n("limit", 50), l.maxFramesPerResult));
        snprintf(line, sizeof(line), "%zu frames of %s in range, showing %zu\n", idx.size(), canre::keyText(k).c_str(), std::min(limit, idx.size()));
        r.text = line;
        for (size_t j = 0; j < idx.size() && j < limit; j++)
        {
            snprintf(line, sizeof(line), "%10.4f ", cap.seconds(idx[j]));
            r.text += line;
            const uint8_t *d = cap.data(idx[j]);
            for (int b = 0; b < cap.frame(idx[j]).len; b++)
            {
                snprintf(line, sizeof(line), "%02X%s", d[b], b + 1 < cap.frame(idx[j]).len ? " " : "");
                r.text += line;
            }
            r.text += "\n";
        }
    }
    else if (name == "signal_values")
    {
        canre::FrameKey k;
        if (!keyFor(k)) return r;
        int start = int(a.n("start_bit", 0)), len = int(a.n("length", 8));
        bool motorola = a.s("byte_order", "intel") == "motorola", sgn = a.b("signed", false);
        double factor = a.n("factor", 1), offset = a.n("offset", 0);
        if (len < 1 || len > 64) return { "length must be 1..64", true };
        int muxStart = int(a.n("mux_start", -1)), muxLen = int(a.n("mux_length", 0));
        int64_t muxValue = int64_t(a.n("mux_value", -1));
        bool muxed = a.has("mux_value");
        if (muxed && (muxStart < 0 || muxLen < 1 || muxLen > 32))
            return { "mux_value needs mux_start and mux_length (1..32) from analyze_id", true };
        std::vector<size_t> idx = cap.indices(k, t0, t1);
        std::vector<std::pair<double, double>> pts;
        for (size_t i : idx)
        {
            bool ok;
            if (muxed && (int64_t(canre::extractBits(cap.data(i), cap.frame(i).len, muxStart, muxLen, false, &ok)) != muxValue || !ok))
                continue;
            uint64_t raw = canre::extractBits(cap.data(i), cap.frame(i).len, start, len, motorola, &ok);
            if (!ok) continue;
            double v;
            if (sgn && len < 64 && (raw & (uint64_t(1) << (len - 1)))) v = double(int64_t(raw) - int64_t(uint64_t(1) << len));
            else v = double(raw);
            pts.push_back({ cap.seconds(i), v * factor + offset });
        }
        if (pts.empty()) return { "no frame of " + canre::keyText(k) + " holds that bit field in the range", true };
        double mn = pts[0].second, mx = mn, sum = 0;
        size_t changes = 0;
        std::set<double> distinct;
        for (size_t i = 0; i < pts.size(); i++)
        {
            mn = std::min(mn, pts[i].second); mx = std::max(mx, pts[i].second); sum += pts[i].second;
            if (i && pts[i].second != pts[i - 1].second) changes++;
            if (distinct.size() < 1000) distinct.insert(pts[i].second);
        }
        snprintf(line, sizeof(line), "%zu values, min %g max %g mean %g, %zu changes, %s%zu distinct\n", pts.size(), mn, mx, sum / pts.size(),
                 changes, distinct.size() >= 1000 ? ">= " : "", distinct.size());
        r.text = line;
        size_t maxp = size_t(std::max(2.0, std::min<double>(a.n("max_points", 60), l.maxFramesPerResult)));
        size_t step = std::max<size_t>(1, (pts.size() + maxp - 1) / maxp);
        r.text += "time_s value\n";
        for (size_t i = 0; i < pts.size(); i += step)
        {
            snprintf(line, sizeof(line), "%.3f %g\n", pts[i].first, pts[i].second);
            r.text += line;
        }
    }
    else if (name == "compare_windows" || name == "changes_around")
    {
        double a0, a1, b0, b1;
        if (name == "changes_around")
        {
            double t = a.n("time_s", 0), before = a.n("before_s", 2), after = a.n("after_s", 2);
            a0 = std::max(0.0, t - before); a1 = t; b0 = t; b1 = t + after;
        }
        else
        {
            a0 = a.n("a_start", 0); a1 = a.n("a_end", 0); b0 = a.n("b_start", 0); b1 = a.n("b_end", 0);
        }
        if (a1 <= a0 || b1 <= b0) return { "each window needs start < end", true };
        snprintf(line, sizeof(line), "window A %.3f..%.3f s, window B %.3f..%.3f s\n", a0, a1, b0, b1);
        r.text = line + canre::describe(canre::diffWindows(cap, a0, a1, b0, b1, size_t(std::max(1.0, a.n("max_bits", 30)))));
    }
    else
        return { "unknown tool " + name, true };
    r.text = cut(r.text, l);
    return r;
}

} // namespace aitools

#endif // AITOOLS_H
