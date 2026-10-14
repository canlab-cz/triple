// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
//
// Host test of SDO block transfer and LSS fastscan (gui/savvycan/re/canopen_block.h)
// against small reference implementations of the server and slave side.
#include <cstdio>
#include <cstring>
#include <deque>
#include "canopen_block.h"

using namespace canopen;

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- reference SDO server, block upload and download only ---- */
struct Server {
    std::vector<uint8_t> object;              /* what an upload returns / a download wrote */
    uint8_t blksize = 5;                      /* the server's block size for downloads */
    bool crc = true;
    int dropSegment = -1;                     /* upload: segment counter value to drop once */
    int dropDownloadSeq = -1;                 /* download: pretend this sequence number was lost (once) */
    bool unsupported = false;

    std::deque<Frame8> out;
    /* upload state */
    size_t pos = 0, blockStart = 0;
    uint8_t clientBlk = 0;
    int sentCount = 0;
    bool clientCrc = false, lastSent = false;
    /* download state */
    std::vector<uint8_t> rx;
    uint8_t expect = 1;
    bool dlLast = false;
    enum { Idle, UpReady, UpSending, UpEnd, Down, DownEnd } st = Idle;

    void sendUploadBlock()
    {
        blockStart = pos;
        lastSent = false;
        for (uint8_t s = 1; s <= clientBlk; s++)
        {
            Frame8 f;
            size_t n = object.size() - pos < 7 ? object.size() - pos : 7;
            bool last = pos + n >= object.size();
            f.d[0] = uint8_t((last ? 0x80 : 0) | s);
            std::memcpy(f.d + 1, object.data() + pos, n);
            pos += n;
            if (sentCount++ != dropSegment) out.push_back(f);
            else dropSegment = -1;
            if (last) { lastSent = true; break; }
        }
    }

    void feed(const Frame8 &c)
    {
        const uint8_t *d = c.d;
        if (unsupported) { out.push_back(detail::abortFrame(detail::get16(d + 1), d[3], 0x05040001)); return; }
        if ((d[0] & 0xE3) == 0xA0 && st == Idle)                 /* initiate block upload */
        {
            clientCrc = d[0] & 0x04;
            clientBlk = d[4];
            Frame8 r;
            r.d[0] = uint8_t(0xC0 | (crc ? 0x04 : 0) | 0x02);
            r.d[1] = d[1]; r.d[2] = d[2]; r.d[3] = d[3];
            detail::put32(r.d + 4, uint32_t(object.size()));
            out.push_back(r);
            st = UpReady; pos = 0;
        }
        else if (d[0] == 0xA3 && st == UpReady) { st = UpSending; sendUploadBlock(); }
        else if (d[0] == 0xA2 && st == UpSending)                /* block acknowledged */
        {
            pos = blockStart + size_t(d[1]) * 7;
            if (pos > object.size()) pos = object.size();
            clientBlk = d[2];
            bool all = lastSent && pos >= object.size();
            if (all)
            {
                Frame8 e;
                size_t inLast = ((object.size() - 1) % 7) + 1;
                e.d[0] = uint8_t(0xC1 | ((7 - inLast) << 2));
                detail::put16(e.d + 1, (crc && clientCrc) ? crc16(object.data(), object.size()) : 0);
                out.push_back(e);
                st = UpEnd;
            }
            else sendUploadBlock();
        }
        else if (d[0] == 0xA1 && st == UpEnd) st = Idle;
        else if ((d[0] & 0xE1) == 0xC0 && st == Idle)            /* initiate block download */
        {
            clientCrc = d[0] & 0x04;
            Frame8 r;
            r.d[0] = uint8_t(0xA0 | (crc ? 0x04 : 0));
            r.d[1] = d[1]; r.d[2] = d[2]; r.d[3] = d[3];
            r.d[4] = blksize;
            out.push_back(r);
            rx.clear(); expect = 1; st = Down;
        }
        else if (st == Down)
        {
            uint8_t s = d[0] & 0x7F;
            bool last = d[0] & 0x80;
            if (s == dropDownloadSeq) { dropDownloadSeq = -1; s = 0xFF; }   /* lost */
            if (s == expect) { rx.insert(rx.end(), d + 1, d + 8); expect++; if (last) dlLast = true; }
            if ((d[0] & 0x7F) == blksize || last)
            {
                Frame8 a;
                a.d[0] = 0xA2;
                a.d[1] = uint8_t(expect - 1);
                a.d[2] = blksize;
                out.push_back(a);
                expect = 1;
                if (dlLast) st = DownEnd;
            }
        }
        else if ((d[0] & 0xE3) == 0xC1 && st == DownEnd)          /* end download */
        {
            int unused = (d[0] >> 2) & 7;
            rx.resize(rx.size() - size_t(unused));
            bool ok = !(crc && clientCrc) || detail::get16(d + 1) == crc16(rx.data(), rx.size());
            object = rx;
            if (ok) { Frame8 r; r.d[0] = 0xA1; out.push_back(r); }
            else out.push_back(detail::abortFrame(0, 0, 0x05040004));
            st = Idle;
        }
    }
};

