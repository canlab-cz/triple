// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "canopenwindow.h"

#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "connections/canconmanager.h"

namespace {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
const auto DATA_FRAME = CommFrame::CANDataFrame;
inline int frameBus(const CANopenFrame &f) { return f.getBus(); }
#else
const auto DATA_FRAME = QCanBusFrame::DataFrame;
inline int frameBus(const CANopenFrame &f) { return f.bus; }
#endif

const int SDO_TIMEOUT_MS = 1000;

/* NMT command specifiers (CiA 301) */
enum { NMT_START = 0x01, NMT_STOP = 0x02, NMT_PREOP = 0x80, NMT_RESET_NODE = 0x81, NMT_RESET_COMM = 0x82 };

quint16 le16(const QByteArray &d, int i) { return quint8(d[i]) | (quint16(quint8(d[i + 1])) << 8); }
quint32 le32(const QByteArray &d, int i) { return le16(d, i) | (quint32(le16(d, i + 2)) << 16); }
QString hex(quint32 v, int w) { return QString::number(v, 16).toUpper().rightJustified(w, '0'); }
}

CANopenWindow::CANopenWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("CANopen"));
    resize(980, 720);

    /* --- bus + NMT ------------------------------------------------------ */
    busSpin = new QSpinBox(this);
    busSpin->setRange(0, 63);

    nmtNode = new QSpinBox(this);
    nmtNode->setRange(0, 127);
    nmtNode->setSpecialValueText(tr("all nodes"));
    nmtNode->setToolTip(tr("Node ID for NMT commands, 0 = all nodes"));

    QHBoxLayout *nmtRow = new QHBoxLayout;
    nmtRow->addWidget(new QLabel(tr("Bus"), this));
    nmtRow->addWidget(busSpin);
    nmtRow->addSpacing(16);
    nmtRow->addWidget(new QLabel(tr("NMT to node"), this));
    nmtRow->addWidget(nmtNode);
    const struct { const char *text; quint8 cmd; } nmtButtons[] = {
        { "Start", NMT_START }, { "Stop", NMT_STOP }, { "Pre-operational", NMT_PREOP },
        { "Reset node", NMT_RESET_NODE }, { "Reset communication", NMT_RESET_COMM },
    };
    for (const auto &b : nmtButtons)
    {
        QPushButton *btn = new QPushButton(tr(b.text), this);
        quint8 cmd = b.cmd;
        connect(btn, &QPushButton::clicked, this, [this, cmd]() { sendNmt(cmd); });
        nmtRow->addWidget(btn);
    }
    QPushButton *syncBtn = new QPushButton(tr("SYNC"), this);
    connect(syncBtn, &QPushButton::clicked, this, [this]() { send(0x080, QByteArray()); log(tr("SYNC sent")); });
    nmtRow->addWidget(syncBtn);
    nmtRow->addStretch(1);

    /* --- node table ----------------------------------------------------- */
    nodeTable = new QTableWidget(0, 6, this);
    nodeTable->setHorizontalHeaderLabels({ tr("Node"), tr("State"), tr("Last seen"), tr("Boot-ups"),
                                           tr("EMCY"), tr("Last emergency") });
    nodeTable->verticalHeader()->setVisible(false);
    nodeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    nodeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    nodeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    nodeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    nodeTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    connect(nodeTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        int id = nodeTable->item(row, 0)->text().toInt();
        sdoNode->setValue(id);
        nmtNode->setValue(id);
    });

    QGroupBox *nodeBox = new QGroupBox(tr("Nodes (from heartbeat, boot-up and EMCY messages)"), this);
    QVBoxLayout *nodeLayout = new QVBoxLayout(nodeBox);
    nodeLayout->addWidget(nodeTable);

    /* --- SDO client ----------------------------------------------------- */
    sdoNode = new QSpinBox(this);
    sdoNode->setRange(1, 127);
    indexEdit = new QLineEdit("1008", this);
    indexEdit->setToolTip(tr("Object index, hex"));
    indexEdit->setMaximumWidth(90);
    subEdit = new QLineEdit("0", this);
    subEdit->setToolTip(tr("Sub-index, hex"));
    subEdit->setMaximumWidth(60);
    typeCombo = new QComboBox(this);
    typeCombo->addItems({ "UNSIGNED8", "UNSIGNED16", "UNSIGNED32", "INTEGER8", "INTEGER16", "INTEGER32",
                          "VISIBLE_STRING", "hex bytes" });
    typeCombo->setCurrentIndex(T_STRING);
    valueEdit = new QLineEdit(this);
    valueEdit->setPlaceholderText(tr("value to write (decimal, 0x hex, text, or hex bytes)"));

    QPushButton *readBtn = new QPushButton(tr("Read"), this);
    QPushButton *writeBtn = new QPushButton(tr("Write"), this);
    QPushButton *identityBtn = new QPushButton(tr("Read identity"), this);
    identityBtn->setToolTip(tr("Reads 1000 device type, 1008 device name, 1009/100A versions, 1018 identity"));
    connect(readBtn, &QPushButton::clicked, this, &CANopenWindow::readClicked);
    connect(writeBtn, &QPushButton::clicked, this, &CANopenWindow::writeClicked);
    connect(identityBtn, &QPushButton::clicked, this, &CANopenWindow::identityClicked);

    resultLabel = new QLabel("-", this);
    resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QHBoxLayout *sdoRow1 = new QHBoxLayout;
    sdoRow1->addWidget(new QLabel(tr("Node"), this));
    sdoRow1->addWidget(sdoNode);
    sdoRow1->addWidget(new QLabel(tr("Index"), this));
    sdoRow1->addWidget(indexEdit);
    sdoRow1->addWidget(new QLabel(tr("Sub"), this));
    sdoRow1->addWidget(subEdit);
    sdoRow1->addWidget(new QLabel(tr("Type"), this));
    sdoRow1->addWidget(typeCombo);
    sdoRow1->addWidget(readBtn);
    sdoRow1->addWidget(identityBtn);
    sdoRow1->addStretch(1);
    QHBoxLayout *sdoRow2 = new QHBoxLayout;
    sdoRow2->addWidget(new QLabel(tr("Value"), this));
    sdoRow2->addWidget(valueEdit, 1);
    sdoRow2->addWidget(writeBtn);
    QHBoxLayout *sdoRow3 = new QHBoxLayout;
    sdoRow3->addWidget(new QLabel(tr("Result"), this));
    sdoRow3->addWidget(resultLabel, 1);

    QGroupBox *sdoBox = new QGroupBox(tr("SDO client"), this);
    QVBoxLayout *sdoLayout = new QVBoxLayout(sdoBox);
    sdoLayout->addLayout(sdoRow1);
    sdoLayout->addLayout(sdoRow2);
    sdoLayout->addLayout(sdoRow3);

    logView = new QPlainTextEdit(this);
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(2000);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(nmtRow);
    layout->addWidget(nodeBox, 2);
    layout->addWidget(sdoBox);
    layout->addWidget(logView, 2);

    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &CANopenWindow::framesReceived);
    sdoTimer.setSingleShot(true);
    connect(&sdoTimer, &QTimer::timeout, this, &CANopenWindow::sdoTimeout);
    connect(&refreshTimer, &QTimer::timeout, this, &CANopenWindow::refreshNodes);
    refreshTimer.start(500);
    clock.start();
}

