// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "canopenwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <functional>

#include "connections/canconmanager.h"
#include "savvycompat.h"

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
        pdoNode->setValue(id);
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
    resultLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   /* long results must not widen the window */
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
    blockCheck = new QCheckBox(tr("Block transfer"), this);
    blockCheck->setToolTip(tr("SDO block transfer (fast for large objects, CRC checked); falls back to normal SDO "
                              "when the node does not support it"));
    sdoRow1->addWidget(blockCheck);
    sdoRow1->addStretch(1);
    QHBoxLayout *sdoRow2 = new QHBoxLayout;
    sdoRow2->addWidget(new QLabel(tr("Value"), this));
    sdoRow2->addWidget(valueEdit, 1);
    sdoRow2->addWidget(writeBtn);
    QPushButton *writeFileBtn = new QPushButton(tr("Write file..."), this);
    writeFileBtn->setToolTip(tr("Writes the bytes of a file to the object (e.g. a program download to 1F50:01)"));
    QPushButton *saveReadBtn = new QPushButton(tr("Save read data..."), this);
    sdoRow2->addWidget(writeFileBtn);
    sdoRow2->addWidget(saveReadBtn);
    connect(writeFileBtn, &QPushButton::clicked, this, &CANopenWindow::writeFileClicked);
    connect(saveReadBtn, &QPushButton::clicked, this, &CANopenWindow::saveReadClicked);
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

    /* --- PDOs ----------------------------------------------------------- */
    pdoNode = new QSpinBox(this);
    pdoNode->setRange(1, 127);
    pdoNode->setToolTip(tr("Node whose PDOs are decoded ($NODEID in the EDS)"));
    connect(pdoNode, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() {
        QSettings().setValue("CANopen/pdoNode", pdoNode->value());
        if (pdosFromNode)                  /* that mapping belongs to the previous node */
        {
            pdosFromNode = false;
            if (!edsLoaded)
                setPdos({}, tr("- node changed, read the mapping again"));
        }
        rebuildPdosFromEds();
    });
    QPushButton *edsBtn = new QPushButton(tr("Load EDS/DCF..."), this);
    QPushButton *mapBtn = new QPushButton(tr("Read mapping from node"), this);
    mapBtn->setToolTip(tr("Reads the PDO communication and mapping objects (1400..., 1600..., 1800..., 1A00...) over SDO"));
    connect(edsBtn, &QPushButton::clicked, this, &CANopenWindow::loadEdsClicked);
    connect(mapBtn, &QPushButton::clicked, this, &CANopenWindow::readMappingClicked);
    pdoStatus = new QLabel(tr("no PDO mapping: load an EDS/DCF file or read the mapping from the node"), this);

    QHBoxLayout *pdoRow = new QHBoxLayout;
    pdoRow->addWidget(new QLabel(tr("Node"), this));
    pdoRow->addWidget(pdoNode);
    pdoRow->addWidget(edsBtn);
    pdoRow->addWidget(mapBtn);
    pdoRow->addWidget(pdoStatus, 1);

    pdoTable = new QTableWidget(0, 9, this);
    pdoTable->setHorizontalHeaderLabels({ tr("COB-ID"), tr("PDO"), tr("Object"), tr("Bits"), tr("Name"),
                                          tr("Value"), tr("Raw"), tr("Frames"), tr("Period") });
    pdoTable->verticalHeader()->setVisible(false);
    pdoTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pdoTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pdoTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    QWidget *pdoPage = new QWidget(this);
    QVBoxLayout *pdoLayout = new QVBoxLayout(pdoPage);
    pdoLayout->addLayout(pdoRow);
    pdoLayout->addWidget(pdoTable);

    QTabWidget *tabs = new QTabWidget(this);
    tabs->addTab(logView, tr("Log"));
    tabs->addTab(pdoPage, tr("PDOs"));
    tabs->addTab(buildLssPage(), tr("LSS"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(nmtRow);
    layout->addWidget(nodeBox, 2);
    layout->addWidget(sdoBox);
    layout->addWidget(tabs, 3);

    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &CANopenWindow::framesReceived);
    sdoTimer.setSingleShot(true);
    connect(&sdoTimer, &QTimer::timeout, this, &CANopenWindow::sdoTimeout);
    connect(&refreshTimer, &QTimer::timeout, this, &CANopenWindow::refreshNodes);
    refreshTimer.start(500);
    connect(&pdoTimer, &QTimer::timeout, this, &CANopenWindow::refreshPdos);
    pdoTimer.start(200);
    clock.start();

    pdoNode->setValue(QSettings().value("CANopen/pdoNode", 1).toInt());
    QString lastEds = QSettings().value("CANopen/edsFile").toString();
    if (!lastEds.isEmpty() && QFileInfo::exists(lastEds))
        loadEds(lastEds);
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
        auto pdo = pdoByCob.constFind(id);
        if (pdo != pdoByCob.constEnd())
        {
            PdoLive &l = pdoLive[pdo.value()];
            qint64 us = guiFrameMicros(f);
            l.periodUs = l.lastUs >= 0 ? us - l.lastUs : -1;
            l.lastUs = us;
            l.data = f.payload();
            l.count++;
            l.dirty = true;
            continue;
        }
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
            if (job.block) handleBlockResponse(d);
            else handleSdoResponse(d);
        }
        else if (id == canopen::lss::SLAVE_COB && d.size() >= 1)               /* LSS answer */
        {
            lssReply(d);
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
    j.block = blockCheck->isChecked();
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
    j.block = blockCheck->isChecked();
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

    if (job.block)
    {
        if (job.upload)
        {
            job.blockUp = std::make_shared<canopen::BlockUpload>(job.index, job.sub);
            sendFrame8(0x600 + job.node, job.blockUp->start());
        }
        else
        {
            std::vector<uint8_t> bytes(job.data.begin(), job.data.end());
            job.blockDown = std::make_shared<canopen::BlockDownload>(job.index, job.sub, bytes);
            sendFrame8(0x600 + job.node, job.blockDown->start());
        }
        sdoTimer.start(SDO_TIMEOUT_MS);
        return;
    }

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

void CANopenWindow::sendFrame8(quint32 id, const canopen::Frame8 &f)
{
    send(id, QByteArray(reinterpret_cast<const char *>(f.d), 8));
}

void CANopenWindow::handleBlockResponse(const QByteArray &d)
{
    sdoTimer.start(SDO_TIMEOUT_MS);
    std::vector<canopen::Frame8> out;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(d.constData());
    canopen::BlockTransfer *t = job.upload ? static_cast<canopen::BlockTransfer *>(job.blockUp.get()) : job.blockDown.get();
    canopen::Step r = job.upload ? job.blockUp->feed(p, out) : job.blockDown->feed(p, out);
    for (const canopen::Frame8 &f : out)
        sendFrame8(0x600 + job.node, f);
    if (r == canopen::Step::Pending)
        return;
    if (r == canopen::Step::Done)
    {
        QString how = tr("block transfer, %1 blocks%2%3").arg(t->blocks).arg(t->crcUsed ? tr(", CRC ok") : QString())
                          .arg(t->retransmits ? tr(", %1 retransmitted").arg(t->retransmits) : QString());
        if (job.upload)
        {
            job.data = QByteArray(reinterpret_cast<const char *>(t->data.data()), int(t->data.size()));
            lastRead = job.data;
            lastReadName = QString("%1_%2").arg(hex(job.index, 4), hex(job.sub, 2));
            QString shown = job.data.size() > 48 ? tr("%1 bytes: %2 ...").arg(job.data.size()).arg(QString(job.data.left(32).toHex(' ').toUpper()))
                                                 : formatValue(job.type, job.data);
            finishJob(shown + " (" + how + ")", true);
        }
        else
            finishJob(tr("written, %1 bytes (%2)").arg(job.data.size()).arg(how), true);
        return;
    }
    /* the node does not know block transfers: same job as normal SDO */
    if (t->abortCode == 0x05040001 && t->blocks == 0)
    {
        log(tr("node %1 has no SDO block transfer (abort 05040001), using normal SDO").arg(job.node));
        job.block = false;
        job.blockUp.reset();
        job.blockDown.reset();
        queue.prepend(job);
        sdoTimer.stop();
        jobActive = false;
        startNextJob();
        return;
    }
    finishJob(tr("abort %1: %2%3").arg(hex(t->abortCode, 8), abortText(t->abortCode),
                                       t->error.empty() ? QString() : " (" + QString::fromStdString(t->error) + ")"), false);
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
                lastRead = job.data;
                lastReadName = QString("%1_%2").arg(hex(job.index, 4), hex(job.sub, 2));
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
                lastRead = job.data;
                lastReadName = QString("%1_%2").arg(hex(job.index, 4), hex(job.sub, 2));
                finishJob(job.data.size() > 48 ? tr("%1 bytes: %2 ...").arg(job.data.size()).arg(QString(job.data.left(32).toHex(' ').toUpper()))
                                               : formatValue(job.type, job.data), true);
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
    if (job.pdoStep != PDO_NONE)
    {
        /* node not answering: drop the rest of the mapping read */
        for (int i = queue.size() - 1; i >= 0; i--)
            if (queue[i].pdoStep != PDO_NONE)
                queue.removeAt(i);
        mappingTimeout = true;
    }
    sdoAbort(0x05040000);
    finishJob(tr("timeout, no response"), false);
}

void CANopenWindow::finishJob(const QString &result, bool ok)
{
    sdoTimer.stop();
    QString obj = QString("%1:%2").arg(hex(job.index, 4), hex(job.sub, 2));
    QString line = tr("node %1 %2 %3%4 -> %5").arg(job.node).arg(job.upload ? "read" : "write", obj,
                                                    job.label.isEmpty() ? QString() : " (" + job.label + ")", result);
    if (job.pdoStep == PDO_NONE)
    {
        log((ok ? "" : "ERROR ") + line);
        resultLabel->setText(line);
    }
    else
    {
        /* missing PDO objects are normal, log only what was found */
        if (ok || mappingTimeout)
            log((ok ? "" : "ERROR ") + line);
        handlePdoJob(ok);
    }
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

/* ---------------------------------------------------------------------- PDO */

void CANopenWindow::loadEdsClicked()
{
    QSettings settings;
    QString path = QFileDialog::getOpenFileName(this, tr("Load EDS or DCF file"),
                                                settings.value("CANopen/edsFile").toString(),
                                                tr("CANopen device description (*.eds *.dcf *.EDS *.DCF);;All files (*)"));
    if (path.isEmpty())
        return;
    if (loadEds(path))
        settings.setValue("CANopen/edsFile", path);
}

bool CANopenWindow::loadEds(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        pdoStatus->setText(tr("cannot open %1").arg(path));
        return false;
    }
    canopen::Eds e;
    e.parse(QString::fromLatin1(f.readAll()).toStdString());
    if (e.objects.empty())
    {
        pdoStatus->setText(tr("%1 has no object dictionary entries").arg(QFileInfo(path).fileName()));
        return false;
    }
    eds = e;
    edsLoaded = true;
    edsName = QFileInfo(path).fileName();
    log(tr("loaded %1 (%2, %3 objects)").arg(edsName, QString::fromStdString(eds.productName))
            .arg(int(eds.objects.size())));
    rebuildPdosFromEds();
    return true;
}

void CANopenWindow::rebuildPdosFromEds()
{
    if (!edsLoaded || mappingRead)
        return;
    if (pdosFromNode)
    {
        /* keep the mapping read from the node, take names and data types from the EDS */
        std::vector<canopen::Pdo> list = pdos;
        for (canopen::Pdo &p : list)
        {
            std::vector<uint32_t> mapping;
            for (const canopen::PdoEntry &e : p.entries)
                mapping.push_back(uint32_t(e.index) << 16 | uint32_t(e.sub) << 8 | e.bits);
            p.entries = canopen::mapEntries(mapping, &eds);
        }
        setPdos(list, nodeSource + tr(", names from %1").arg(edsName));
        return;
    }
    pdosFromNode = false;
    setPdos(canopen::buildPdos(eds, pdoNode->value()), tr("from %1").arg(edsName));
}

void CANopenWindow::readMappingClicked()
{
    if (mappingRead)
        return;
    pending.clear();
    mappingRead = true;
    mappingTimeout = false;
    int node = pdoNode->value();
    for (int dir = 0; dir < 2; dir++)
        for (int n = 1; n <= 8; n++)
        {
            SdoJob j;
            j.node = node;
            j.index = quint16((dir == 0 ? 0x1800 : 0x1400) + n - 1);
            j.sub = 1;
            j.type = T_U32;
            j.label = QString("%1PDO%2 COB-ID").arg(dir == 0 ? "T" : "R").arg(n);
            j.pdoStep = PDO_COB;
            j.pdoTx = dir == 0;
            j.pdoNum = n;
            queue.append(j);
        }
    pdoStatus->setText(tr("reading the PDO mapping of node %1...").arg(node));
    startNextJob();
}

bool CANopenWindow::pdoJobsPending() const
{
    for (const SdoJob &j : queue)
        if (j.pdoStep != PDO_NONE)
            return true;
    return false;
}

void CANopenWindow::handlePdoJob(bool ok)
{
    int key = (job.pdoTx ? 0 : 256) + job.pdoNum;
    QString dir = job.pdoTx ? "T" : "R";
    quint16 mapIndex = quint16((job.pdoTx ? 0x1A00 : 0x1600) + job.pdoNum - 1);

    if (ok && job.pdoStep == PDO_COB && job.data.size() >= 4)
    {
        PendingPdo &p = pending[key];
        p.haveCob = true;
        p.cob = le32(job.data, 0);
        SdoJob j;
        j.node = job.node;
        j.index = mapIndex;
        j.sub = 0;
        j.type = T_U8;
        j.label = QString("%1PDO%2 mapped objects").arg(dir).arg(job.pdoNum);
        j.pdoStep = PDO_COUNT;
        j.pdoTx = job.pdoTx;
        j.pdoNum = job.pdoNum;
        queue.append(j);
    }
    else if (ok && job.pdoStep == PDO_COUNT && job.data.size() >= 1)
    {
        int count = qMin(64, int(quint8(job.data[0])));
        pending[key].mapping = QVector<quint32>(count, 0);
        for (int s = 1; s <= count; s++)
        {
            SdoJob j;
            j.node = job.node;
            j.index = mapIndex;
            j.sub = quint8(s);
            j.type = T_U32;
            j.label = QString("%1PDO%2 entry %3").arg(dir).arg(job.pdoNum).arg(s);
            j.pdoStep = PDO_ENTRY;
            j.pdoTx = job.pdoTx;
            j.pdoNum = job.pdoNum;
            queue.append(j);
        }
    }
    else if (ok && job.pdoStep == PDO_ENTRY && job.data.size() >= 4)
    {
        PendingPdo &p = pending[key];
        if (job.sub >= 1 && job.sub <= p.mapping.size())
            p.mapping[job.sub - 1] = le32(job.data, 0);
    }

    if (!pdoJobsPending())
        finishMappingRead();
}

void CANopenWindow::finishMappingRead()
{
    std::vector<canopen::Pdo> list;
    int tx = 0, rx = 0;
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it)
    {
        const PendingPdo &pp = it.value();
        if (!pp.haveCob)
            continue;
        canopen::Pdo p;
        p.tx = it.key() < 256;
        p.number = it.key() % 256;
        p.valid = !(pp.cob & 0x80000000u);
        p.cobId = pp.cob & 0x7FF;
        std::vector<uint32_t> mapping;
        for (quint32 m : pp.mapping)
            if (m)
                mapping.push_back(m);
        p.entries = canopen::mapEntries(mapping, edsLoaded ? &eds : nullptr);
        list.push_back(p);
        (p.tx ? tx : rx)++;
    }
    mappingRead = false;
    pending.clear();
    if (mappingTimeout && list.empty())
    {
        pdoStatus->setText(tr("node %1 did not answer").arg(pdoNode->value()));
        return;
    }
    QString source = tr("read from node %1: %2 TPDO, %3 RPDO").arg(pdoNode->value()).arg(tx).arg(rx);
    if (mappingTimeout)
        source += tr(" (incomplete, node stopped answering)");
    log(source);
    pdosFromNode = true;
    nodeSource = source;
    setPdos(list, edsLoaded ? source + tr(", names from %1").arg(edsName) : source);
}

