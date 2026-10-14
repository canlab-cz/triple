// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANIDCALC_H
#define CANIDCALC_H

/*
 * CAN identifier conversions, header only, no Qt, so they can be unit
 * tested on the host (tests/gui/canidcalc_test.cpp).
 *
 *   29-bit identifier  <->  11-bit base + 18-bit extension ("11+18")
 *   identifier         <->  MCP2510 / MCP2515 TX/RX buffer registers
 *                           SIDH, SIDL, EID8, EID0
 *   identifier         <->  SJA1000 (PeliCAN mode) identifier registers
 *                           ID1..ID2 (standard) or ID1..ID4 (extended)
 *
 * MCP2515 data sheet, registers 3-3..3-6:
 *   SIDH = SID[10:3]
 *   SIDL = SID[2:0] << 5 | EXIDE << 3 | EID[17:16]
 *   EID8 = EID[15:8], EID0 = EID[7:0]
 * SJA1000 data sheet, PeliCAN TX/RX buffer:
 *   standard: ID1 = ID[10:3], ID2 = ID[2:0] << 5 | RTR << 4
 *   extended: ID1 = ID[28:21], ID2 = ID[20:13], ID3 = ID[12:5],
 *             ID4 = ID[4:0] << 3 | RTR << 2
 */

#include <cstdint>

namespace canidcalc {

struct Mcp {
    uint8_t sidh, sidl, eid8, eid0;
};

struct Sja {
    uint8_t id[4];
    int     count;                  /* 2 for standard, 4 for extended frames */
};

inline uint32_t baseOf(uint32_t id29) { return (id29 >> 18) & 0x7FF; }
inline uint32_t extOf(uint32_t id29)  { return id29 & 0x3FFFF; }
inline uint32_t join(uint32_t base11, uint32_t ext18) { return ((base11 & 0x7FF) << 18) | (ext18 & 0x3FFFF); }

inline Mcp toMcp(uint32_t id, bool extended)
{
    Mcp r;
    uint32_t sid = extended ? baseOf(id) : (id & 0x7FF);
    uint32_t eid = extended ? extOf(id) : 0;
    r.sidh = uint8_t(sid >> 3);
    r.sidl = uint8_t(((sid & 0x7) << 5) | (extended ? 0x08 : 0x00) | ((eid >> 16) & 0x3));
    r.eid8 = uint8_t(eid >> 8);
    r.eid0 = uint8_t(eid);
    return r;
}

/* Returns the identifier; sets extended from the EXIDE bit. */
inline uint32_t fromMcp(const Mcp &m, bool &extended)
{
    uint32_t sid = (uint32_t(m.sidh) << 3) | (uint32_t(m.sidl) >> 5);
    extended = (m.sidl & 0x08) != 0;
    if (!extended)
        return sid;
    uint32_t eid = (uint32_t(m.sidl & 0x3) << 16) | (uint32_t(m.eid8) << 8) | m.eid0;
    return join(sid, eid);
}

inline Sja toSja(uint32_t id, bool extended, bool rtr)
{
    Sja r = {};
    if (!extended)
    {
        id &= 0x7FF;
        r.id[0] = uint8_t(id >> 3);
        r.id[1] = uint8_t(((id & 0x7) << 5) | (rtr ? 0x10 : 0));
        r.count = 2;
    }
    else
    {
        id &= 0x1FFFFFFF;
        r.id[0] = uint8_t(id >> 21);
        r.id[1] = uint8_t(id >> 13);
        r.id[2] = uint8_t(id >> 5);
        r.id[3] = uint8_t(((id & 0x1F) << 3) | (rtr ? 0x04 : 0));
        r.count = 4;
    }
    return r;
}

inline uint32_t fromSja(const Sja &s, bool extended, bool &rtr)
{
    if (!extended)
    {
        rtr = (s.id[1] & 0x10) != 0;
        return (uint32_t(s.id[0]) << 3) | (uint32_t(s.id[1]) >> 5);
    }
    rtr = (s.id[3] & 0x04) != 0;
    return (uint32_t(s.id[0]) << 21) | (uint32_t(s.id[1]) << 13) | (uint32_t(s.id[2]) << 5) | (uint32_t(s.id[3]) >> 3);
}

} // namespace canidcalc

#endif // CANIDCALC_H