int CANopenWindow::currentBus() const
{
    return busSpin->value();
}

void CANopenWindow::log(const QString &line)
{
    logView->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + line);
}

void CANopenWindow::send(quint32 id, const QByteArray &data)
{
    CANopenFrame f;
    f.setFrameType(DATA_FRAME);
    f.setExtendedFrameFormat(false);
    f.setFrameId(id);
    f.setPayload(data);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    f.setBus(currentBus());
    f.setReceived(false);
#else
    f.bus = currentBus();
    f.isReceived = false;
#endif
    CANConManager::getInstance()->sendFrame(f);
}

void CANopenWindow::sendNmt(quint8 cmd)
{
    quint8 node = quint8(nmtNode->value());
    QByteArray d(2, 0);
    d[0] = char(cmd);
    d[1] = char(node);
    send(0x000, d);
    QString what = cmd == NMT_START ? "start" : cmd == NMT_STOP ? "stop" : cmd == NMT_PREOP ? "pre-operational"
                 : cmd == NMT_RESET_NODE ? "reset node" : "reset communication";
    log(tr("NMT %1 -> %2").arg(what, node ? tr("node %1").arg(node) : tr("all nodes")));
}

/* ------------------------------------------------------------------ receive */

void CANopenWindow::framesReceived(CANConnection *conn, QVector<CANopenFrame> &frames)
{
    Q_UNUSED(conn);
    for (const CANopenFrame &f : frames)
    {
        if (frameBus(f) != currentBus() || f.hasExtendedFrameFormat() || f.frameType() != DATA_FRAME)
            continue;
        quint32 id = f.frameId();
        quint32 fn = id & 0x780;
        int node = id & 0x7F;
        QByteArray d = f.payload();

        if (fn == 0x700 && node && d.size() >= 1)               /* heartbeat / boot-up */
        {
            Node &n = nodes[node];
            int state = quint8(d[0]) & 0x7F;
            if (state == 0)
            {
                n.bootups++;
                log(tr("node %1 boot-up").arg(node));
            }
            else if (state != n.state)
                log(tr("node %1 %2").arg(node).arg(stateText(state)));
            n.state = state;
            n.lastSeenMs = clock.elapsed();
        }
        else if (fn == 0x080 && node && d.size() >= 3)          /* EMCY (0x080 alone is SYNC) */
        {
            Node &n = nodes[node];
            quint16 code = le16(d, 0);
            quint8 reg = quint8(d[2]);
            n.emcyCount++;
            n.lastSeenMs = clock.elapsed();
            n.lastEmcy = code == 0 ? tr("error reset / no error")
                                   : tr("%1 %2, register %3").arg(hex(code, 4), emcyText(code), hex(reg, 2));
            log(tr("node %1 EMCY %2").arg(node).arg(n.lastEmcy) + (d.size() > 3 ? "  data " + d.mid(3).toHex(' ').toUpper() : QString()));
        }
        else if (fn == 0x580 && jobActive && node == job.node && d.size() == 8)  /* SDO response */
        {
            handleSdoResponse(d);
        }
    }
}

