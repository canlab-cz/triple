// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANOPENWINDOW_H
#define CANOPENWINDOW_H

/*
 * CANopen basics (CiA 301) for one bus: node monitor from heartbeat,
 * boot-up and EMCY messages, NMT master commands, SYNC, and an SDO client
 * (expedited and segmented upload/download) with readable abort codes,
 * and live PDO decoding with the mapping taken from an EDS/DCF file or
 * read from the node over SDO. Works with any SavvyCAN connection.
 */

#include <QWidget>
#include <QMap>
#include <QList>
#include <QTimer>
#include <QElapsedTimer>
#include <QByteArray>
#include <QHash>
#include <QVector>
#include <vector>

#include "can_structs.h"
#include "canopen_eds.h"

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
    void loadEdsClicked();
    void readMappingClicked();
    void refreshPdos();

private:
    enum ValueType { T_U8, T_U16, T_U32, T_I8, T_I16, T_I32, T_STRING, T_HEX };
    /* SDO reads issued by "Read mapping from node" */
    enum PdoStep { PDO_NONE, PDO_COB, PDO_COUNT, PDO_ENTRY };

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
        PdoStep    pdoStep = PDO_NONE;
        bool       pdoTx = true;
        int        pdoNum = 0;        /* 1-based PDO number */
    };

    struct PendingPdo {               /* mapping being read from the node */
        bool    haveCob = false;
        quint32 cob = 0;
        QVector<quint32> mapping;
    };

    struct PdoLive {                  /* last frame of one PDO */
        QByteArray data;
        int        count = 0;
        qint64     lastUs = -1;
        qint64     periodUs = -1;
        bool       dirty = false;
        int        row = 0;           /* first table row */
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

    bool loadEds(const QString &path);
    void rebuildPdosFromEds();
    void setPdos(const std::vector<canopen::Pdo> &list, const QString &source);
    void handlePdoJob(bool ok);
    void finishMappingRead();
    bool pdoJobsPending() const;

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
    QSpinBox       *pdoNode;
    QLabel         *pdoStatus;
    QTableWidget   *pdoTable;

    canopen::Eds    eds;
    bool            edsLoaded = false;
    QString         edsName;
    std::vector<canopen::Pdo> pdos;
    QVector<PdoLive> pdoLive;
    QHash<quint32, int> pdoByCob;     /* COB-ID -> index in pdos */
    QMap<int, PendingPdo> pending;    /* key: (tx ? 0 : 256) + number */
    bool            mappingRead = false;
    bool            pdosFromNode = false;  /* current mapping was read over SDO */
    QString         nodeSource;            /* "read from node 5: ..." */
    bool            mappingTimeout = false;
    QTimer          pdoTimer;

    QMap<int, Node> nodes;
    QList<SdoJob>   queue;
    SdoJob          job;
    bool            jobActive = false;
    QTimer          sdoTimer;
    QTimer          refreshTimer;
    QElapsedTimer   clock;
};

#endif // CANOPENWINDOW_H