template <class T> static Step run(T &client, Server &srv, int *frames = nullptr)
{
    std::vector<Frame8> out{ client.start() };
    Step r = Step::Pending;
    int n = 0;
    for (int guard = 0; guard < 100000 && r == Step::Pending; guard++)
    {
        for (const Frame8 &f : out) { srv.feed(f); n++; }
        out.clear();
        if (srv.out.empty()) break;
        Frame8 f = srv.out.front();
        srv.out.pop_front();
        r = client.feed(f.d, out);
        n++;
    }
    for (const Frame8 &f : out) srv.feed(f);   /* the last response (end confirmation) */
    if (frames) *frames = n;
    return r;
}

/* ---- reference LSS slave, fastscan only ---- */
struct LssSlave {
    uint32_t id[4];
    bool configured = false;      /* has a node-ID: does not take part in fastscan */
    int pos = 0;
    bool configState = false;
    bool feed(const Frame8 &f)    /* returns true if it answers "identify slave" */
    {
        if (f.d[0] != lss::FASTSCAN || configured) return false;
        uint32_t idn = detail::get32(f.d + 1);
        uint8_t bit = f.d[5], sub = f.d[6], next = f.d[7];
        if (bit == 0x80) { pos = 0; return true; }
        if (sub != pos || bit > 31) return false;
        uint32_t mask = bit == 0 ? 0xFFFFFFFFu : ~((uint32_t(1) << bit) - 1);
        if (((id[sub] ^ idn) & mask) != 0) return false;
        if (bit == 0)
        {
            if (next < sub) configState = true;   /* wrapped after the serial number */
            pos = next;
        }
        return true;
    }
};

