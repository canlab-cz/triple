#include <QDebug>
#include <QDateTime>
#include <QSerialPortInfo>
#include <QStringBuilder>
#include <QMetaMethod>

#include "triple_serial.h"
#include "canconmanager.h"
#include "utility.h"

/* SavvyCAN's data rate combo offers 1M, 2M, 4M, 5M; the adapter also does
 * 250k .. 10M. Anything not in the firmware table is mapped to the next
 * supported value above it. */
static uint32_t nearestSupportedDataRate(uint32_t nominal, uint32_t wanted)
{
    static const uint32_t rates[] = { 250000, 500000, 833333, 1000000, 1500000, 2000000, 3000000,
                                      4000000, 5000000, 6700000, 8000000, 10000000 };
    for (uint32_t r : rates)
        if (r >= wanted && r > nominal)
            return r;
    return 10000000;
}

TripleSerial::TripleSerial(QString portName) :
    CANConnection(portName, "USB2CAN Triple", CANCon::TRIPLE, 115200, 0, false, 0, TRIPLE_PORTS, 4000, true)
{
    serial = nullptr;
    rxCount = 0;
    rxEscaped = false;
    rxResync = false;
    tsBaseMicros = 0;
    tsLast = 0;
    tsWrapAdd = 0;
    tsHaveBase = false;
    for (int i = 0; i < TRIPLE_PORTS; i++)
    {
        lastTec[i] = lastRec[i] = lastState[i] = 0;
        busState[i] = 0;
    }

    /* defaults matching the Linux driver's README: 250k, 500k, FD 125k / 6.7M */
    CANBus bus;
    bus.setActive(true);
    bus.setListenOnly(false);
    bus.setSpeed(250000);
    setBusConfig(0, bus);
    bus.setSpeed(500000);
    setBusConfig(1, bus);
    bus.setSpeed(125000);
    bus.setCanFD(true);
    bus.setDataRate(6700000);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bus.setCanFDCapable(true); /* QT6WIP: makes the connection window show the FD controls for this bus */
#endif
    setBusConfig(2, bus);

    sendDebug("TripleSerial()");
}

TripleSerial::~TripleSerial()
{
    stop();
    sendDebug("~TripleSerial()");
}

/* hex dumps are only built when the connection window's console listens */
bool TripleSerial::consoleWanted()
{
    static const QMetaMethod sig = QMetaMethod::fromSignal(&CANConnection::debugOutput);
    return isSignalConnected(sig);
}

void TripleSerial::sendDebug(const QString &text)
{
    /* qInfo: SavvyCAN release builds compile qDebug out (QT_NO_DEBUG_OUTPUT) */
    qInfo() << text;
    emit debugOutput(text);
}

void TripleSerial::sendToSerial(const QByteArray &bytes)
{
    if (!serial || !serial->isOpen())
    {
        sendDebug("Triple: write attempted while the serial port is not open");
        return;
    }
    if (consoleWanted())
    {
        QString s = "Triple TX ->";
        for (unsigned char b : bytes)
            s = s % " " % QString::number(b, 16).rightJustified(2, '0');
        sendDebug(s);
    }
    serial->write(bytes);
}

/* ---------------------------------------------------------------- lifecycle */

void TripleSerial::piStarted()
{
    connectDevice();
}

void TripleSerial::piStop()
{
    disconnectDevice();
}

void TripleSerial::piSuspend(bool pSuspend)
{
    setCapSuspended(pSuspend);
    if (isCapSuspended())
        getQueue().flush();
}

