// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef RECAPTURE_H
#define RECAPTURE_H

/* SavvyCAN's frame list -> canre::Capture (data frames only). */

#include <QVector>
#include "canre.h"
#include "savvycompat.h"

inline void fillCapture(canre::Capture &cap, const QVector<GuiFrame> &frames)
{
    cap = canre::Capture();
    for (const GuiFrame &f : frames)
    {
        if (f.frameType() != GUI_DATA_FRAME)
            continue;
        QByteArray d = f.payload();
        qint64 us = guiFrameMicros(f);
        cap.add(us < 0 ? 0 : uint64_t(us), uint8_t(guiFrameBus(f)), f.frameId(), f.hasExtendedFrameFormat(), uint8_t(qMin(d.size(), 64)),
                reinterpret_cast<const uint8_t *>(d.constData()));
    }
}

#endif // RECAPTURE_H
