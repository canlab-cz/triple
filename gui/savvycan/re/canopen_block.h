// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANOPEN_BLOCK_H
#define CANOPEN_BLOCK_H

/*
 * CANopen SDO block transfer (CiA 301 7.2.4.3.9 ff.) and LSS master
 * services including fastscan (CiA 305), as Qt free state machines so they
 * are unit tested on the host (tests/gui/canopen_block_test.cpp). The
 * CANopen window only moves their 8-byte frames to and from the bus.
 *
 *   canopen::BlockUpload up(0x1008, 0);          // read
 *   send(0x600 + node, up.start());
 *   on 0x580 + node: auto r = up.feed(d, out);   // send everything in out
 *   r == Done -> up.data, r == Failed -> up.abortCode / up.error
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace canopen {

struct Frame8 {
    uint8_t d[8] = { 0 };
};

/* CRC-16 of block transfers: CCITT polynomial 0x1021, initial value 0, no
 * reflection (XMODEM) */
inline uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc = 0)
{
    for (size_t i = 0; i < n; i++)
    {
        crc ^= uint16_t(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

enum class Step { Pending, Done, Failed };

namespace detail {
inline void put16(uint8_t *d, uint16_t v) { d[0] = uint8_t(v); d[1] = uint8_t(v >> 8); }
inline void put32(uint8_t *d, uint32_t v) { for (int i = 0; i < 4; i++) d[i] = uint8_t(v >> (8 * i)); }
inline uint16_t get16(const uint8_t *d) { return uint16_t(d[0] | (d[1] << 8)); }
inline uint32_t get32(const uint8_t *d) { return uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24; }

inline Frame8 abortFrame(uint16_t index, uint8_t sub, uint32_t code)
{
    Frame8 f;
    f.d[0] = 0x80;
    put16(f.d + 1, index);
    f.d[3] = sub;
    put32(f.d + 4, code);
    return f;
}
}

/* Common part: object, result, abort handling. */
class BlockTransfer {
public:
    uint16_t index;
    uint8_t  sub;
    std::vector<uint8_t> data;
    uint32_t abortCode = 0;       /* from the server, or sent by us */
    std::string error;
    bool     crcUsed = false;     /* both sides agreed on a CRC */
    int      blocks = 0, retransmits = 0;

protected:
    BlockTransfer(uint16_t i, uint8_t s) : index(i), sub(s) {}
    Step fail(std::vector<Frame8> &out, uint32_t code, const std::string &why, bool sendAbort = true)
    {
        abortCode = code;
        error = why;
        if (sendAbort) out.push_back(detail::abortFrame(index, sub, code));
        return Step::Failed;
    }
    bool isAbort(const uint8_t *d, std::vector<Frame8> &, Step &r)
    {
        if (d[0] != 0x80) return false;
        abortCode = detail::get32(d + 4);
        error = "aborted by the server";
        r = Step::Failed;
        return true;
    }
};

/* Client reads an object with a block upload. */
class BlockUpload : public BlockTransfer {
public:
    BlockUpload(uint16_t idx, uint8_t s, uint8_t blockSize = 127, bool wantCrc = true)
        : BlockTransfer(idx, s), blksize(blockSize < 1 ? 1 : blockSize > 127 ? 127 : blockSize), crcWanted(wantCrc) {}

    Frame8 start()
    {
        Frame8 f;
        f.d[0] = uint8_t(0xA0 | (crcWanted ? 0x04 : 0));     /* ccs 5, cc, cs 0: initiate */
        detail::put16(f.d + 1, index);
        f.d[3] = sub;
        f.d[4] = blksize;
        f.d[5] = 0;                                          /* pst 0: no switch to a normal transfer */
        state = Initiate;
        return f;
    }

    Step feed(const uint8_t *d, std::vector<Frame8> &out)
    {
        Step r;
        if (state != Segments && isAbort(d, out, r)) return r;
        switch (state)
        {
        case Initiate:
            /* scs 6, sc (bit 2) and s (bit 1) vary, ss (bit 0) 0 */
            if ((d[0] & 0xE1) != 0xC0) return fail(out, 0x05040001, "unexpected answer to the block upload request");
            if (detail::get16(d + 1) != index || d[3] != sub) return fail(out, 0x05040001, "answer for another object");
            crcUsed = crcWanted && (d[0] & 0x04);
            sizeKnown = d[0] & 0x02;
            size = sizeKnown ? detail::get32(d + 4) : 0;
            out.push_back(command(0xA3));                    /* start upload */
            state = Segments;
            seq = 0;
            block.clear();
            return Step::Pending;
        case Segments:
        {
            if (d[0] == 0x80)                                /* sequence number 0 does not exist: an abort */
            {
                isAbort(d, out, r);
                return r;
            }
            uint8_t s = d[0] & 0x7F;
            bool last = d[0] & 0x80;
            if (s == seq + 1 && !lastSeen)
            {
                seq = s;
                block.insert(block.end(), d + 1, d + 8);
                if (last) lastSeen = true;
            }
            /* acknowledge after a full block or the last segment */
            if (s == blksize || last)
            {
                if (seq < s && !lastSeen) retransmits++;
                Frame8 ack = command(0xA2);
                ack.d[1] = seq;
                ack.d[2] = blksize;
                out.push_back(ack);
                data.insert(data.end(), block.begin(), block.end());
                block.clear();
                blocks++;
                if (lastSeen) state = End;
                seq = 0;
            }
            return Step::Pending;
        }
        case End:
        {
            if ((d[0] & 0xE3) != 0xC1) return fail(out, 0x05040001, "expected the end of the block upload");
            int unused = (d[0] >> 2) & 0x07;
            if (unused > 0 && size_t(unused) <= data.size()) data.resize(data.size() - size_t(unused));
            if (sizeKnown && data.size() != size)
                return fail(out, 0x06070010, "size does not match: announced " + std::to_string(size) + ", got " + std::to_string(data.size()));
            if (crcUsed && detail::get16(d + 1) != crc16(data.data(), data.size()))
                return fail(out, 0x05040004, "CRC error");
            out.push_back(command(0xA1));                    /* end upload response */
            state = Finished;
            return Step::Done;
        }
        default:
            return Step::Pending;
        }
    }

private:
    enum State { Idle, Initiate, Segments, End, Finished } state = Idle;
    uint8_t blksize;
    bool crcWanted, sizeKnown = false, lastSeen = false;
    uint32_t size = 0;
    uint8_t seq = 0;
    std::vector<uint8_t> block;

    Frame8 command(uint8_t cs) { Frame8 f; f.d[0] = cs; return f; }
};

/* Client writes an object with a block download. */
class BlockDownload : public BlockTransfer {
public:
    BlockDownload(uint16_t idx, uint8_t s, const std::vector<uint8_t> &bytes, bool wantCrc = true)
        : BlockTransfer(idx, s), crcWanted(wantCrc) { data = bytes; }

    Frame8 start()
    {
        Frame8 f;
        f.d[0] = uint8_t(0xC0 | (crcWanted ? 0x04 : 0) | 0x02);   /* ccs 6, cc, s, cs 0 */
        detail::put16(f.d + 1, index);
        f.d[3] = sub;
        detail::put32(f.d + 4, uint32_t(data.size()));
        state = Initiate;
        return f;
    }

    Step feed(const uint8_t *d, std::vector<Frame8> &out)
    {
        Step r;
        if (isAbort(d, out, r)) return r;
        switch (state)
        {
        case Initiate:
            if ((d[0] & 0xE3) != 0xA0) return fail(out, 0x05040001, "unexpected answer to the block download request");
            if (detail::get16(d + 1) != index || d[3] != sub) return fail(out, 0x05040001, "answer for another object");
            crcUsed = crcWanted && (d[0] & 0x04);
            blksize = d[4];
            if (blksize < 1 || blksize > 127) return fail(out, 0x05040002, "invalid block size from the server");
            pos = 0;
            sendBlock(out);
            state = Ack;
            return Step::Pending;
        case Ack:
        {
            if (d[0] != 0xA2) return fail(out, 0x05040001, "expected a block acknowledge");
            uint8_t ackseq = d[1];
            size_t acked = blockStart + size_t(ackseq) * 7;
            if (ackseq < sentInBlock)
            {
                retransmits++;
                lastSent = false;
            }
            pos = acked < data.size() ? acked : data.size();
            blocks++;
            uint8_t next = d[2];
            if (next >= 1 && next <= 127) blksize = next;
            if (lastSent && ackseq == sentInBlock)
            {
                Frame8 e;
                size_t inLast = data.empty() ? 0 : ((data.size() - 1) % 7) + 1;
                e.d[0] = uint8_t(0xC1 | ((7 - inLast) << 2));      /* end download, n unused bytes */
                detail::put16(e.d + 1, crcUsed ? crc16(data.data(), data.size()) : 0);
                out.push_back(e);
                state = End;
                return Step::Pending;
            }
            sendBlock(out);
            return Step::Pending;
        }
        case End:
            if (d[0] != 0xA1) return fail(out, 0x05040001, "expected the end of the block download");
            state = Finished;
            return Step::Done;
        default:
            return Step::Pending;
        }
    }

private:
    enum State { Idle, Initiate, Ack, End, Finished } state = Idle;
    bool crcWanted, lastSent = false;
    uint8_t blksize = 0, sentInBlock = 0;
    size_t pos = 0, blockStart = 0;

    void sendBlock(std::vector<Frame8> &out)
    {
        blockStart = pos;
        sentInBlock = 0;
        lastSent = false;
        do
        {
            Frame8 f;
            size_t n = data.size() - pos < 7 ? data.size() - pos : 7;
            sentInBlock++;
            bool last = pos + n >= data.size();
            f.d[0] = uint8_t((last ? 0x80 : 0) | sentInBlock);
            if (n) std::memcpy(f.d + 1, data.data() + pos, n);
            pos += n;
            out.push_back(f);
            if (last) { lastSent = true; break; }
        } while (sentInBlock < blksize);
    }
};

/* ------------------------------------------------------------------- LSS */

namespace lss {

const uint32_t MASTER_COB = 0x7E5;    /* master -> slaves */
const uint32_t SLAVE_COB = 0x7E4;     /* slaves -> master */

enum Cs : uint8_t {
    SWITCH_GLOBAL = 0x04,
    CONFIGURE_NODE_ID = 0x11, CONFIGURE_BIT_TIMING = 0x13, ACTIVATE_BIT_TIMING = 0x15, STORE = 0x17,
    SWITCH_SELECTIVE_VENDOR = 0x40, SWITCH_SELECTIVE_PRODUCT = 0x41, SWITCH_SELECTIVE_REVISION = 0x42,
    SWITCH_SELECTIVE_SERIAL = 0x43, SWITCH_SELECTIVE_RESPONSE = 0x44,
    IDENTIFY_SLAVE = 0x4F, IDENTIFY_NON_CONFIGURED = 0x4C, NON_CONFIGURED_RESPONSE = 0x50, FASTSCAN = 0x51,
    INQUIRE_VENDOR = 0x5A, INQUIRE_PRODUCT = 0x5B, INQUIRE_REVISION = 0x5C, INQUIRE_SERIAL = 0x5D, INQUIRE_NODE_ID = 0x5E,
};

/* CiA 305 bit timing table 0 (index -> bit/s) */
inline uint32_t tableBitrate(int index)
{
    static const uint32_t t[] = { 1000000, 800000, 500000, 250000, 125000, 0, 50000, 20000, 10000 };
    return index >= 0 && index < 9 ? t[index] : 0;
}

inline Frame8 cmd(uint8_t cs) { Frame8 f; f.d[0] = cs; return f; }
inline Frame8 switchGlobal(bool configuration) { Frame8 f = cmd(SWITCH_GLOBAL); f.d[1] = configuration ? 1 : 0; return f; }
inline Frame8 switchSelective(int part, uint32_t v) { Frame8 f = cmd(uint8_t(SWITCH_SELECTIVE_VENDOR + part)); detail::put32(f.d + 1, v); return f; }
inline Frame8 configureNodeId(uint8_t id) { Frame8 f = cmd(CONFIGURE_NODE_ID); f.d[1] = id; return f; }
inline Frame8 configureBitTiming(uint8_t index) { Frame8 f = cmd(CONFIGURE_BIT_TIMING); f.d[1] = 0; f.d[2] = index; return f; }
inline Frame8 activateBitTiming(uint16_t delayMs) { Frame8 f = cmd(ACTIVATE_BIT_TIMING); detail::put16(f.d + 1, delayMs); return f; }
inline Frame8 store() { return cmd(STORE); }
inline Frame8 inquire(uint8_t cs) { return cmd(cs); }
inline Frame8 fastscan(uint32_t idNumber, uint8_t bitChecked, uint8_t sub, uint8_t next)
{
    Frame8 f = cmd(FASTSCAN);
    detail::put32(f.d + 1, idNumber);
    f.d[5] = bitChecked; f.d[6] = sub; f.d[7] = next;
    return f;
}

inline std::string errorText(uint8_t cs, uint8_t code)
{
    if (code == 0) return "ok";
    if (cs == CONFIGURE_NODE_ID) return code == 1 ? "node-ID out of range" : "error " + std::to_string(code);
    if (cs == CONFIGURE_BIT_TIMING) return code == 1 ? "bit timing not supported" : "error " + std::to_string(code);
    if (cs == STORE) return code == 1 ? "store not supported" : code == 2 ? "storage media access error" : "error " + std::to_string(code);
    return "error " + std::to_string(code);
}

/*
 * Fastscan: finds one non-configured slave's identity (vendor, product,
 * revision, serial) bit by bit and leaves it in configuration state, about
 * 132 requests. Each request either gets an "identify slave" answer or
 * times out; the caller sends next() and calls answer(true/false).
 */
class Fastscan {
public:
    uint32_t id[4] = { 0, 0, 0, 0 };
    enum Phase { Reset, Bits, Confirm, Found, NoneFound, Lost } phase = Reset;

    Frame8 next() const
    {
        if (phase == Reset) return fastscan(0, 0x80, 0, 0);
        if (phase == Bits) return fastscan(id[sub], uint8_t(bit), uint8_t(sub), uint8_t(sub));
        return fastscan(id[sub], 0, uint8_t(sub), uint8_t((sub + 1) % 4));   /* Confirm */
    }

    /* answered: an identify-slave frame came back before the timeout */
    void answer(bool answered)
    {
        switch (phase)
        {
        case Reset:
            phase = answered ? Bits : NoneFound;
            sub = 0; bit = 31; id[0] = id[1] = id[2] = id[3] = 0;
            break;
        case Bits:
            /* the slave answers when its value matches IDNumber on the bits
             * above and including bit; no answer: that bit is 1 */
            if (!answered) id[sub] |= uint32_t(1) << bit;
            if (bit == 0) phase = Confirm;
            else bit--;
            break;
        case Confirm:
            if (!answered) { phase = Lost; break; }
            if (sub == 3) phase = Found;
            else { sub++; bit = 31; phase = Bits; }
            break;
        default:
            break;
        }
    }

    bool finished() const { return phase == Found || phase == NoneFound || phase == Lost; }
    int requests = 0;

private:
    int sub = 0, bit = 31;
};

} // namespace lss
} // namespace canopen

#endif // CANOPEN_BLOCK_H