void CANopenWindow::setPdos(const std::vector<canopen::Pdo> &list, const QString &source)
{
    pdos = list;
    pdoLive = QVector<PdoLive>(int(pdos.size()));
    pdoByCob.clear();
    pdoTable->clearSpans();

    int rows = 0;
    for (size_t i = 0; i < pdos.size(); i++)
    {
        pdoLive[int(i)].row = rows;
        rows += qMax(1, int(pdos[i].entries.size()));
        if (pdos[i].valid && !pdoByCob.contains(pdos[i].cobId))
            pdoByCob.insert(pdos[i].cobId, int(i));
    }
    pdoTable->setRowCount(rows);

    auto cell = [this](int row, int col, const QString &text) {
        QTableWidgetItem *item = new QTableWidgetItem(text);
        if (col == 0 || col == 3 || col >= 5)
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pdoTable->setItem(row, col, item);
    };
    int valid = 0;
    for (size_t i = 0; i < pdos.size(); i++)
    {
        const canopen::Pdo &p = pdos[i];
        int row = pdoLive[int(i)].row;
        int n = qMax(1, int(p.entries.size()));
        valid += p.valid ? 1 : 0;
        for (int r = 0; r < n; r++)
        {
            cell(row + r, 0, r ? QString() : "0x" + hex(p.cobId, 3));
            cell(row + r, 1, r ? QString() : QString("%1PDO%2%3").arg(p.tx ? "T" : "R").arg(p.number)
                                                   .arg(p.valid ? QString() : tr(" (disabled)")));
            if (p.entries.empty())
            {
                cell(row, 2, "-");
                cell(row, 3, QString());
                cell(row, 4, tr("no mapped objects"));
            }
            else
            {
                const canopen::PdoEntry &e = p.entries[size_t(r)];
                cell(row + r, 2, QString("%1:%2").arg(hex(e.index, 4), hex(e.sub, 2)));
                cell(row + r, 3, QString::number(e.bits));
                cell(row + r, 4, QString::fromStdString(e.name));
            }
            for (int c = 5; c < 9; c++)
                cell(row + r, c, QString());
        }
        if (n > 1)
            for (int c : { 0, 1, 7, 8 })
                pdoTable->setSpan(row, c, n, 1);
    }
    pdoStatus->setText(tr("%1 PDOs (%2 enabled) %3").arg(int(pdos.size())).arg(valid).arg(source));
}

