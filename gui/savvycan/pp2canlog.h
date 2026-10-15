// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef PP2CANLOG_H
#define PP2CANLOG_H

/*
 * PP2CAN log files (CANLAB s.r.o.) in SavvyCAN: loading through the normal
 * "Load Log File" dialog and autodetection (so Playback can replay them,
 * which is PP2CAN's "File sender"), and saving the current capture in the
 * same format. Format details in pp2canlogfmt.h.
 */

#include <QString>
#include <QVector>
#include "re/savvycompat.h"

class QWidget;

namespace PP2CANLog {

/* True when the file starts with the PP2CAN header. */
bool isFile(const QString &filename);

/* Appends the frames of a PP2CAN log. Timestamps are the logged time of
 * day in microseconds (a log crossing midnight continues past 24 h).
 * ">" lines are received frames, "<" lines sent frames, all on bus 0. */
bool load(const QString &filename, QVector<GuiFrame> *frames);

/* Writes data and remote frames (error frames are skipped) as a PP2CAN log.
 * speed is written into the header, e.g. "250k". */
bool save(const QString &filename, const QVector<GuiFrame> *frames, const QString &speed);

/* File dialog plus save, for a menu entry. */
void saveWithDialog(QWidget *parent, const QVector<GuiFrame> *frames);

}

#endif // PP2CANLOG_H