void CANopenWindow::refreshNodes()
{
    nodeTable->setRowCount(nodes.size());
    int row = 0;
    qint64 now = clock.elapsed();
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it, ++row)
    {
        const Node &n = it.value();
        double ago = n.lastSeenMs < 0 ? -1 : (now - n.lastSeenMs) / 1000.0;
        const QString cells[6] = {
            QString::number(it.key()),
            stateText(n.state),
            ago < 0 ? QString("-") : (ago < 3 ? tr("%1 s ago").arg(ago, 0, 'f', 1) : tr("silent %1 s").arg(ago, 0, 'f', 0)),
            QString::number(n.bootups),
            QString::number(n.emcyCount),
            n.lastEmcy,
        };
        for (int c = 0; c < 6; c++)
        {
            QTableWidgetItem *item = nodeTable->item(row, c);
            if (!item)
            {
                item = new QTableWidgetItem;
                nodeTable->setItem(row, c, item);
            }
            item->setText(cells[c]);
        }
        nodeTable->item(row, 2)->setForeground(ago >= 3 ? QColor(0xd6, 0x27, 0x28) : palette().color(QPalette::Text));
    }
}

/* --------------------------------------------------------------- SDO client */

bool CANopenWindow::parseObject(int &node, quint16 &index, quint8 &sub)
{
    bool ok1, ok2;
    node = sdoNode->value();
    index = quint16(indexEdit->text().trimmed().remove("0x", Qt::CaseInsensitive).toUInt(&ok1, 16));
    sub = quint8(subEdit->text().trimmed().remove("0x", Qt::CaseInsensitive).toUInt(&ok2, 16));
    if (!ok1 || !ok2)
    {
        resultLabel->setText(tr("index and sub-index are hex numbers, e.g. 1018 and 1"));
        return false;
    }
    return true;
}