void CANopenWindow::refreshPdos()
{
    for (int i = 0; i < pdoLive.size(); i++)
    {
        PdoLive &l = pdoLive[i];
        if (!l.dirty)
            continue;
        l.dirty = false;
        const canopen::Pdo &p = pdos[size_t(i)];
        const uint8_t *d = reinterpret_cast<const uint8_t *>(l.data.constData());
        size_t len = size_t(l.data.size());
        if (p.entries.empty())
            pdoTable->item(l.row, 6)->setText(QString(l.data.toHex(' ').toUpper()));
        for (size_t r = 0; r < p.entries.size(); r++)
        {
            const canopen::PdoEntry &e = p.entries[r];
            uint64_t raw;
            int row = l.row + int(r);
            pdoTable->item(row, 5)->setText(QString::fromStdString(canopen::formatEntry(e, d, len)));
            pdoTable->item(row, 6)->setText(canopen::extract(e, d, len, raw)
                ? QString::number(qulonglong(raw), 16).toUpper().rightJustified((e.bits + 3) / 4, '0')
                : tr("short frame (%1 bytes)").arg(int(len)));
        }
        pdoTable->item(l.row, 7)->setText(QString::number(l.count));
        pdoTable->item(l.row, 8)->setText(l.periodUs < 0 ? QString("-")
                                                          : QString("%1 ms").arg(l.periodUs / 1000.0, 0, 'f', 1));
    }
}