int main()
{
    /* CRC-16 CCITT XMODEM check value */
    const uint8_t chk[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK(crc16(chk, 9) == 0x31C3);

    std::vector<uint8_t> obj;
    for (int i = 0; i < 1000; i++) obj.push_back(uint8_t(i * 7 + 3));

    /* upload of 1000 bytes in blocks of 20 segments, with CRC */
    {
        Server srv; srv.object = obj;
        BlockUpload up(0x1F50, 1, 20);
        int frames = 0;
        Step r = run(up, srv, &frames);
        CHECK(r == Step::Done && up.data == obj && up.crcUsed && up.retransmits == 0);
        CHECK(up.blocks == 8);                                       /* 143 segments / 20 */
        std::printf("upload 1000 bytes: %d frames, %d blocks\n", frames, up.blocks);
    }
    /* a lost segment is acknowledged short and sent again */
    {
        Server srv; srv.object = obj; srv.dropSegment = 25;
        BlockUpload up(0x1F50, 1, 20);
        CHECK(run(up, srv) == Step::Done && up.data == obj && up.retransmits == 1);
    }
    /* sizes around a segment: 1, 7, 8, 14 bytes; without CRC */
    for (size_t n : { 1u, 7u, 8u, 14u })
    {
        Server srv; srv.object.assign(obj.begin(), obj.begin() + long(n)); srv.crc = false;
        BlockUpload up(0x2001, 0, 127, true);
        CHECK(run(up, srv) == Step::Done && up.data.size() == n && !up.crcUsed);
    }
    /* server without block transfer: the abort comes back as a failure */
    {
        Server srv; srv.unsupported = true;
        BlockUpload up(0x1008, 0);
        CHECK(run(up, srv) == Step::Failed && up.abortCode == 0x05040001);
    }

    /* download of 1000 bytes, server block size 5, with CRC */
    {
        Server srv; srv.blksize = 5;
        BlockDownload dn(0x1F50, 1, obj);
        int frames = 0;
        Step r = run(dn, srv, &frames);
        CHECK(r == Step::Done && srv.object == obj && dn.crcUsed && dn.retransmits == 0);
        std::printf("download 1000 bytes: %d frames, %d blocks\n", frames, dn.blocks);
    }
    /* a segment lost on the way to the server */
    {
        Server srv; srv.blksize = 10; srv.dropDownloadSeq = 4;
        BlockDownload dn(0x1F50, 1, obj);
        CHECK(run(dn, srv) == Step::Done && srv.object == obj && dn.retransmits == 1);
    }
    for (size_t n : { 1u, 6u, 7u, 8u, 70u })
    {
        Server srv; srv.blksize = 10;
        std::vector<uint8_t> part(obj.begin(), obj.begin() + long(n));
        BlockDownload dn(0x2001, 0, part, false);
        CHECK(run(dn, srv) == Step::Done && srv.object == part);
    }
    /* block upload request bytes: ccs 5, cc, blksize 127, pst 0 */
    {
        BlockUpload up(0x1018, 4);
        Frame8 f = up.start();
        CHECK(f.d[0] == 0xA4 && f.d[1] == 0x18 && f.d[2] == 0x10 && f.d[3] == 4 && f.d[4] == 127 && f.d[5] == 0);
        BlockDownload dn(0x1018, 4, { 1, 2, 3 });
        f = dn.start();
        CHECK(f.d[0] == 0xC6 && f.d[4] == 3 && f.d[5] == 0);
    }

    /* LSS fastscan finds the identity of the unconfigured slave and leaves it in configuration state */
    {
        LssSlave a{ { 0x000004D8, 0x0000F0D7, 0x00010002, 0x0027BBE2 } };
        LssSlave b{ { 0x000004D8, 0x0000F0D7, 0x00010002, 0x12345678 } };
        b.configured = true;                                      /* has a node-ID */
        lss::Fastscan fs;
        int requests = 0;
        while (!fs.finished() && requests < 200)
        {
            Frame8 f = fs.next();
            bool ans = a.feed(f);
            ans = b.feed(f) || ans;
            fs.answer(ans);
            requests++;
        }
        CHECK(fs.phase == lss::Fastscan::Found);
        CHECK(fs.id[0] == 0x4D8 && fs.id[1] == 0xF0D7 && fs.id[2] == 0x10002 && fs.id[3] == 0x0027BBE2);
        CHECK(a.configState && !b.configState);
        CHECK(requests == 1 + 4 * 33);
        std::printf("fastscan: %d requests\n", requests);

        lss::Fastscan none;
        LssSlave c{ { 1, 2, 3, 4 } };
        c.configured = true;
        none.answer(c.feed(none.next()));
        CHECK(none.phase == lss::Fastscan::NoneFound && none.finished());
    }
    /* LSS command bytes */
    CHECK(lss::switchGlobal(true).d[0] == 0x04 && lss::switchGlobal(true).d[1] == 1);
    CHECK(lss::switchSelective(3, 0x0027BBE2).d[0] == 0x43 && lss::switchSelective(3, 0x0027BBE2).d[1] == 0xE2);
    CHECK(lss::configureNodeId(5).d[0] == 0x11 && lss::configureNodeId(5).d[1] == 5);
    CHECK(lss::configureBitTiming(3).d[2] == 3 && lss::tableBitrate(3) == 250000 && lss::tableBitrate(5) == 0);
    CHECK(lss::activateBitTiming(1000).d[1] == 0xE8 && lss::activateBitTiming(1000).d[2] == 0x03);
    CHECK(lss::errorText(lss::STORE, 1) == "store not supported");

    std::printf("%s (%d failures)\n", fails ? "CANOPEN BLOCK TEST FAILED" : "CANOPEN BLOCK TEST PASSED", fails);
    return fails ? 1 : 0;
}
