// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANRE_H
#define CANRE_H

/*
 * CAN reverse engineering analysis, header only and Qt free so it is unit
 * tested on the host (tests/gui/canre_test.cpp). Used by the Signal Finder
 * window and as the tools of the AI assistant.
 *
 *   canre::Capture cap;  cap.add(us, bus, id, ext, len, data); ...
 *   auto ids = canre::listIds(cap);
 *   canre::IdAnalysis a = canre::analyzeId(cap, key);   // counters, checksums, signals
 *   auto d = canre::diffWindows(cap, a0, a1, b0, b1);   // what changed between two time ranges
 *   std::string dbc = canre::toDbc(messages);
 *
 * Bit numbering: "bit k of byte b" is the usual (k = 0 is the LSB). Signal
 * positions use DBC conventions: an Intel (little endian) signal starts at
 * its LSB, a Motorola (big endian) signal at its MSB, both as b * 8 + k.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace canre {

/* ------------------------------------------------------------------ capture */

struct FrameKey {
    uint8_t  bus = 0;
    bool     ext = false;
    uint32_t id = 0;
    bool operator<(const FrameKey &o) const
    {
        if (bus != o.bus) return bus < o.bus;
        if (ext != o.ext) return ext < o.ext;
        return id < o.id;
    }
    bool operator==(const FrameKey &o) const { return bus == o.bus && ext == o.ext && id == o.id; }
};

inline std::string keyText(const FrameKey &k)
{
    char b[32];
    snprintf(b, sizeof(b), k.ext ? "%08X" : "%03X", k.id);
    return std::string(b) + (k.bus ? "@" + std::to_string(k.bus) : std::string());
}

/* Compact copy of a frame list: 24 bytes per frame plus its data. */
class Capture {
public:
    struct Frame {
        uint64_t us;
        uint32_t id;
        uint32_t off;
        uint8_t  bus, len;
        bool     ext;
    };

    void add(uint64_t us, uint8_t bus, uint32_t id, bool ext, uint8_t len, const uint8_t *data)
    {
        if (len > 64) len = 64;
        Frame f{ us, id, uint32_t(pool.size()), bus, len, ext };
        pool.insert(pool.end(), data, data + len);
        frames.push_back(f);
    }
    size_t size() const { return frames.size(); }
    const Frame &frame(size_t i) const { return frames[i]; }
    const uint8_t *data(size_t i) const { return pool.data() + frames[i].off; }
    FrameKey key(size_t i) const { FrameKey k; k.bus = frames[i].bus; k.ext = frames[i].ext; k.id = frames[i].id; return k; }
    uint64_t startUs() const { return frames.empty() ? 0 : frames.front().us; }
    double seconds(size_t i) const { return (frames[i].us - startUs()) / 1e6; }
    double spanSeconds() const { return frames.empty() ? 0 : (frames.back().us - startUs()) / 1e6; }

    /* frame indices of one ID, optionally limited to [t0, t1] seconds */
    std::vector<size_t> indices(const FrameKey &k, double t0 = -1, double t1 = -1) const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < frames.size(); i++)
        {
            if (!(key(i) == k)) continue;
            double t = seconds(i);
            if (t0 >= 0 && t < t0) continue;
            if (t1 >= 0 && t > t1) continue;
            out.push_back(i);
        }
        return out;
    }

private:
    std::vector<Frame> frames;
    std::vector<uint8_t> pool;
};

/* ------------------------------------------------------------ ID overview */

struct IdInfo {
    FrameKey key;
    size_t   count = 0;
    double   firstS = 0, lastS = 0;
    double   periodMs = 0;        /* mean interval */
    double   jitterMs = 0;        /* standard deviation of the interval */
    std::set<uint8_t> lengths;
    int      changingBytes = 0;   /* bytes that took more than one value */
    int      uniquePayloads = 0;  /* capped at 1000 */
};