/* ------------------------------------------------------------ SDO with files */

void CANopenWindow::writeFileClicked()
{
    SdoJob j;
    if (!parseObject(j.node, j.index, j.sub))
        return;
    QSettings settings;
    QString path = QFileDialog::getOpenFileName(this, tr("Write file to the object"), settings.value("CANopen/writeFile").toString());
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        resultLabel->setText(tr("cannot open %1").arg(path));
        return;
    }
    j.data = f.readAll();
    if (j.data.isEmpty())
    {
        resultLabel->setText(tr("%1 is empty").arg(path));
        return;
    }
    settings.setValue("CANopen/writeFile", path);
    j.upload = false;
    j.type = T_HEX;
    j.block = blockCheck->isChecked();
    j.label = QFileInfo(path).fileName() + tr(", %1 bytes").arg(j.data.size());
    queue.append(j);
    startNextJob();
}

void CANopenWindow::saveReadClicked()
{
    if (lastRead.isEmpty())
    {
        resultLabel->setText(tr("read an object first"));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save read data"), lastReadName + ".bin");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        resultLabel->setText(tr("cannot write %1").arg(path));
        return;
    }
    f.write(lastRead);
    resultLabel->setText(tr("saved %1 bytes to %2").arg(lastRead.size()).arg(path));
}

