// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANOPENWINDOW_H
#define CANOPENWINDOW_H

/*
 * CANopen basics (CiA 301) for one bus: node monitor from heartbeat,
 * boot-up and EMCY messages, NMT master commands, SYNC, and an SDO client
 * (expedited and segmented upload/download) with readable abort codes.
 * Works with any SavvyCAN connection.
 */

#include <QWidget>
#include <QMap>
#include <QList>
#include <QTimer>
#include <QElapsedTimer>
#include <QByteArray>

#include "can_structs.h"

class QTableWidget;
class QSpinBox;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class CANConnection;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef CommFrame CANopenFrame;    /* SavvyCAN QT6WIP branch */
#else
typedef CANFrame CANopenFrame;     /* SavvyCAN master (Qt 5) */
#endif

class CANopenWindow : public QWidget
{
    Q_OBJECT

public:
    explicit CANopenWindow(QWidget *parent = nullptr);

public slots:
    void framesReceived(CANConnection *conn, QVector<CANopenFrame> &frames);

private slots:
    void refreshNodes();
    void sdoTimeout();
    void readClicked();
    void writeClicked();
    void identityClicked();

private:
    enum ValueType { T_U8, T_U16, T_U32, T_I8, T_I16, T_I32, T_STRING, T_HEX };

    struct Node {
        int     state = -1;           /* last heartbeat state byte, -1 unknown */
        qint64  lastSeenMs = -1;
        int     bootups = 0;
        int     emcyCount = 0;
        QString lastEmcy;
    };

    struct SdoJob {
        int        node = 0;
        quint16    index = 0;
        quint8     sub = 0;
        bool       upload = true;
        ValueType  type = T_HEX;
        QByteArray data;              /* download: data to send; upload: data received */
        int        pos = 0;           /* download: bytes sent so far */
        bool       toggle = false;
        bool       segmented = false;
        QString    label;             /* name shown in the log, e.g. "device name" */
    };

    void send(quint32 id, const QByteArray &data);
    void sendNmt(quint8 cmd);
    void handleSdoResponse(const QByteArray &d);
    void startNextJob();
    void finishJob(const QString &result, bool ok);
    void sdoAbort(quint32 code);
    void log(const QString &line);
    bool parseObject(int &node, quint16 &index, quint8 &sub);
    QByteArray encodeValue(ValueType t, const QString &text, bool &ok) const;
    static QString formatValue(ValueType t, const QByteArray &d);
    static QString abortText(quint32 code);
    static QString emcyText(quint16 code);
    static QString stateText(int state);

    int currentBus() const;

    QSpinBox       *busSpin;
    QTableWidget   *nodeTable;
    QSpinBox       *nmtNode;
    QSpinBox       *sdoNode;
    QLineEdit      *indexEdit;
    QLineEdit      *subEdit;
    QComboBox      *typeCombo;
    QLineEdit      *valueEdit;
    QLabel         *resultLabel;
    QPlainTextEdit *logView;

    QMap<int, Node> nodes;
    QList<SdoJob>   queue;
    SdoJob          job;
    bool            jobActive = false;
    QTimer          sdoTimer;
    QTimer          refreshTimer;
    QElapsedTimer   clock;
};

#endif // CANOPENWINDOW_H