inline std::vector<IdInfo> listIds(const Capture &c)
{
    struct Acc {
        IdInfo info;
        double lastT = -1, sum = 0, sum2 = 0;
        size_t intervals = 0;
        uint8_t first[64]; uint8_t firstLen = 0; bool haveFirst = false;
        uint64_t changedMask = 0;
        std::set<std::string> payloads;
    };
    std::map<FrameKey, Acc> m;
    for (size_t i = 0; i < c.size(); i++)
    {
        Acc &a = m[c.key(i)];
        const Capture::Frame &f = c.frame(i);
        double t = c.seconds(i);
        if (a.info.count == 0) { a.info.key = c.key(i); a.info.firstS = t; }
        a.info.count++;
        a.info.lastS = t;
        a.info.lengths.insert(f.len);
        if (a.lastT >= 0)
        {
            double d = (t - a.lastT) * 1000.0;
            a.sum += d; a.sum2 += d * d; a.intervals++;
        }
        a.lastT = t;
        const uint8_t *d = c.data(i);
        if (!a.haveFirst) { memcpy(a.first, d, f.len); a.firstLen = f.len; a.haveFirst = true; }
        for (int b = 0; b < f.len && b < 64; b++)
            if (b >= a.firstLen || d[b] != a.first[b])
                a.changedMask |= uint64_t(1) << b;
        if (a.payloads.size() < 1000)
            a.payloads.insert(std::string(reinterpret_cast<const char *>(d), f.len));
    }
    std::vector<IdInfo> out;
    for (auto &kv : m)
    {
        Acc &a = kv.second;
        if (a.intervals)
        {
            a.info.periodMs = a.sum / double(a.intervals);
            double var = a.sum2 / double(a.intervals) - a.info.periodMs * a.info.periodMs;
            a.info.jitterMs = var > 0 ? std::sqrt(var) : 0;
        }
        int n = 0;
        for (int b = 0; b < 64; b++) if (a.changedMask & (uint64_t(1) << b)) n++;
        a.info.changingBytes = n;
        a.info.uniquePayloads = int(a.payloads.size());
        out.push_back(a.info);
    }
    return out;
}

/* --------------------------------------------------------- per ID analysis */

struct ByteStat {
    int    minV = 255, maxV = 0, unique = 0, changes = 0;
    double entropy = 0;           /* bits */
};

enum class FieldKind { Constant, Counter, Checksum, Signal, Flag, Multiplexer };

inline const char *kindName(FieldKind k)
{
    switch (k)
    {
    case FieldKind::Constant: return "constant";
    case FieldKind::Counter:  return "counter";
    case FieldKind::Checksum: return "checksum";
    case FieldKind::Flag:     return "flag";
    case FieldKind::Multiplexer: return "multiplexer";
    default:                  return "signal";
    }
}

struct Field {
    FieldKind   kind = FieldKind::Signal;
    int         startBit = 0;     /* DBC start bit (Intel: LSB, Motorola: MSB) */
    int         length = 1;
    bool        bigEndian = false;
    bool        isSigned = false;
    uint64_t    minRaw = 0, maxRaw = 0;
    int         unique = 0;
    std::string note;             /* e.g. "+1 per frame, wraps at 15", "XOR of bytes 0..6" */
};

struct IdAnalysis {
    FrameKey key;
    size_t   frames = 0;
    int      length = 0;          /* most common length */
    std::vector<ByteStat> bytes;
    std::vector<double>   flipRate;   /* per bit, index b * 8 + k, flips / (frames - 1) */
    std::vector<double>   oneRate;    /* fraction of frames with the bit set */
    std::vector<Field>    fields;
    /* multiplexer found (whole capture analysis): Intel bit field whose value
     * selects the layout of the other bytes; analyze each value separately */
    int      muxStart = -1, muxLength = 0;
    std::vector<uint64_t> muxValues;
    /* set when the analysis was limited to one multiplexer value */
    int64_t  muxFilter = -1;
};

/* Raw value of a bit field. Intel: start = LSB position, bits go up.
 * Motorola: start = MSB position, then down within the byte and on to the
 * next byte's bit 7 (DBC convention). */
inline uint64_t extractBits(const uint8_t *d, int len, int start, int length, bool bigEndian, bool *ok = nullptr)
{
    uint64_t v = 0;
    if (ok) *ok = true;
    if (length < 1 || length > 64) { if (ok) *ok = false; return 0; }
    if (!bigEndian)
    {
        for (int i = 0; i < length; i++)
        {
            int pos = start + i;
            if (pos / 8 >= len) { if (ok) *ok = false; return 0; }
            if (d[pos / 8] & (1u << (pos % 8))) v |= uint64_t(1) << i;
        }
        return v;
    }
    int pos = start;
    for (int i = 0; i < length; i++)
    {
        if (pos / 8 >= len || pos < 0) { if (ok) *ok = false; return 0; }
        v = (v << 1) | ((d[pos / 8] >> (pos % 8)) & 1u);
        if (pos % 8 == 0) pos += 15;     /* next byte, bit 7 */
        else pos--;
    }
    return v;
}

/* Motorola start bit (MSB) of a field whose LSB is at lsbPos and that is
 * `length` bits long. */
inline int motorolaStartFromLsb(int lsbPos, int length)
{
    int pos = lsbPos;
    for (int i = 1; i < length; i++)
    {
        if (pos % 8 == 7) pos -= 15;     /* previous byte, bit 0 */
        else pos++;
    }
    return pos;
}