/* --------------------------------------------------------------------- LSS */

QWidget *CANopenWindow::buildLssPage()
{
    using namespace canopen;
    QWidget *page = new QWidget(this);
    lssTimer.setSingleShot(true);
    connect(&lssTimer, &QTimer::timeout, this, &CANopenWindow::lssTimeout);

    auto button = [this, page](const QString &text, const QString &tip, std::function<void()> fn) {
        QPushButton *b = new QPushButton(text, page);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };

    QHBoxLayout *row1 = new QHBoxLayout;
    row1->addWidget(button(tr("Fastscan"), tr("Finds a node without node-ID (FFh) bit by bit and puts it into configuration state"),
                           [this]() {
                               if (lssFastscan) return;
                               fastscan = lss::Fastscan();
                               lssFastscan = true;
                               lssStatus->setText(tr("fastscan running..."));
                               log(tr("LSS fastscan started"));
                               fastscanStep();
                           }));
    row1->addWidget(button(tr("Any unconfigured node?"), tr("LSS identify non-configured remote slave"), [this]() {
        LssOp op;
        op.frame = lss::cmd(lss::IDENTIFY_NON_CONFIGURED);
        op.replyCs = lss::NON_CONFIGURED_RESPONSE;
        op.label = tr("identify non-configured");
        lssQueue(op);
    }));
    row1->addWidget(button(tr("All: configuration"), tr("Switch state global: every LSS node into configuration state"), [this]() {
        LssOp op;
        op.frame = lss::switchGlobal(true);
        op.label = tr("all nodes to configuration state");
        lssQueue(op);
    }));
    row1->addWidget(button(tr("All: waiting"), tr("Switch state global to waiting; a node that just got its first node-ID starts with it"),
                           [this]() {
                               LssOp op;
                               op.frame = lss::switchGlobal(false);
                               op.label = tr("all nodes to waiting state");
                               lssQueue(op);
                           }));
    row1->addStretch(1);

    QHBoxLayout *row2 = new QHBoxLayout;
    const char *names[4] = { "Vendor", "Product", "Revision", "Serial" };
    for (int i = 0; i < 4; i++)
    {
        lssId[i] = new QLineEdit(page);
        lssId[i]->setPlaceholderText(tr("hex"));
        lssId[i]->setMaximumWidth(110);
        row2->addWidget(new QLabel(tr(names[i]), page));
        row2->addWidget(lssId[i]);
    }
    row2->addWidget(button(tr("Select"), tr("Switch state selective: only the node with this identity goes to configuration state"),
                           [this]() {
                               for (int i = 0; i < 4; i++)
                               {
                                   bool ok;
                                   quint32 v = lssId[i]->text().trimmed().remove("0x", Qt::CaseInsensitive).toUInt(&ok, 16);
                                   if (!ok)
                                   {
                                       lssStatus->setText(tr("enter vendor, product, revision and serial in hex"));
                                       lssOps.clear();
                                       return;
                                   }
                                   LssOp op;
                                   op.frame = lss::switchSelective(i, v);
                                   op.label = tr("select by identity");
                                   if (i == 3) op.replyCs = lss::SWITCH_SELECTIVE_RESPONSE;
                                   lssQueue(op);
                               }
                           }));
    row2->addWidget(button(tr("Inquire"), tr("Reads identity and node-ID of the node in configuration state"), [this]() {
        for (int i = 0; i < 5; i++)
        {
            LssOp op;
            op.frame = lss::inquire(quint8(lss::INQUIRE_VENDOR + i));
            op.replyCs = quint8(lss::INQUIRE_VENDOR + i);
            op.part = i;
            op.label = tr("inquire");
            lssQueue(op);
        }
    }));
    row2->addStretch(1);

    QHBoxLayout *row3 = new QHBoxLayout;
    lssNodeId = new QSpinBox(page);
    lssNodeId->setRange(1, 127);
    lssNodeId->setValue(10);
    row3->addWidget(new QLabel(tr("Node-ID"), page));
    row3->addWidget(lssNodeId);
    row3->addWidget(button(tr("Set node-ID"), tr("Configure node-ID; it becomes active after NMT reset communication "
                                                 "(or, for a node that had none, when switched to waiting)"),
                           [this]() {
                               LssOp op;
                               op.frame = lss::configureNodeId(quint8(lssNodeId->value()));
                               op.replyCs = lss::CONFIGURE_NODE_ID;
                               op.label = tr("set node-ID %1").arg(lssNodeId->value());
                               lssQueue(op);
                           }));
    lssBitrate = new QComboBox(page);
    for (int i = 0; i < 9; i++)
        if (lss::tableBitrate(i))
            lssBitrate->addItem(QString("%1 kbit/s").arg(lss::tableBitrate(i) / 1000), i);
    lssBitrate->setCurrentIndex(3);
    row3->addSpacing(16);
    row3->addWidget(lssBitrate);
    row3->addWidget(button(tr("Set bit timing"), tr("Configure bit timing (CiA 305 table 0)"), [this]() {
        LssOp op;
        op.frame = lss::configureBitTiming(quint8(lssBitrate->currentData().toInt()));
        op.replyCs = lss::CONFIGURE_BIT_TIMING;
        op.label = tr("set bit timing %1").arg(lssBitrate->currentText());
        lssQueue(op);
    }));
    lssDelay = new QSpinBox(page);
    lssDelay->setRange(0, 65535);
    lssDelay->setValue(1000);
    lssDelay->setSuffix(" ms");
    row3->addWidget(lssDelay);
    row3->addWidget(button(tr("Activate bit timing"), tr("All nodes in configuration state switch after the delay; switch this bus too"),
                           [this]() {
                               LssOp op;
                               op.frame = lss::activateBitTiming(quint16(lssDelay->value()));
                               op.label = tr("activate bit timing after %1 ms").arg(lssDelay->value());
                               lssQueue(op);
                           }));
    row3->addWidget(button(tr("Store"), tr("Store configuration (node-ID, bit timing) in the node"), [this]() {
        LssOp op;
        op.frame = lss::store();
        op.replyCs = lss::STORE;
        op.label = tr("store configuration");
        lssQueue(op);
    }));
    row3->addStretch(1);

    lssStatus = new QLabel(tr("LSS (CiA 305): find nodes without node-ID, set node-ID and bit rate. Typical: Fastscan, "
                              "Set node-ID, Store, All: waiting."), page);
    lssStatus->setWordWrap(true);
    lssStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QVBoxLayout *l = new QVBoxLayout(page);
    l->addLayout(row1);
    l->addLayout(row2);
    l->addLayout(row3);
    l->addWidget(lssStatus);
    l->addStretch(1);
    return page;
}