void CANopenWindow::readClicked()
{
    SdoJob j;
    if (!parseObject(j.node, j.index, j.sub))
        return;
    j.upload = true;
    j.type = ValueType(typeCombo->currentIndex());
    queue.append(j);
    startNextJob();
}

void CANopenWindow::writeClicked()
{
    SdoJob j;
    if (!parseObject(j.node, j.index, j.sub))
        return;
    bool ok = false;
    j.upload = false;
    j.type = ValueType(typeCombo->currentIndex());
    j.data = encodeValue(j.type, valueEdit->text(), ok);
    if (!ok || j.data.isEmpty())
    {
        resultLabel->setText(tr("value does not fit the selected type"));
        return;
    }
    queue.append(j);
    startNextJob();
}

void CANopenWindow::identityClicked()
{
    const struct { quint16 index; quint8 sub; ValueType type; const char *label; } objs[] = {
        { 0x1000, 0, T_U32, "device type" },
        { 0x1008, 0, T_STRING, "device name" },
        { 0x1009, 0, T_STRING, "hardware version" },
        { 0x100A, 0, T_STRING, "software version" },
        { 0x1018, 1, T_U32, "vendor ID" },
        { 0x1018, 2, T_U32, "product code" },
        { 0x1018, 3, T_U32, "revision" },
        { 0x1018, 4, T_U32, "serial number" },
    };
    for (const auto &o : objs)
    {
        SdoJob j;
        j.node = sdoNode->value();
        j.index = o.index;
        j.sub = o.sub;
        j.type = o.type;
        j.upload = true;
        j.label = o.label;
        queue.append(j);
    }
    startNextJob();
}

void CANopenWindow::startNextJob()
{
    if (jobActive || queue.isEmpty())
        return;
    job = queue.takeFirst();
    jobActive = true;

    QByteArray d(8, 0);
    d[1] = char(job.index & 0xFF);
    d[2] = char(job.index >> 8);
    d[3] = char(job.sub);
    if (job.upload)
    {
        d[0] = char(0x40);                                     /* initiate upload */
    }
    else if (job.data.size() <= 4)
    {
        int n = 4 - job.data.size();
        d[0] = char(0x23 | (n << 2));                          /* expedited download, size indicated */
        for (int i = 0; i < job.data.size(); i++)
            d[4 + i] = job.data[i];
    }
    else
    {
        job.segmented = true;
        quint32 size = quint32(job.data.size());
        d[0] = char(0x21);                                     /* segmented download, size indicated */
        d[4] = char(size & 0xFF);
        d[5] = char((size >> 8) & 0xFF);
        d[6] = char((size >> 16) & 0xFF);
        d[7] = char((size >> 24) & 0xFF);
    }
    send(0x600 + job.node, d);
    sdoTimer.start(SDO_TIMEOUT_MS);
}