namespace detail {

inline uint8_t crc8(const uint8_t *p, int n, uint8_t poly, uint8_t init, uint8_t xorOut)
{
    uint8_t c = init;
    for (int i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? uint8_t((c << 1) ^ poly) : uint8_t(c << 1);
    }
    return uint8_t(c ^ xorOut);
}

struct Series {
    std::vector<const uint8_t *> d;
    int len = 0;
};

/* fraction of steps where (v[i] - v[i-1]) mod 2^bits == step */
inline double counterScore(const std::vector<uint64_t> &v, int bits, uint64_t &stepOut)
{
    if (v.size() < 8) return 0;
    uint64_t mod = uint64_t(1) << bits;
    std::map<uint64_t, size_t> steps;
    for (size_t i = 1; i < v.size(); i++)
        steps[(v[i] - v[i - 1]) & (mod - 1)]++;
    auto best = std::max_element(steps.begin(), steps.end(),
                                 [](const std::pair<const uint64_t, size_t> &a, const std::pair<const uint64_t, size_t> &b) { return a.second < b.second; });
    stepOut = best->first;
    if (stepOut == 0) return 0;
    return double(best->second) / double(v.size() - 1);
}

} // namespace detail

/* muxStart/muxLength/muxValue limit the analysis to the frames whose
 * multiplexer field (Intel bit field) has that value. */