void CANopenWindow::lssQueue(const LssOp &op)
{
    lssOps.append(op);
    lssNext();
}

void CANopenWindow::lssNext()
{
    if (lssActive || lssFastscan || lssOps.isEmpty())
        return;
    lssOp = lssOps.takeFirst();
    sendFrame8(canopen::lss::MASTER_COB, lssOp.frame);
    if (lssOp.replyCs)
    {
        lssActive = true;
        lssTimer.start(lssOp.timeoutMs);
    }
    else
    {
        log(tr("LSS %1 sent").arg(lssOp.label));
        lssStatus->setText(tr("%1 sent").arg(lssOp.label));
        QTimer::singleShot(20, this, [this]() { lssNext(); });   /* spaced, no answer to wait for */
    }
}

void CANopenWindow::lssReply(const QByteArray &d)
{
    using namespace canopen;
    quint8 cs = quint8(d[0]);
    if (lssFastscan)
    {
        if (cs == lss::IDENTIFY_SLAVE && lssTimer.isActive())
        {
            lssTimer.stop();
            fastscan.answer(true);
            fastscanStep();
        }
        return;
    }
    if (!lssActive || cs != lssOp.replyCs)
        return;
    lssTimer.stop();
    lssActive = false;
    QString result;
    quint8 b1 = d.size() > 1 ? quint8(d[1]) : 0;
    switch (cs)
    {
    case lss::SWITCH_SELECTIVE_RESPONSE: result = tr("node selected, in configuration state"); break;
    case lss::CONFIGURE_NODE_ID:
    case lss::CONFIGURE_BIT_TIMING:
    case lss::STORE: result = QString::fromStdString(lss::errorText(cs, b1)); break;
    case lss::NON_CONFIGURED_RESPONSE: result = tr("at least one node without node-ID is present"); break;
    case lss::INQUIRE_NODE_ID: result = b1 == 0xFF ? tr("node-ID: none (FFh)") : tr("node-ID: %1").arg(b1); break;
    default:
        if (cs >= lss::INQUIRE_VENDOR && cs <= lss::INQUIRE_SERIAL && d.size() >= 5)
        {
            quint32 v = le32(d, 1);
            lssId[cs - lss::INQUIRE_VENDOR]->setText(hex(v, 8));
            const char *names[4] = { "vendor", "product", "revision", "serial" };
            result = QString("%1 %2").arg(names[cs - lss::INQUIRE_VENDOR]).arg(hex(v, 8));
        }
    }
    log(tr("LSS %1: %2").arg(lssOp.label, result));
    lssStatus->setText(tr("%1: %2").arg(lssOp.label, result));
    lssNext();
}