void CANopenWindow::handleSdoResponse(const QByteArray &d)
{
    quint8 cs = quint8(d[0]);
    sdoTimer.start(SDO_TIMEOUT_MS);

    if (cs == 0x80)
    {
        finishJob(tr("abort %1: %2").arg(hex(le32(d, 4), 8), abortText(le32(d, 4))), false);
        return;
    }

    if (job.upload)
    {
        if (!job.segmented && (cs & 0xE0) == 0x40)            /* initiate upload response */
        {
            if (le16(d, 1) != job.index || quint8(d[3]) != job.sub)
                return;
            if (cs & 0x02)                                     /* expedited */
            {
                int n = (cs & 0x01) ? 4 - ((cs >> 2) & 0x03) : 4;
                job.data = d.mid(4, n);
                finishJob(formatValue(job.type, job.data), true);
                return;
            }
            job.segmented = true;                              /* segmented: ask for the first segment */
            job.toggle = false;
            QByteArray r(8, 0);
            r[0] = char(0x60);
            send(0x600 + job.node, r);
            return;
        }
        if (job.segmented && (cs & 0xE0) == 0x00)              /* upload segment */
        {
            bool toggle = cs & 0x10;
            if (toggle != job.toggle)
            {
                sdoAbort(0x05030000);
                finishJob(tr("toggle bit error"), false);
                return;
            }
            int n = 7 - ((cs >> 1) & 0x07);
            job.data.append(d.mid(1, n));
            if (cs & 0x01)                                     /* last segment */
            {
                finishJob(formatValue(job.type, job.data), true);
                return;
            }
            job.toggle = !job.toggle;
            QByteArray r(8, 0);
            r[0] = char(0x60 | (job.toggle ? 0x10 : 0));
            send(0x600 + job.node, r);
        }
        return;
    }

    /* download */
    if (cs == 0x60 && !job.segmented)                          /* expedited download confirmed */
    {
        finishJob(tr("written"), true);
        return;
    }
    if (job.segmented && (cs == 0x60 || (cs & 0xE0) == 0x20))
    {
        if (cs != 0x60)                                        /* segment response: check toggle */
        {
            bool toggle = cs & 0x10;
            if (toggle != job.toggle)
            {
                sdoAbort(0x05030000);
                finishJob(tr("toggle bit error"), false);
                return;
            }
            job.toggle = !job.toggle;
        }
        if (job.pos >= job.data.size())
        {
            finishJob(tr("written (%1 bytes, segmented)").arg(job.data.size()), true);
            return;
        }
        int n = qMin(7, job.data.size() - job.pos);
        bool last = job.pos + n >= job.data.size();
        QByteArray r(8, 0);
        r[0] = char((job.toggle ? 0x10 : 0) | ((7 - n) << 1) | (last ? 0x01 : 0));
        for (int i = 0; i < n; i++)
            r[1 + i] = job.data[job.pos + i];
        job.pos += n;
        send(0x600 + job.node, r);
    }
}

void CANopenWindow::sdoAbort(quint32 code)
{
    QByteArray d(8, 0);
    d[0] = char(0x80);
    d[1] = char(job.index & 0xFF);
    d[2] = char(job.index >> 8);
    d[3] = char(job.sub);
    d[4] = char(code & 0xFF);
    d[5] = char((code >> 8) & 0xFF);
    d[6] = char((code >> 16) & 0xFF);
    d[7] = char((code >> 24) & 0xFF);
    send(0x600 + job.node, d);
}

void CANopenWindow::sdoTimeout()
{
    if (!jobActive)
        return;
    sdoAbort(0x05040000);
    finishJob(tr("timeout, no response"), false);
}

void CANopenWindow::finishJob(const QString &result, bool ok)
{
    sdoTimer.stop();
    QString obj = QString("%1:%2").arg(hex(job.index, 4), hex(job.sub, 2));
    QString line = tr("node %1 %2 %3%4 -> %5").arg(job.node).arg(job.upload ? "read" : "write", obj,
                                                    job.label.isEmpty() ? QString() : " (" + job.label + ")", result);
    log((ok ? "" : "ERROR ") + line);
    resultLabel->setText(line);
    jobActive = false;
    startNextJob();
}

/* -------------------------------------------------------------- formatting */

QByteArray CANopenWindow::encodeValue(ValueType t, const QString &text, bool &ok) const
{
    QString s = text.trimmed();
    ok = true;
    if (t == T_STRING)
        return s.toLatin1();
    if (t == T_HEX)
    {
        QByteArray b = QByteArray::fromHex(s.remove(' ').remove("0x", Qt::CaseInsensitive).toLatin1());
        ok = !b.isEmpty();
        return b;
    }
    qint64 v = s.startsWith("0x", Qt::CaseInsensitive) ? s.mid(2).toLongLong(&ok, 16) : s.toLongLong(&ok, 10);
    if (!ok)
        return QByteArray();
    int size = (t == T_U8 || t == T_I8) ? 1 : (t == T_U16 || t == T_I16) ? 2 : 4;
    bool sign = t == T_I8 || t == T_I16 || t == T_I32;
    qint64 lo = sign ? -(qint64(1) << (8 * size - 1)) : 0;
    qint64 hi = sign ? (qint64(1) << (8 * size - 1)) - 1 : (qint64(1) << (8 * size)) - 1;
    if (v < lo || v > hi)
    {
        ok = false;
        return QByteArray();
    }
    QByteArray b(size, 0);
    for (int i = 0; i < size; i++)
        b[i] = char((quint64(v) >> (8 * i)) & 0xFF);
    return b;
}

