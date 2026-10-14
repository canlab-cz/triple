// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef RESPONSEMETERWINDOW_H
#define RESPONSEMETERWINDOW_H

/*
 * Response meter: time from a request frame to the matching response
 * frame, with statistics.
 *
 * Watch mode: both frames are seen on the bus (another node sends the
 * request); the delay comes from the frame timestamps, so with a device
 * that timestamps in hardware it is accurate to microseconds.
 *
 * Send mode: this window sends the request periodically. The request time
 * is taken from the host clock and the response time from the frame
 * timestamp, so the result carries the offset between the two clocks
 * (typically a few milliseconds with USB adapters).
 */

#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>

#include "savvycompat.h"

class QSpinBox;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class CANConnection;

class ResponseMeterWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ResponseMeterWindow(QWidget *parent = nullptr);

public slots:
    void framesReceived(CANConnection *conn, QVector<GuiFrame> &frames);

private slots:
    void startStop();
    void reset();
    void tick();
    void sendRequest();

private:
    bool parseId(const QLineEdit *e, quint32 &id, bool &ext) const;
    void addMeasurement(qint64 delayUs);
    void updateStats();
    qint64 hostMicros() const;

    QSpinBox     *busSpin;
    QLineEdit    *reqIdEdit, *respIdEdit, *reqDataEdit;
    QComboBox    *modeCombo;
    QSpinBox     *periodSpin, *countSpin, *timeoutSpin;
    QPushButton  *startBtn;
    QLabel       *statsLabel, *noteLabel;
    QTableWidget *table;

    QTimer        sendTimer;
    QTimer        checkTimer;
    QElapsedTimer pendingSince;
    bool          running = false;
    bool          pending = false;
    qint64        pendingUs = 0;
    quint32       reqId = 0, respId = 0;
    bool          reqExt = false, respExt = false;
    int           sent = 0;
    int           timeouts = 0;
    QVector<qint64> delays;
};

#endif // RESPONSEMETERWINDOW_H