void CANopenWindow::lssTimeout()
{
    if (lssFastscan)
    {
        fastscan.answer(false);
        fastscanStep();
        return;
    }
    if (!lssActive)
        return;
    lssActive = false;
    QString result = lssOp.replyCs == canopen::lss::NON_CONFIGURED_RESPONSE ? tr("no node without node-ID answered")
                                                                             : tr("no answer (is a node in configuration state?)");
    log(tr("LSS %1: %2").arg(lssOp.label, result));
    lssStatus->setText(tr("%1: %2").arg(lssOp.label, result));
    lssNext();
}

void CANopenWindow::fastscanStep()
{
    using namespace canopen;
    if (fastscan.finished())
    {
        lssFastscan = false;
        QString result;
        if (fastscan.phase == lss::Fastscan::Found)
        {
            for (int i = 0; i < 4; i++)
                lssId[i]->setText(hex(fastscan.id[i], 8));
            result = tr("found vendor %1 product %2 revision %3 serial %4; it is in configuration state now, set its node-ID")
                         .arg(hex(fastscan.id[0], 8), hex(fastscan.id[1], 8), hex(fastscan.id[2], 8), hex(fastscan.id[3], 8));
        }
        else if (fastscan.phase == lss::Fastscan::NoneFound)
            result = tr("no node without node-ID answered");
        else
            result = tr("the node stopped answering during the scan");
        log(tr("LSS fastscan: %1 (%2 requests)").arg(result).arg(fastscan.requests));
        lssStatus->setText(tr("fastscan: %1").arg(result));
        lssNext();
        return;
    }
    fastscan.requests++;
    sendFrame8(lss::MASTER_COB, fastscan.next());
    lssTimer.start(100);                     /* no answer within this time: the bit is 1 */
    if (fastscan.requests % 10 == 0)
        lssStatus->setText(tr("fastscan running, %1 requests...").arg(fastscan.requests));
}