QString CANopenWindow::formatValue(ValueType t, const QByteArray &d)
{
    auto num = [&](int size, bool sign) -> QString {
        if (d.size() < size)
            return QString("%1 (short reply: %2)").arg(QString(d.toHex(' ').toUpper())).arg(d.size());
        quint64 v = 0;
        for (int i = 0; i < size; i++)
            v |= quint64(quint8(d[i])) << (8 * i);
        if (sign && (v & (quint64(1) << (8 * size - 1))))
            return QString::number(qint64(v) - (qint64(1) << (8 * size)));
        return QString("%1 (0x%2)").arg(v).arg(hex(quint32(v), 2 * size));
    };
    switch (t)
    {
    case T_U8:  return num(1, false);
    case T_U16: return num(2, false);
    case T_U32: return num(4, false);
    case T_I8:  return num(1, true);
    case T_I16: return num(2, true);
    case T_I32: return num(4, true);
    case T_STRING: return "\"" + QString::fromLatin1(d).trimmed() + "\"";
    default:    return QString(d.toHex(' ').toUpper());
    }
}

QString CANopenWindow::stateText(int state)
{
    switch (state)
    {
    case -1:   return "-";
    case 0:    return "boot-up";
    case 4:    return "stopped";
    case 5:    return "operational";
    case 0x7F: return "pre-operational";
    default:   return QString("unknown (0x%1)").arg(hex(quint32(state), 2));
    }
}

QString CANopenWindow::abortText(quint32 code)
{
    switch (code)
    {
    case 0x05030000: return "toggle bit not alternated";
    case 0x05040000: return "SDO protocol timed out";
    case 0x05040001: return "command specifier not valid or unknown";
    case 0x05040005: return "out of memory";
    case 0x06010000: return "unsupported access to an object";
    case 0x06010001: return "attempt to read a write only object";
    case 0x06010002: return "attempt to write a read only object";
    case 0x06020000: return "object does not exist in the object dictionary";
    case 0x06040041: return "object cannot be mapped to the PDO";
    case 0x06040047: return "general internal incompatibility in the device";
    case 0x06060000: return "access failed due to a hardware error";
    case 0x06070010: return "data type does not match, length of service parameter does not match";
    case 0x06070012: return "data type does not match, length too high";
    case 0x06070013: return "data type does not match, length too low";
    case 0x06090011: return "sub-index does not exist";
    case 0x06090030: return "invalid value for parameter";
    case 0x06090031: return "value of parameter written too high";
    case 0x06090032: return "value of parameter written too low";
    case 0x08000000: return "general error";
    case 0x08000020: return "data cannot be transferred or stored to the application";
    case 0x08000021: return "data cannot be transferred because of local control";
    case 0x08000022: return "data cannot be transferred because of the present device state";
    default:         return "unknown abort code";
    }
}

QString CANopenWindow::emcyText(quint16 code)
{
    switch (code)
    {
    case 0x8110: return "CAN overrun (objects lost)";
    case 0x8120: return "CAN in error passive mode";
    case 0x8130: return "life guard or heartbeat error";
    case 0x8140: return "recovered from bus off";
    case 0x8150: return "CAN-ID collision";
    case 0x8210: return "PDO not processed due to length error";
    case 0x8220: return "PDO length exceeded";
    default: break;
    }
    switch (code >> 12)
    {
    case 0x1: return "generic error";
    case 0x2: return "current";
    case 0x3: return "voltage";
    case 0x4: return "temperature";
    case 0x5: return "device hardware";
    case 0x6: return "device software";
    case 0x7: return "additional modules";
    case 0x8: return "monitoring / communication";
    case 0x9: return "external error";
    case 0xF: return (code >> 8) == 0xFF ? "device specific" : "additional functions";
    default:  return "";
    }
}