void TripleSerial::connectDevice()
{
    if (serial)
        disconnectDevice();

    serial = new QSerialPort(QSerialPortInfo(getPort()));

    /* CDC ACM: the line settings are irrelevant, but QSerialPort wants them */
    serial->setBaudRate(115200);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);

    /* The error signal is connected only after a successful open: open()
     * emits it synchronously, and serialError() would delete the port
     * while we are still using it here. */
    if (!serial->open(QIODevice::ReadWrite))
    {
        QString why = serial->errorString();
        sendDebug("Triple: cannot open " + getPort() + ": " + why);
        disconnectDevice();
        return;
    }
    connect(serial, SIGNAL(readyRead()), this, SLOT(readSerialData()));
    connect(serial, SIGNAL(errorOccurred(QSerialPort::SerialPortError)), this, SLOT(serialError(QSerialPort::SerialPortError)));
    serial->setDataTerminalReady(true);

    rxCount = 0;
    rxEscaped = false;
    rxResync = false;
    tsHaveBase = false;
    tsWrapAdd = 0;
    tsLast = 0;
    fwVersion.clear();

    uint8_t cmd[64];
    int n;
    n = triple_encode_buffer_mode(cmd, 1);
    sendToSerial(QByteArray((const char *)cmd, n));
    n = triple_encode_timestamp_mode(cmd, true);
    sendToSerial(QByteArray((const char *)cmd, n));
    for (int i = 0; i < TRIPLE_PORTS; i++)
        sendBusSettings(i);
    n = triple_encode_fw_version_req(cmd);
    sendToSerial(QByteArray((const char *)cmd, n));

    setStatus(CANCon::CONNECTED);
    CANConStatus stats;
    stats.conStatus = getStatus();
    stats.numHardwareBuses = TRIPLE_PORTS;
    emit status(stats);

    sendDebug("Triple: connected on " + getPort());
}

void TripleSerial::disconnectDevice()
{
    if (serial)
    {
        if (serial->isOpen())
            serial->close();
        serial->disconnect();
        delete serial;
        serial = nullptr;
    }

    setStatus(CANCon::NOT_CONNECTED);
    CANConStatus stats;
    stats.conStatus = getStatus();
    stats.numHardwareBuses = TRIPLE_PORTS;
    emit status(stats);
}

