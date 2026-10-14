#ifndef TRIPLE_SERIAL_H
#define TRIPLE_SERIAL_H

/*
 * SavvyCAN connection for the CANLAB USB2CAN Triple adapter over its
 * CDC ACM serial port: three buses (CAN1, CAN2 classic, CAN3 CAN FD),
 * hardware receive timestamps, error counters.
 *
 * Protocol: see doc/protocol.md in the canlab-cz/triple repository.
 * Codec: triple_proto.[ch], shared with the Linux kernel driver.
 */

#include <QSerialPort>
#include <QByteArray>
#include <stdint.h>

#include "canconnection.h"
#include "triple_proto.h"

/* SavvyCAN master (Qt 5) passes CANFrame (a QCanBusFrame); the QT6WIP
 * branch (Qt 6) uses its own CommFrame. The two differ in a few names. */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef CommFrame TripleGuiFrame;
#define TRIPLE_GUI_DATA_FRAME CommFrame::CANDataFrame
#else
typedef CANFrame TripleGuiFrame;
#define TRIPLE_GUI_DATA_FRAME QCanBusFrame::DataFrame
#endif

class TripleSerial : public CANConnection
{
    Q_OBJECT

public:
    explicit TripleSerial(QString portName);
    virtual ~TripleSerial();

protected:
    virtual void piStarted();
    virtual void piStop();
    virtual void piSetBusSettings(int pBusIdx, CANBus pBus);
    virtual bool piGetBusSettings(int pBusIdx, CANBus& pBus);
    virtual void piSuspend(bool pSuspend);
    virtual bool piSendFrame(const TripleGuiFrame&);

public slots:
    void debugInput(QByteArray bytes);

private slots:
    void readSerialData();
    void serialError(QSerialPort::SerialPortError err);

private:
    void connectDevice();
    void disconnectDevice();
    void sendToSerial(const QByteArray &bytes);
    void sendDebug(const QString &text);
    bool consoleWanted();
    void sendBusSettings(int busIdx);
    void processMessage(const uint8_t *wire, int len);
    void handleFrame(const triple_frame &f);
    void handleStatus(const triple_status &st);
    qint64 adapterTimeToMicros(uint32_t ts);
    qint64 hostTimeToMicros();
    void queueErrorFrame(int port, int state, int oldState, uint8_t tec, uint8_t rec);

    QSerialPort *serial;

    /* receive stream splitter (same rules as the kernel driver) */
    uint8_t  rxBuf[TRIPLE_MTU];
    int      rxCount;
    bool     rxEscaped;
    bool     rxResync;

    /* adapter 32-bit microsecond clock, extended and tied to system time */
    qint64   tsBaseMicros;      /* manager-relative time (us) corresponding to adapter time 0 */
    qint64   tsManagerBasis;    /* manager basis the offset was computed against */
    uint32_t tsLast;
    quint64  tsWrapAdd;
    bool     tsHaveBase;

    uint8_t  lastTec[TRIPLE_PORTS];
    uint8_t  lastRec[TRIPLE_PORTS];
    uint8_t  lastState[TRIPLE_PORTS];
    int      busState[TRIPLE_PORTS];  /* 0 active, 1 warning, 2 passive, 3 bus off */
    QString  fwVersion;
};

#endif // TRIPLE_SERIAL_H
