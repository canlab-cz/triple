// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef SAVVYCOMPAT_H
#define SAVVYCOMPAT_H

/*
 * Small helpers so a tool window compiles on SavvyCAN master (Qt 5,
 * frames are CANFrame / QCanBusFrame) and on the QT6WIP branch (Qt 6,
 * frames are CommFrame).
 */

#include <QtGlobal>
#include "can_structs.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef CommFrame GuiFrame;
#define GUI_DATA_FRAME CommFrame::CANDataFrame
#else
typedef CANFrame GuiFrame;
#define GUI_DATA_FRAME QCanBusFrame::DataFrame
#endif

inline int guiFrameBus(const GuiFrame &f)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return f.getBus();
#else
    return f.bus;
#endif
}

inline bool guiFrameReceived(const GuiFrame &f)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return f.isReceived();
#else
    return f.isReceived;
#endif
}

/* Microseconds on SavvyCAN's frame timeline (seconds and microseconds of
 * the frame timestamp combined; connections may fill either part). */
inline qint64 guiFrameMicros(const GuiFrame &f)
{
    return qint64(f.timeStamp().seconds()) * 1000000 + qint64(f.timeStamp().microSeconds());
}

/* A data frame ready for CANConManager::sendFrame(). */
inline GuiFrame guiMakeFrame(int bus, quint32 id, bool extended, const QByteArray &data)
{
    GuiFrame f;
    f.setFrameType(GUI_DATA_FRAME);
    f.setExtendedFrameFormat(extended);
    f.setFrameId(id);
    f.setPayload(data);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    f.setBus(bus);
    f.setReceived(false);
#else
    f.bus = bus;
    f.isReceived = false;
#endif
    return f;
}

#endif // SAVVYCOMPAT_H