void TripleSerial::serialError(QSerialPort::SerialPortError err)
{
    if (err == QSerialPort::NoError)
        return;
    QString why = serial ? serial->errorString() : QString("port already closed");
    sendDebug("Triple: serial error " + QString::number(err) + ": " + why);
    switch (err)
    {
    case QSerialPort::DeviceNotFoundError:
    case QSerialPort::PermissionError:
    case QSerialPort::OpenError:
    case QSerialPort::ResourceError:
    case QSerialPort::NotOpenError:
    case QSerialPort::UnknownError:
        piStop();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- bus settings */

bool TripleSerial::piGetBusSettings(int pBusIdx, CANBus& pBus)
{
    return getBusConfig(pBusIdx, pBus);
}

void TripleSerial::piSetBusSettings(int pBusIdx, CANBus bus)
{
    if (pBusIdx < 0 || pBusIdx >= TRIPLE_PORTS)
        return;
    setBusConfig(pBusIdx, bus);
    if (serial && serial->isOpen())
        sendBusSettings(pBusIdx);
}

void TripleSerial::sendBusSettings(int busIdx)
{
    CANBus bus;
    if (!getBusConfig(busIdx, bus))
        return;

    /* an inactive bus is kept silent by switching it to listen only */
    bool listen = bus.isListenOnly() || !bus.isActive();
    uint8_t cmd[64];
    int n;

    if (busIdx < 2)
    {
        uint16_t kbit = triple_can_speed_kbit(bus.getSpeed());
        if (!kbit)
        {
            sendDebug("Triple: unsupported bitrate " + QString::number(bus.getSpeed()) + " on CAN" + QString::number(busIdx + 1));
            return;
        }
        n = triple_encode_settings_can(cmd, busIdx + 1, kbit, listen);
        sendDebug("Triple: CAN" + QString::number(busIdx + 1) + " " + QString::number(kbit) + " kbit/s" + (listen ? " listen only" : ""));
    }
    else
    {
        uint32_t nominal = bus.getSpeed();
        uint32_t data = bus.isCanFD() ? nearestSupportedDataRate(nominal, bus.getDataRate()) : triple_fd_default_data_bitrate(nominal);
        uint32_t code = triple_fd_speed_code(nominal, data);
        if (!code)
        {
            sendDebug("Triple: unsupported CAN3 bitrate pair " + QString::number(nominal) + " / " + QString::number(data) +
                      " (nominal 125k 250k 500k 1M, data above nominal)");
            return;
        }
        n = triple_encode_settings_fd(cmd, code, listen, true, false);
        sendDebug("Triple: CAN3 FD " + QString::number(nominal) + " / " + QString::number(data) + " bit/s" + (listen ? " listen only" : ""));
    }
    sendToSerial(QByteArray((const char *)cmd, n));
    setConfigured(busIdx, true);
}

/* ------------------------------------------------------------------ transmit */

bool TripleSerial::piSendFrame(const TripleGuiFrame& frame)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int busIdx = frame.getBus();
#else
    int busIdx = frame.bus;
#endif
    if (!serial || !serial->isOpen())
        return false;
    if (busIdx < 0 || busIdx >= TRIPLE_PORTS)
        return false;
    if (frame.frameType() == TripleGuiFrame::ErrorFrame)
        return true;

    CANBus bus;
    if (getBusConfig(busIdx, bus) && (bus.isListenOnly() || !bus.isActive()))
        return false;

    triple_frame f;
    memset(&f, 0, sizeof(f));
    f.port = busIdx;
    f.eff = frame.hasExtendedFrameFormat();
    f.rtr = frame.frameType() == TripleGuiFrame::RemoteRequestFrame;
    f.fd = frame.hasFlexibleDataRateFormat();
    f.brs = f.fd && frame.hasBitrateSwitch();
    f.esi = f.fd && frame.hasErrorStateIndicator();
    f.id = frame.frameId() & (f.eff ? 0x1FFFFFFFu : 0x7FFu);

    QByteArray payload = frame.payload();
    int maxLen = f.fd ? TRIPLE_MAX_DATA : 8;
    if (payload.size() > maxLen)
    {
        sendDebug("Triple: frame has too much data for " + QString(f.fd ? "CAN FD" : "CAN 2.0") + ", dropped");
        return false;
    }
    if (f.fd && busIdx != 2)
    {
        sendDebug("Triple: CAN FD frames can only be sent on CAN3, dropped");
        return false;
    }
    f.len = payload.size();
    memcpy(f.data, payload.constData(), f.len);

    uint8_t wire[TRIPLE_MTU];
    int n = triple_encode_frame(&f, wire, sizeof(wire));
    if (n < 0)
        return false;
    if (n > TRIPLE_FW_MAX_WIRE_LEN)
    {
        /* longer messages lock the adapter firmware up until power cycle */
        sendDebug("Triple: frame is " + QString::number(n) + " bytes on the wire, above the adapter limit of " +
                  QString::number(TRIPLE_FW_MAX_WIRE_LEN) + ", dropped");
        return false;
    }
    sendToSerial(QByteArray((const char *)wire, n));
    return true;
}

/* ------------------------------------------------------------------- receive */

void TripleSerial::readSerialData()
{
    if (!serial)
        return;
    QByteArray data = serial->readAll();

    for (int i = 0; i < data.size(); i++)
    {
        uint8_t b = (uint8_t)data.at(i);

        if (rxResync)
        {
            if (b == TRIPLE_LAST_BYTE)
            {
                rxResync = false;
                rxEscaped = false;
                rxCount = 0;
            }
            continue;
        }
        if (rxCount >= TRIPLE_MTU)
        {
            rxResync = true;
            rxCount = 0;
            continue;
        }
        if (rxCount == 0 && b != TRIPLE_FIRST_BYTE)
            continue;
        if (rxCount == 1)
        {
            rxBuf[rxCount++] = b;       /* raw length byte, never escaped */
            continue;
        }
        bool wasEscaped = rxEscaped;
        rxEscaped = false;
        rxBuf[rxCount++] = b;
        if (wasEscaped)
            continue;
        if (b == TRIPLE_ESC_BYTE)
            rxEscaped = true;
        else if (b == TRIPLE_LAST_BYTE)
        {
            processMessage(rxBuf, rxCount);
            rxCount = 0;
        }
    }
}

void TripleSerial::processMessage(const uint8_t *wire, int len)
{
    if (consoleWanted())
    {
        QString s = "Triple RX <-";
        for (int i = 0; i < len; i++)
            s = s % " " % QString::number(wire[i], 16).rightJustified(2, '0');
        sendDebug(s);
    }

    triple_msg m;
    int type = triple_decode(wire, len, &m);
    if (type < 0)
    {
        sendDebug("Triple: dropped message, decode error " + QString::number(type));
        return;
    }
    switch (type)
    {
    case TRIPLE_MSG_FRAME:
        handleFrame(m.frame);
        break;
    case TRIPLE_MSG_STATUS:
        handleStatus(m.status);
        break;
    case TRIPLE_MSG_FW_VER:
        if (fwVersion != QString(m.fw_version))
        {
            fwVersion = m.fw_version;
            sendDebug("Triple: firmware " + fwVersion);
            emit deviceInfo(fwVersion.toDouble() * 100, 0);
        }
        break;
    }
}

/* SavvyCAN convention (see gvretserial.cpp): frames carry microseconds
 * relative to CANConManager::getTimeBasis() in the TimeStamp's microSeconds
 * field with seconds = 0. The adapter clock is tied to that basis at the
 * first frame and re-tied whenever the manager resets its basis. */
qint64 TripleSerial::adapterTimeToMicros(uint32_t ts)
{
    qint64 managerBasis = (qint64)CANConManager::getInstance()->getTimeBasis();

    if (tsHaveBase && ts < tsLast)
        tsWrapAdd += 1ull << 32;
    tsLast = ts;
    quint64 unwrapped = tsWrapAdd + ts;

    if (!tsHaveBase || managerBasis != tsManagerBasis)
    {
        qint64 nowRelative = QDateTime::currentMSecsSinceEpoch() * 1000ll - managerBasis;
        tsBaseMicros = nowRelative - (qint64)unwrapped;
        tsManagerBasis = managerBasis;
        tsHaveBase = true;
    }
    return tsBaseMicros + (qint64)unwrapped;
}

void TripleSerial::handleFrame(const triple_frame &f)
{
    if (isCapSuspended())
        return;

    TripleGuiFrame frame;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    frame.setBus(f.port);
    frame.setReceived(true);
#else
    frame.bus = f.port;
    frame.isReceived = true;
#endif
    frame.setFrameId(f.id);
    frame.setExtendedFrameFormat(f.eff);
    frame.setFrameType(f.rtr ? TripleGuiFrame::RemoteRequestFrame : TRIPLE_GUI_DATA_FRAME);
    if (f.fd)
    {
        frame.setFlexibleDataRateFormat(true);
        frame.setBitrateSwitch(f.brs);
        frame.setErrorStateIndicator(f.esi);
    }
    frame.setPayload(QByteArray((const char *)f.data, f.len));

    if (useSystemTime)
        frame.setTimeStamp(TripleGuiFrame::TimeStamp::fromMicroSeconds(QDateTime::currentMSecsSinceEpoch() * 1000ll));
    else if (f.has_ts)
        frame.setTimeStamp(TripleGuiFrame::TimeStamp(0, adapterTimeToMicros(f.ts)));
    else
        frame.setTimeStamp(TripleGuiFrame::TimeStamp(0, hostTimeToMicros()));

    TripleGuiFrame *slot = getQueue().get();
    if (!slot)
    {
        sendDebug("Triple: receive queue full, frame lost");
        return;
    }
    *slot = frame;
    checkTargettedFrame(frame);
    getQueue().queue();
}

qint64 TripleSerial::hostTimeToMicros()
{
    return QDateTime::currentMSecsSinceEpoch() * 1000ll - (qint64)CANConManager::getInstance()->getTimeBasis();
}

/* Bus state from the adapter's counters, same thresholds as the Linux
 * driver and the CAN specification: warning at 96, passive at 128. The
 * firmware's own state byte (0 active, 1 passive) is honoured too, and
 * anything above 1 is taken as bus off (not observed on hardware yet). */
static int busStateFrom(const triple_port_status &ps)
{
    if (ps.state >= 2)
        return 3;
    int cnt = ps.tec > ps.rec ? ps.tec : ps.rec;
    if (ps.state == 1 || cnt >= 128)
        return 2;
    if (cnt >= 96)
        return 1;
    return 0;
}

void TripleSerial::handleStatus(const triple_status &st)
{
    static const char *stateName[] = { "error active", "error warning", "error passive", "bus off" };
    for (int i = 0; i < TRIPLE_PORTS; i++)
    {
        const triple_port_status &ps = st.port[i];
        if (ps.tec == lastTec[i] && ps.rec == lastRec[i] && ps.state == lastState[i])
            continue;
        lastTec[i] = ps.tec;
        lastRec[i] = ps.rec;
        lastState[i] = ps.state;

        int s = busStateFrom(ps);
        sendDebug("Triple: CAN" + QString::number(i + 1) + " " + stateName[s] +
                  ", TEC " + QString::number(ps.tec) + " REC " + QString::number(ps.rec));
        if (s != busState[i])
        {
            queueErrorFrame(i, s, busState[i], ps.tec, ps.rec);
            busState[i] = s;
        }
    }
}

/* Error frame in SocketCAN layout (linux/can/error.h), which SavvyCAN
 * shows with its error flags: data[1] controller state, data[6] TEC,
 * data[7] REC. */
void TripleSerial::queueErrorFrame(int port, int state, int oldState, uint8_t tec, uint8_t rec)
{
    if (isCapSuspended())
        return;

    enum { CRTL_RX_WARNING = 0x04, CRTL_TX_WARNING = 0x08, CRTL_RX_PASSIVE = 0x10,
           CRTL_TX_PASSIVE = 0x20, CRTL_ACTIVE = 0x40 };
    uint8_t ctrl = 0;
    switch (state)
    {
    case 0: ctrl = CRTL_ACTIVE; break;
    case 1: ctrl = tec >= rec ? CRTL_TX_WARNING : CRTL_RX_WARNING; break;
    case 2: ctrl = tec >= rec ? CRTL_TX_PASSIVE : CRTL_RX_PASSIVE; break;
    default: break;
    }
    Q_UNUSED(oldState);

    QByteArray data(8, 0);
    data[1] = (char)ctrl;
    data[6] = (char)tec;
    data[7] = (char)rec;

    TripleGuiFrame frame;
    frame.setFrameType(TripleGuiFrame::ErrorFrame);
    frame.setError(state == 3 ? TripleGuiFrame::BusOffError : TripleGuiFrame::ControllerError);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    frame.setBus(port);
    frame.setReceived(true);
#else
    frame.bus = port;
    frame.isReceived = true;
#endif
    frame.setPayload(data);
    if (useSystemTime)
        frame.setTimeStamp(TripleGuiFrame::TimeStamp::fromMicroSeconds(QDateTime::currentMSecsSinceEpoch() * 1000ll));
    else
        frame.setTimeStamp(TripleGuiFrame::TimeStamp(0, hostTimeToMicros()));

    TripleGuiFrame *slot = getQueue().get();
    if (!slot)
        return;
    *slot = frame;
    getQueue().queue();
}

/* debugging data typed into the connection window console: send raw */
void TripleSerial::debugInput(QByteArray bytes)
{
    sendToSerial(bytes);
}