inline IdAnalysis analyzeId(const Capture &c, const FrameKey &key, double t0 = -1, double t1 = -1,
                            int muxStart = -1, int muxLength = 0, int64_t muxValue = -1)
{
    IdAnalysis a;
    a.key = key;
    std::vector<size_t> idx = c.indices(key, t0, t1);
    if (muxStart >= 0 && muxLength > 0 && muxValue >= 0)
    {
        std::vector<size_t> keep;
        for (size_t i : idx)
        {
            bool ok;
            uint64_t v = extractBits(c.data(i), c.frame(i).len, muxStart, muxLength, false, &ok);
            if (ok && v == uint64_t(muxValue)) keep.push_back(i);
        }
        idx.swap(keep);
        a.muxFilter = muxValue;
        a.muxStart = muxStart;
        a.muxLength = muxLength;
    }
    std::map<int, size_t> lens;
    for (size_t i : idx) lens[c.frame(i).len]++;
    if (idx.empty()) return a;
    a.length = std::max_element(lens.begin(), lens.end(),
                                [](const std::pair<const int, size_t> &x, const std::pair<const int, size_t> &y) { return x.second < y.second; })->first;
    detail::Series s;
    s.len = a.length;
    for (size_t i : idx)
        if (c.frame(i).len == a.length)
            s.d.push_back(c.data(i));
    a.frames = s.d.size();
    const int L = a.length, n = int(s.d.size());

    /* byte and bit statistics */
    a.bytes.resize(size_t(L));
    a.flipRate.assign(size_t(L * 8), 0.0);
    a.oneRate.assign(size_t(L * 8), 0.0);
    for (int b = 0; b < L; b++)
    {
        size_t hist[256] = { 0 };
        ByteStat &st = a.bytes[size_t(b)];
        for (int i = 0; i < n; i++)
        {
            uint8_t v = s.d[size_t(i)][b];
            hist[v]++;
            st.minV = std::min(st.minV, int(v));
            st.maxV = std::max(st.maxV, int(v));
            if (i && v != s.d[size_t(i - 1)][b]) st.changes++;
            for (int k = 0; k < 8; k++)
            {
                if (v & (1u << k)) a.oneRate[size_t(b * 8 + k)] += 1;
                if (i && ((v ^ s.d[size_t(i - 1)][b]) & (1u << k))) a.flipRate[size_t(b * 8 + k)] += 1;
            }
        }
        for (int v = 0; v < 256; v++)
            if (hist[v])
            {
                st.unique++;
                double p = double(hist[v]) / n;
                st.entropy -= p * std::log2(p);
            }
    }
    for (size_t i = 0; i < a.flipRate.size(); i++)
    {
        a.flipRate[i] = n > 1 ? a.flipRate[i] / (n - 1) : 0;
        a.oneRate[i] /= n;
    }

    std::vector<bool> used(size_t(L * 8), false);
    auto markUsed = [&](int lsbPos, int length) {   /* Intel style bit range */
        for (int i = 0; i < length; i++) if (lsbPos + i < L * 8) used[size_t(lsbPos + i)] = true;
    };
    auto rangeFree = [&](int lsbPos, int length) {
        for (int i = 0; i < length; i++) if (lsbPos + i >= L * 8 || used[size_t(lsbPos + i)]) return false;
        return true;
    };

    /* checksums: a byte equal to f(other bytes) [+ constant] in >= 98 % of
     * frames. Before the counters, whose 2-3 bit candidates can match bits of
     * a checksum byte by chance; a counter byte is skipped instead, because a
     * symmetric XOR or SUM relation also holds for it. */
    auto counterLike = [&](int b) {
        const int cand[][2] = { { 0, 8 }, { 0, 4 }, { 4, 4 } };
        for (const auto &cd : cand)
        {
            std::vector<uint64_t> v;
            for (int i = 0; i < n; i++) v.push_back(extractBits(s.d[size_t(i)], L, b * 8 + cd[0], cd[1], false));
            uint64_t step;
            if (detail::counterScore(v, cd[1], step) >= 0.9 && step == 1) return true;
        }
        return false;
    };
    bool checksumFound = false;
    if (n >= 8 && L >= 2)
        for (int cb = L - 1; cb >= 0 && !checksumFound; cb--)       /* usually the last byte; an XOR relation */
        {                                                            /* would match every byte in it, so stop at one */
            if (a.bytes[size_t(cb)].unique < 3 || !rangeFree(cb * 8, 8) || counterLike(cb)) continue;
            struct Algo { const char *name; int type; };
            const Algo algos[] = { { "XOR", 0 }, { "SUM", 1 }, { "CRC-8 SAE J1850", 2 }, { "CRC-8 AUTOSAR (0x2F)", 3 } };
            for (const Algo &al : algos)
            {
                std::map<int, int> offsets;
                for (int i = 0; i < n; i++)
                {
                    uint8_t other[64]; int m = 0;
                    for (int b = 0; b < L; b++) if (b != cb) other[m++] = s.d[size_t(i)][b];
                    uint8_t f = 0;
                    if (al.type == 0) for (int b = 0; b < m; b++) f ^= other[b];
                    else if (al.type == 1) for (int b = 0; b < m; b++) f = uint8_t(f + other[b]);
                    else if (al.type == 2) f = detail::crc8(other, m, 0x1D, 0xFF, 0xFF);
                    else f = detail::crc8(other, m, 0x2F, 0xFF, 0xFF);
                    int off = al.type <= 1 ? (al.type == 0 ? (s.d[size_t(i)][cb] ^ f) : uint8_t(s.d[size_t(i)][cb] - f))
                                           : (s.d[size_t(i)][cb] ^ f);
                    offsets[off]++;
                }
                auto best = std::max_element(offsets.begin(), offsets.end(),
                                             [](const std::pair<const int, int> &x, const std::pair<const int, int> &y) { return x.second < y.second; });
                if (best->second >= int(0.98 * n))
                {
                    Field f;
                    f.kind = FieldKind::Checksum;
                    f.startBit = cb * 8; f.length = 8; f.bigEndian = false;
                    f.minRaw = uint64_t(a.bytes[size_t(cb)].minV); f.maxRaw = uint64_t(a.bytes[size_t(cb)].maxV);
                    f.unique = a.bytes[size_t(cb)].unique;
                    char note[96];
                    if (best->first)
                        snprintf(note, sizeof(note), "%s of the other bytes %s 0x%02X", al.name, al.type == 1 ? "+" : "^", best->first);
                    else
                        snprintf(note, sizeof(note), "%s of the other bytes", al.name);
                    f.note = note;
                    a.fields.push_back(f);
                    markUsed(cb * 8, 8);
                    checksumFound = true;
                    break;
                }
            }
        }

    /* counters: byte, nibbles and 2..3 bit fields inside a byte, Intel order,
     * stepping by +1 (mod 2^bits) in at least 90 % of frames. The best
     * scoring field wins, the wider one on a tie (the low nibble of a byte
     * counter counts too); a byte may hold more than one counter. */
    if (n >= 8)
        for (int b = 0; b < L; b++)
            for (;;)
            {
                const int cand[][2] = { { 0, 8 }, { 0, 4 }, { 4, 4 }, { 0, 3 }, { 4, 3 }, { 0, 2 }, { 2, 2 }, { 4, 2 }, { 6, 2 } };
                double bestScore = 0;
                int bestLsb = -1, bestLen = 0;
                for (const auto &cd : cand)
                {
                    int lsb = b * 8 + cd[0], len = cd[1];
                    if (!rangeFree(lsb, len)) continue;
                    std::vector<uint64_t> v;
                    for (int i = 0; i < n; i++) v.push_back(extractBits(s.d[size_t(i)], L, lsb, len, false));
                    uint64_t step;
                    double sc = detail::counterScore(v, len, step);
                    std::set<uint64_t> distinct(v.begin(), v.end());
                    if (step != 1 || sc < 0.9 || distinct.size() < std::min<size_t>(4, size_t(1) << len)) continue;
                    /* the low bits of a ramp (a value growing by a constant
                     * other than 1, e.g. +5) also count +1: if a wider field
                     * from the same bit steps by a constant, it is a signal */
                    bool ramp = false;
                    for (int w = len + 1; w <= 16 && lsb + w <= L * 8 && !ramp; w++)
                    {
                        std::vector<uint64_t> vw;
                        for (int i = 0; i < n; i++) vw.push_back(extractBits(s.d[size_t(i)], L, lsb, w, false));
                        uint64_t stepW;
                        ramp = detail::counterScore(vw, w, stepW) >= 0.9 && stepW != 1;
                    }
                    if (ramp) continue;
                    if (sc > bestScore + 1e-9 || (std::fabs(sc - bestScore) <= 1e-9 && len > bestLen))
                    {
                        bestScore = sc; bestLsb = lsb; bestLen = len;
                    }
                }
                if (bestLsb < 0) break;
                Field f;
                f.kind = FieldKind::Counter;
                f.startBit = bestLsb; f.length = bestLen;
                std::set<uint64_t> distinct;
                for (int i = 0; i < n; i++) distinct.insert(extractBits(s.d[size_t(i)], L, bestLsb, bestLen, false));
                f.minRaw = *distinct.begin(); f.maxRaw = *distinct.rbegin(); f.unique = int(distinct.size());
                char note[96];
                snprintf(note, sizeof(note), "+1 per frame in %.0f %% of frames, %d bits", bestScore * 100, bestLen);
                f.note = note;
                a.fields.push_back(f);
                markUsed(bestLsb, bestLen);
            }

    /* multiplexer: a byte or nibble with 2..32 values that changes often,
     * while the other changing bytes rarely change between frames with the
     * same value of it (each value selects a page with its own layout).
     * Ratio of the change rates "same value" / "consecutive" below 0.5. */
    if (n >= 16 && a.muxFilter < 0)
    {
        /* a page selector that cycles 0, 1, 2, ... was taken for a counter
         * above, so counter fields are candidates too; checksum bytes depend
         * on everything and are no evidence */
        std::set<int> checksumBytes;
        for (const Field &f : a.fields) if (f.kind == FieldKind::Checksum) checksumBytes.insert(f.startBit / 8);
        auto candidateFree = [&](int lsb, int len) {
            for (int i = 0; i < len; i++)
            {
                int pos = lsb + i;
                if (pos >= L * 8) return false;
                if (!used[size_t(pos)]) continue;
                bool inCounter = false;
                for (const Field &f : a.fields)
                    if (f.kind == FieldKind::Counter && pos >= f.startBit && pos < f.startBit + f.length) inCounter = true;
                if (!inCounter) return false;
            }
            return true;
        };
        double bestRatio = 0.5;
        int bestLsb = -1, bestLen = 0;
        for (int b = 0; b < L; b++)
        {
            const int cand[][2] = { { 0, 8 }, { 0, 4 }, { 4, 4 } };
            for (const auto &cd : cand)
            {
                int lsb = b * 8 + cd[0], len = cd[1];
                if (!candidateFree(lsb, len)) continue;
                std::vector<uint64_t> v(size_t(n), 0);
                std::set<uint64_t> distinct;
                int changes = 0;
                for (int i = 0; i < n; i++)
                {
                    v[size_t(i)] = extractBits(s.d[size_t(i)], L, lsb, len, false);
                    distinct.insert(v[size_t(i)]);
                    if (i && v[size_t(i)] != v[size_t(i - 1)]) changes++;
                }
                if (distinct.size() < 2 || distinct.size() > 32 || changes < 0.3 * (n - 1)) continue;
                double sumU = 0, sumC = 0;
                int used_ = 0;
                for (int o = 0; o < L; o++)
                {
                    if (o == b || checksumBytes.count(o)) continue;
                    int cu = 0;
                    for (int i = 1; i < n; i++) if (s.d[size_t(i)][o] != s.d[size_t(i - 1)][o]) cu++;
                    double ru = double(cu) / (n - 1);
                    if (ru < 0.2) continue;                   /* byte barely changes anyway */
                    std::map<uint64_t, int> last;
                    int pairs = 0, cc = 0;
                    for (int i = 0; i < n; i++)
                    {
                        auto it = last.find(v[size_t(i)]);
                        if (it != last.end())
                        {
                            pairs++;
                            if (s.d[size_t(i)][o] != s.d[size_t(it->second)][o]) cc++;
                        }
                        last[v[size_t(i)]] = i;
                    }
                    if (!pairs) continue;
                    sumU += ru;
                    sumC += double(cc) / pairs;
                    used_++;
                }
                if (!used_) continue;
                double ratio = sumC / sumU;
                if (ratio < bestRatio - 1e-9)
                {
                    bestRatio = ratio; bestLsb = lsb; bestLen = len;
                }
            }
        }
        if (bestLsb >= 0)
        {
            std::set<uint64_t> distinct;
            for (int i = 0; i < n; i++) distinct.insert(extractBits(s.d[size_t(i)], L, bestLsb, bestLen, false));
            /* a counter field inside it was the selector */
            a.fields.erase(std::remove_if(a.fields.begin(), a.fields.end(), [&](const Field &x) {
                               return x.kind == FieldKind::Counter && x.startBit < bestLsb + bestLen && bestLsb < x.startBit + x.length;
                           }), a.fields.end());
            Field f;
            f.kind = FieldKind::Multiplexer;
            f.startBit = bestLsb; f.length = bestLen;
            f.minRaw = *distinct.begin(); f.maxRaw = *distinct.rbegin(); f.unique = int(distinct.size());
            std::string vals;
            for (uint64_t x : distinct) vals += (vals.empty() ? "" : " ") + std::to_string(x);
            char note[160];
            snprintf(note, sizeof(note), "selects the layout of the other bytes (change ratio %.2f); values %s; analyze each value separately",
                     bestRatio, vals.c_str());
            f.note = note;
            a.fields.push_back(f);
            markUsed(bestLsb, bestLen);
            a.muxStart = bestLsb;
            a.muxLength = bestLen;
            a.muxValues.assign(distinct.begin(), distinct.end());
        }
    }

    /* remaining bits: flags and signal segments. Segmentation after READ
     * (Marchetti, Stabili 2018): inside a signal the bit flip rate rises from
     * the MSB to the LSB, so a boundary sits where it drops by an order of
     * magnitude. The walk is done twice, MSB first across the bytes in
     * Motorola order (byte 0 first) and in Intel order (last byte first);
     * a multi-byte field is taken from whichever order makes it longer. */
    auto mag = [&](int pos) { double r = a.flipRate[size_t(pos)]; return r <= 0 ? -99 : int(std::floor(std::log10(r))); };
    auto segment = [&](bool intel) {
        std::vector<std::vector<int>> segs;
        std::vector<int> cur;
        for (int bi = 0; bi < L; bi++)
        {
            int b = intel ? L - 1 - bi : bi;
            for (int k = 7; k >= 0; k--)
            {
                int pos = b * 8 + k;
                if (used[size_t(pos)] || a.flipRate[size_t(pos)] == 0)
                {
                    if (!cur.empty()) { segs.push_back(cur); cur.clear(); }
                    continue;
                }
                if (!cur.empty() && mag(pos) < mag(cur.back()))
                {
                    segs.push_back(cur);
                    cur.clear();
                }
                cur.push_back(pos);
            }
        }
        if (!cur.empty()) segs.push_back(cur);
        return segs;
    };
    struct Seg { std::vector<int> bits; bool intel; };
    std::vector<Seg> multi;
    for (int o = 0; o < 2; o++)
        for (const std::vector<int> &sg : segment(o == 1))
            if (sg.front() / 8 != sg.back() / 8)
                multi.push_back({ sg, o == 1 });
    std::sort(multi.begin(), multi.end(), [](const Seg &x, const Seg &y) { return x.bits.size() > y.bits.size(); });

    auto addField = [&](const std::vector<int> &bits, bool intel) {
        Field f;
        f.length = int(bits.size());
        /* bits are listed MSB first; for one byte both orders read the same */
        int msb = bits.front(), lsb = bits.back();
        bool multiByte = msb / 8 != lsb / 8;
        f.bigEndian = multiByte && !intel;
        f.startBit = f.bigEndian ? msb : lsb;
        std::set<uint64_t> distinct;
        uint64_t mn = UINT64_MAX, mx = 0;
        for (int i = 0; i < n; i++)
        {
            uint64_t v = extractBits(s.d[size_t(i)], L, f.startBit, f.length, f.bigEndian);
            if (distinct.size() < 100000) distinct.insert(v);
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        f.minRaw = mn; f.maxRaw = mx; f.unique = int(distinct.size());
        f.kind = f.length == 1 ? FieldKind::Flag : FieldKind::Signal;
        if (multiByte) f.note = intel ? "Intel byte order (low byte first)" : "Motorola byte order (high byte first)";
        a.fields.push_back(f);
        for (int p : bits) used[size_t(p)] = true;
    };
    for (const Seg &sg : multi)
    {
        bool free_ = true;
        for (int p : sg.bits) if (used[size_t(p)]) { free_ = false; break; }
        if (free_) addField(sg.bits, sg.intel);
    }
    for (const std::vector<int> &sg : segment(false))     /* what is left lies within single bytes */
        addField(sg, false);

    /* constant bytes, for completeness */
    for (int b = 0; b < L; b++)
        if (a.bytes[size_t(b)].unique == 1)
        {
            Field f;
            f.kind = FieldKind::Constant;
            f.startBit = b * 8; f.length = 8;
            f.minRaw = f.maxRaw = uint64_t(a.bytes[size_t(b)].minV); f.unique = 1;
            a.fields.push_back(f);
        }

    std::sort(a.fields.begin(), a.fields.end(), [](const Field &x, const Field &y) {
        auto first = [](const Field &f) { return f.bigEndian ? (f.startBit / 8) * 8 + (7 - f.startBit % 8) : (f.startBit / 8) * 8 + 7 - (f.startBit + f.length - 1) % 8; };
        return first(x) < first(y);
    });
    return a;
}

/* Human/AI readable summary of an analysis. */
inline std::string describe(const IdAnalysis &a)
{
    std::string out;
    char line[256];
    snprintf(line, sizeof(line), "ID %s: %zu frames of %d bytes\n", keyText(a.key).c_str(), a.frames, a.length);
    out += line;
    if (a.muxFilter >= 0)
    {
        snprintf(line, sizeof(line), "only frames with the multiplexer at bit %d, %d bits = %lld\n", a.muxStart, a.muxLength,
                 static_cast<long long>(a.muxFilter));
        out += line;
    }
    out += "byte  min  max  uniq  changes  entropy  flip rate per bit (bit7..bit0)\n";
    for (size_t b = 0; b < a.bytes.size(); b++)
    {
        const ByteStat &s = a.bytes[b];
        snprintf(line, sizeof(line), "%4zu  %02X   %02X   %4d  %7d  %7.2f  ", b, s.minV, s.maxV, s.unique, s.changes, s.entropy);
        out += line;
        for (int k = 7; k >= 0; k--)
        {
            snprintf(line, sizeof(line), "%.2f%s", a.flipRate[b * 8 + size_t(k)], k ? " " : "\n");
            out += line;
        }
    }
    out += "fields (DBC start bit; Intel = LSB, Motorola = MSB):\n";
    for (const Field &f : a.fields)
    {
        snprintf(line, sizeof(line), "  %-8s start %2d len %2d %s raw %llu..%llu, %d values%s%s\n", kindName(f.kind), f.startBit,
                 f.length, f.bigEndian ? "Motorola" : "Intel   ", static_cast<unsigned long long>(f.minRaw),
                 static_cast<unsigned long long>(f.maxRaw), f.unique, f.note.empty() ? "" : ", ", f.note.c_str());
        out += line;
    }
    return out;
}

/* ------------------------------------------------- what changed: two windows */

struct BitChange {
    FrameKey key;
    int      byte = 0, bit = 0;
    double   oneA = 0, oneB = 0;      /* fraction of frames with the bit set */
    double   flipA = 0, flipB = 0;
    double   score = 0;
};

struct WindowDiff {
    std::vector<FrameKey> onlyA, onlyB;
    std::vector<std::pair<FrameKey, std::pair<double, double>>> rateChanged;   /* frames/s A, B */
    std::vector<BitChange> bits;      /* strongest first */
};

inline WindowDiff diffWindows(const Capture &c, double a0, double a1, double b0, double b1, size_t maxBits = 50)
{
    struct Acc { size_t n = 0; std::vector<double> ones, flips; std::vector<uint8_t> prev; int len = 0; };
    std::map<FrameKey, Acc> A, B;
    for (size_t i = 0; i < c.size(); i++)
    {
        double t = c.seconds(i);
        Acc *acc = (t >= a0 && t <= a1) ? &A[c.key(i)] : (t >= b0 && t <= b1) ? &B[c.key(i)] : nullptr;
        if (!acc) continue;
        int len = c.frame(i).len;
        const uint8_t *d = c.data(i);
        if (acc->ones.size() < size_t(len * 8)) { acc->ones.resize(size_t(len * 8), 0); acc->flips.resize(size_t(len * 8), 0); }
        for (int p = 0; p < len * 8; p++)
        {
            bool v = d[p / 8] & (1u << (p % 8));
            if (v) acc->ones[size_t(p)] += 1;
            if (acc->n && p / 8 < acc->len && bool(acc->prev[size_t(p / 8)] & (1u << (p % 8))) != v) acc->flips[size_t(p)] += 1;
        }
        acc->prev.assign(d, d + len);
        acc->len = len;
        acc->n++;
    }
    WindowDiff w;
    for (auto &kv : A) if (!B.count(kv.first)) w.onlyA.push_back(kv.first);
    for (auto &kv : B) if (!A.count(kv.first)) w.onlyB.push_back(kv.first);
    double la = std::max(a1 - a0, 1e-6), lb = std::max(b1 - b0, 1e-6);
    for (auto &kv : A)
    {
        auto it = B.find(kv.first);
        if (it == B.end()) continue;
        double ra = kv.second.n / la, rb = it->second.n / lb;
        if (std::max(ra, rb) > 1 && std::fabs(ra - rb) > 0.3 * std::max(ra, rb))
            w.rateChanged.push_back({ kv.first, { ra, rb } });
        size_t bitsN = std::min(kv.second.ones.size(), it->second.ones.size());
        for (size_t p = 0; p < bitsN; p++)
        {
            BitChange bc;
            bc.key = kv.first; bc.byte = int(p / 8); bc.bit = int(p % 8);
            bc.oneA = kv.second.ones[p] / kv.second.n;
            bc.oneB = it->second.ones[p] / it->second.n;
            bc.flipA = kv.second.n > 1 ? kv.second.flips[p] / (kv.second.n - 1) : 0;
            bc.flipB = it->second.n > 1 ? it->second.flips[p] / (it->second.n - 1) : 0;
            /* a bit that is steady in both windows but at a different level is
             * the strongest hint (a switch, a state); a changed toggle rate next */
            double level = std::fabs(bc.oneA - bc.oneB);
            double steady = 1 - std::min(1.0, (bc.flipA + bc.flipB) * 5);
            bc.score = level * (0.5 + 0.5 * steady) + 0.25 * std::fabs(bc.flipA - bc.flipB);
            if (bc.score >= 0.2) w.bits.push_back(bc);
        }
    }
    std::sort(w.bits.begin(), w.bits.end(), [](const BitChange &x, const BitChange &y) { return x.score > y.score; });
    if (w.bits.size() > maxBits) w.bits.resize(maxBits);
    return w;
}

inline std::string describe(const WindowDiff &w)
{
    std::string out;
    char line[200];
    auto keys = [](const std::vector<FrameKey> &v) { std::string s; for (const FrameKey &k : v) s += (s.empty() ? "" : " ") + keyText(k); return s.empty() ? std::string("-") : s; };
    out += "IDs only in window A: " + keys(w.onlyA) + "\n";
    out += "IDs only in window B: " + keys(w.onlyB) + "\n";
    for (const auto &r : w.rateChanged)
    {
        snprintf(line, sizeof(line), "rate of %s: %.1f -> %.1f frames/s\n", keyText(r.first).c_str(), r.second.first, r.second.second);
        out += line;
    }
    out += "bits that behave differently (score, ID, byte.bit, set A -> B, flip rate A -> B):\n";
    for (const BitChange &b : w.bits)
    {
        snprintf(line, sizeof(line), "  %.2f  %s  %d.%d  %.2f -> %.2f  %.2f -> %.2f\n", b.score, keyText(b.key).c_str(), b.byte, b.bit,
                 b.oneA, b.oneB, b.flipA, b.flipB);
        out += line;
    }
    if (w.bits.empty()) out += "  none\n";
    return out;
}

/* ------------------------------------------------------------------ DBC */

struct DbcSignal {
    std::string name;
    int         startBit = 0, length = 8;
    bool        bigEndian = false, isSigned = false;
    int         muxRole = 0;      /* 0 plain, 1 the multiplexer (M), 2 multiplexed (m<muxValue>) */
    int         muxValue = 0;
    double      factor = 1, offset = 0, minV = 0, maxV = 0;
    std::string unit, comment;
};

struct DbcMessage {
    FrameKey    key;
    std::string name;
    int         length = 8;
    std::vector<DbcSignal> signals_;
};

inline std::string dbcIdent(const std::string &s)
{
    std::string o;
    for (char ch : s) o += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') ? ch : '_';
    if (o.empty() || std::isdigit(static_cast<unsigned char>(o[0]))) o = "S_" + o;
    return o;
}

inline std::string toDbc(const std::vector<DbcMessage> &msgs)
{
    std::string out = "VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: Vector__XXX\n\n";
    char line[512];
    std::string comments;
    for (const DbcMessage &m : msgs)
    {
        uint32_t id = m.key.id | (m.key.ext ? 0x80000000u : 0);
        snprintf(line, sizeof(line), "BO_ %u %s: %d Vector__XXX\n", id, dbcIdent(m.name).c_str(), m.length);
        out += line;
        for (const DbcSignal &s : m.signals_)
        {
            std::string mux = s.muxRole == 1 ? " M" : s.muxRole == 2 ? " m" + std::to_string(s.muxValue) : "";
            snprintf(line, sizeof(line), " SG_ %s%s : %d|%d@%d%c (%.10g,%.10g) [%.10g|%.10g] \"%s\" Vector__XXX\n", dbcIdent(s.name).c_str(),
                     mux.c_str(), s.startBit, s.length, s.bigEndian ? 0 : 1, s.isSigned ? '-' : '+', s.factor, s.offset, s.minV, s.maxV,
                     s.unit.c_str());
            out += line;
            if (!s.comment.empty())
            {
                std::string c = s.comment;
                std::replace(c.begin(), c.end(), '"', '\'');
                snprintf(line, sizeof(line), "CM_ SG_ %u %s \"%s\";\n", id, dbcIdent(s.name).c_str(), c.c_str());
                comments += line;
            }
        }
        out += "\n";
    }
    return out + comments;
}

} // namespace canre

#endif // CANRE_H
