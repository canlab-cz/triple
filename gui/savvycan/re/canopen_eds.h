// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANOPEN_EDS_H
#define CANOPEN_EDS_H

/*
 * CANopen EDS / DCF reading (CiA 306) and PDO decoding (CiA 301), header
 * only and Qt free so it can be unit tested on the host
 * (tests/gui/canopen_eds_test.cpp).
 *
 *   Eds eds; eds.parse(text);                       // EDS or DCF file contents
 *   std::vector<Pdo> pdos = buildPdos(eds, node);   // TPDOs and RPDOs with mapping
 *   std::string v = formatEntry(pdo.entries[i], frameData, frameLen);
 *
 * Values: DCF "ParameterValue" wins over "DefaultValue". "$NODEID" in a
 * value is replaced by the node ID; sums such as "$NODEID+0x180" or
 * "0x200 + $NODEID" are evaluated.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace canopen {

enum DataType : uint16_t {
    BOOLEAN = 0x0001, INTEGER8 = 0x0002, INTEGER16 = 0x0003, INTEGER32 = 0x0004,
    UNSIGNED8 = 0x0005, UNSIGNED16 = 0x0006, UNSIGNED32 = 0x0007, REAL32 = 0x0008,
    VISIBLE_STRING = 0x0009, OCTET_STRING = 0x000A, INTEGER24 = 0x0010, REAL64 = 0x0011,
    INTEGER40 = 0x0012, INTEGER48 = 0x0013, INTEGER56 = 0x0014, INTEGER64 = 0x0015,
    UNSIGNED24 = 0x0016, UNSIGNED40 = 0x0018, UNSIGNED48 = 0x0019, UNSIGNED56 = 0x001A,
    UNSIGNED64 = 0x001B,
};

inline bool isSigned(uint16_t t)
{
    return t == INTEGER8 || t == INTEGER16 || t == INTEGER24 || t == INTEGER32 || t == INTEGER40
        || t == INTEGER48 || t == INTEGER56 || t == INTEGER64;
}

struct Object {
    std::string name;
    uint16_t    dataType = 0;
    std::string access;
    std::string value;            /* ParameterValue (DCF) or DefaultValue, unevaluated */
    bool        pdoMapping = false;
};

namespace detail {
inline std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
inline std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
inline bool parseUInt(const std::string &t, uint64_t &v)
{
    std::string s = trim(t);
    if (s.empty())
        return false;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        s = s.substr(2);
    }
    v = 0;
    for (char c : s)
    {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * uint64_t(base) + uint64_t(d);
    }
    return true;
}
}

/* Evaluates "0x180", "384", "$NODEID+0x180", "0x200 + $NODEID". */
inline bool evalValue(const std::string &expr, int nodeId, uint64_t &out)
{
    std::string s = expr;
    std::string l = detail::lower(s);
    size_t p;
    while ((p = l.find("$nodeid")) != std::string::npos)
    {
        s.replace(p, 7, std::to_string(nodeId));
        l.replace(p, 7, std::to_string(nodeId));
    }
    out = 0;
    std::stringstream ss(s);
    std::string term;
    bool any = false;
    while (std::getline(ss, term, '+'))
    {
        uint64_t v;
        if (!detail::parseUInt(term, v))
            return false;
        out += v;
        any = true;
    }
    return any;
}

class Eds {
public:
    std::map<std::pair<uint16_t, uint8_t>, Object> objects;
    std::string productName;

    void parse(const std::string &text)
    {
        objects.clear();
        std::istringstream in(text);
        std::string line, section;
        bool isObject = false;
        uint16_t index = 0;
        uint8_t sub = 0;
        std::map<std::string, std::string> keys;

        auto flush = [&]() {
            if (!isObject)
                return;
            Object o;
            o.name = keys["parametername"];
            uint64_t v;
            if (detail::parseUInt(keys["datatype"], v)) o.dataType = uint16_t(v);
            o.access = detail::lower(keys["accesstype"]);
            o.value = keys.count("parametervalue") ? keys["parametervalue"] : keys["defaultvalue"];
            o.pdoMapping = keys["pdomapping"] == "1";
            /* record / array headers (ObjectType 8 or 9) have no data type */
            if (o.dataType || keys.count("defaultvalue") || keys.count("parametervalue"))
                objects[{index, sub}] = o;
        };

        while (std::getline(in, line))
        {
            line = detail::trim(line);
            if (line.empty() || line[0] == ';')
                continue;
            if (line[0] == '[')
            {
                flush();
                keys.clear();
                section = detail::lower(detail::trim(line.substr(1, line.find(']') - 1)));
                isObject = false;
                size_t sp = section.find("sub");
                std::string idx = sp == std::string::npos ? section : section.substr(0, sp);
                uint64_t iv, sv = 0;
                bool hexIndex = idx.size() == 4 && detail::parseUInt("0x" + idx, iv);
                bool subOk = sp == std::string::npos || detail::parseUInt("0x" + section.substr(sp + 3), sv);
                if (hexIndex && subOk && sv <= 0xFF)
                {
                    isObject = true;
                    index = uint16_t(iv);
                    sub = uint8_t(sv);
                }
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = detail::lower(detail::trim(line.substr(0, eq)));
            std::string v = detail::trim(line.substr(eq + 1));
            if (section == "deviceinfo" && k == "productname")
                productName = v;
            keys[k] = v;
        }
        flush();
    }

    const Object *find(uint16_t index, uint8_t sub) const
    {
        auto it = objects.find({index, sub});
        return it == objects.end() ? nullptr : &it->second;
    }

    bool value(uint16_t index, uint8_t sub, int nodeId, uint64_t &v) const
    {
        const Object *o = find(index, sub);
        return o && evalValue(o->value, nodeId, v);
    }

    std::string name(uint16_t index, uint8_t sub) const
    {
        const Object *o = find(index, sub);
        if (o && !o->name.empty())
            return o->name;
        const Object *parent = find(index, 0);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04X:%02X", index, sub);
        return parent && sub == 0 ? parent->name : std::string(buf);
    }
};

struct PdoEntry {
    uint16_t    index = 0;
    uint8_t     sub = 0;
    uint8_t     bits = 0;
    unsigned    bitOffset = 0;
    uint16_t    dataType = 0;     /* 0 when unknown (no EDS entry) */
    std::string name;
};

struct Pdo {
    bool     tx = true;           /* TPDO: sent by the node; RPDO: received by it */
    int      number = 1;          /* 1-based */
    uint32_t cobId = 0;           /* 11-bit identifier */
    bool     valid = true;        /* bit 31 of the COB-ID clear */
    std::vector<PdoEntry> entries;
};

/* Builds the entry list from raw mapping values (index << 16 | sub << 8 | bits). */
inline std::vector<PdoEntry> mapEntries(const std::vector<uint32_t> &mapping, const Eds *eds)
{
    std::vector<PdoEntry> out;
    unsigned off = 0;
    for (uint32_t m : mapping)
    {
        PdoEntry e;
        e.index = uint16_t(m >> 16);
        e.sub = uint8_t(m >> 8);
        e.bits = uint8_t(m);
        e.bitOffset = off;
        off += e.bits;
        if (eds)
        {
            const Object *o = eds->find(e.index, e.sub);
            if (o) e.dataType = o->dataType;
            e.name = eds->name(e.index, e.sub);
        }
        else
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04X:%02X", e.index, e.sub);
            e.name = buf;
        }
        out.push_back(e);
    }
    return out;
}

