// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef BUSSTATSWINDOW_H
#define BUSSTATSWINDOW_H

/*
 * Bus statistics: per bus load in percent, frame rate, error frames and
 * the last reported bus state with TEC/REC, plus a load graph. Works with
 * any SavvyCAN connection; bus state and counters come from error frames
 * in SocketCAN layout (state in byte 1, TEC/REC in bytes 6/7), which the
 * USB2CAN Triple connection and SocketCAN devices produce.
 *
 * Load is the time the observed frames occupy the bus, computed from
 * frame length, identifier type and, for CAN FD with bit rate switch, the
 * data phase at the data bitrate. Bit stuffing is not counted, so the
 * value is a lower bound (stuffing adds up to about 20 %).
 */

#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>

#include "can_structs.h"

class QTableWidget;
class QLabel;
class QCustomPlot;
class CANConnection;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef CommFrame BusStatsFrame;   /* SavvyCAN QT6WIP branch */
#else
typedef CANFrame BusStatsFrame;    /* SavvyCAN master (Qt 5) */
#endif

class BusStatsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit BusStatsWindow(QWidget *parent = nullptr);

public slots:
    void framesReceived(CANConnection *conn, QVector<BusStatsFrame> &frames);

private slots:
    void tick();
    void reset();

private:
    struct Bus {
        quint32 nominal = 0;        /* bit/s, 0 = unknown */
        quint32 data = 0;           /* CAN FD data phase bit/s, 0 = none */
        quint64 frames = 0;
        quint64 errors = 0;
        quint64 overflows = 0;      /* error frames reporting lost frames (controller RX/TX overflow) */
        quint64 intervalFrames = 0;
        double  intervalBusy = 0;   /* seconds of bus time in this interval */
        double  load = 0;
        double  peak = 0;
        double  rate = 0;
        int     state = -1;         /* -1 unknown, 0 active, 1 warning, 2 passive, 3 bus off */
        int     tec = -1;
        int     rec = -1;
    };

    void refreshBitrates();
    void ensureBus(int idx);
    static double frameSeconds(const BusStatsFrame &f, quint32 nominal, quint32 data);
    void updateTable();
    void updatePlot(double t);

    QVector<Bus>   buses;
    QTableWidget  *table;
    QCustomPlot   *plot;
    QLabel        *note;
    QTimer         timer;
    QElapsedTimer  interval;
    QElapsedTimer  since;
};

#endif // BUSSTATSWINDOW_H