/* TPDOs (1800/1A00..) and RPDOs (1400/1600..) described in the EDS for a node. */
inline std::vector<Pdo> buildPdos(const Eds &eds, int nodeId)
{
    std::vector<Pdo> out;
    for (int dir = 0; dir < 2; dir++)
    {
        uint16_t comm = dir == 0 ? 0x1800 : 0x1400;
        uint16_t map = dir == 0 ? 0x1A00 : 0x1600;
        for (int n = 0; n < 512; n++)
        {
            uint64_t cob, count;
            if (!eds.value(uint16_t(comm + n), 1, nodeId, cob))
                continue;
            Pdo p;
            p.tx = dir == 0;
            p.number = n + 1;
            p.valid = !(cob & 0x80000000ull);
            p.cobId = uint32_t(cob & 0x7FF);
            std::vector<uint32_t> mapping;
            if (eds.value(uint16_t(map + n), 0, nodeId, count))
                for (uint64_t s = 1; s <= count && s <= 64; s++)
                {
                    uint64_t m;
                    if (eds.value(uint16_t(map + n), uint8_t(s), nodeId, m))
                        mapping.push_back(uint32_t(m));
                }
            p.entries = mapEntries(mapping, &eds);
            out.push_back(p);
        }
    }
    return out;
}

/* Raw little-endian bit field of a PDO entry; false if the frame is too short. */
inline bool extract(const PdoEntry &e, const uint8_t *data, size_t len, uint64_t &raw)
{
    if (e.bits == 0 || e.bits > 64 || e.bitOffset + e.bits > len * 8)
        return false;
    raw = 0;
    for (unsigned i = 0; i < e.bits; i++)
    {
        unsigned bit = e.bitOffset + i;
        if (data[bit / 8] & (1u << (bit % 8)))
            raw |= uint64_t(1) << i;
    }
    return true;
}

/* Value of a PDO entry as text, using the EDS data type. */
inline std::string formatEntry(const PdoEntry &e, const uint8_t *data, size_t len)
{
    uint64_t raw;
    if (!extract(e, data, len, raw))
        return "-";
    char buf[64];
    if (e.dataType == BOOLEAN)
        return raw ? "TRUE" : "FALSE";
    if (e.dataType == REAL32 && e.bits == 32)
    {
        uint32_t u = uint32_t(raw);
        float f;
        std::memcpy(&f, &u, 4);
        snprintf(buf, sizeof(buf), "%g", double(f));
        return buf;
    }
    if (e.dataType == REAL64 && e.bits == 64)
    {
        double d;
        std::memcpy(&d, &raw, 8);
        snprintf(buf, sizeof(buf), "%g", d);
        return buf;
    }
    if ((e.dataType == VISIBLE_STRING || e.dataType == OCTET_STRING) && e.bits % 8 == 0)
    {
        std::string s;
        for (unsigned i = 0; i < e.bits / 8; i++)
        {
            char c = char((raw >> (8 * i)) & 0xFF);
            if (e.dataType == VISIBLE_STRING)
                s += (c >= 32 && c < 127) ? c : '.';
            else
            {
                snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", unsigned(uint8_t(c)));
                s += buf;
            }
        }
        return e.dataType == VISIBLE_STRING ? "\"" + s + "\"" : s;
    }
    if (isSigned(e.dataType) && e.bits < 64 && (raw & (uint64_t(1) << (e.bits - 1))))
    {
        int64_t v = int64_t(raw) - int64_t(uint64_t(1) << e.bits);
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    if (isSigned(e.dataType))
    {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(raw));
        return buf;
    }
    unsigned digits = (e.bits + 3) / 4;
    snprintf(buf, sizeof(buf), "%llu (0x%0*llX)", static_cast<unsigned long long>(raw), int(digits),
             static_cast<unsigned long long>(raw));
    return buf;
}

} // namespace canopen

#endif // CANOPEN_EDS_H
