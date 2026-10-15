// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "busstatswindow.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "qcustomplot.h"
#include "connections/canbus.h"
#include "connections/canconmanager.h"
#include "connections/canconnection.h"

namespace {
const int HISTORY_SECONDS = 120;
const char *STATE_NAMES[] = { "error active", "error warning", "error passive", "bus off" };
const QColor PALETTE[] = { QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c),
                           QColor(0xd6, 0x27, 0x28), QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b) };

enum Col { C_BUS, C_NOMINAL, C_DATA, C_RATE, C_LOAD, C_PEAK, C_FRAMES, C_ERRORS, C_OVERFLOWS, C_STATE, C_TEC, C_REC, C_COUNT };

inline int frameBus(const BusStatsFrame &f)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return f.getBus();
#else
    return f.bus;
#endif
}

QString bitrateText(quint32 bps)
{
    if (!bps) return "-";
    if (bps >= 1000000 && bps % 100000 == 0) return QString::number(bps / 1e6, 'g', 3) + "M";
    if (bps >= 1000) return QString::number(bps / 1e3, 'g', 4) + "k";
    return QString::number(bps);
}
}

BusStatsWindow::BusStatsWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Bus Statistics"));
    resize(900, 560);

    table = new QTableWidget(0, C_COUNT, this);
    table->setHorizontalHeaderLabels({ tr("Bus"), tr("Bitrate"), tr("Data rate"), tr("Frames/s"), tr("Load %"),
                                       tr("Peak %"), tr("Frames"), tr("Errors"), tr("Overflows"), tr("State"), tr("TEC"), tr("REC") });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(C_STATE, QHeaderView::Stretch);

    plot = new QCustomPlot(this);
    plot->xAxis->setLabel(tr("seconds"));
    plot->yAxis->setLabel(tr("bus load %"));
    plot->yAxis->setRange(0, 100);
    plot->xAxis->setRange(0, HISTORY_SECONDS);
    plot->legend->setVisible(true);
    plot->setMinimumHeight(220);

    note = new QLabel(tr("Load is the bus time of the observed frames without bit stuffing (a lower bound). "
                         "Frames sent by this program are counted only if the device echoes them."), this);
    note->setWordWrap(true);

    QPushButton *resetButton = new QPushButton(tr("Reset"), this);
    connect(resetButton, &QPushButton::clicked, this, &BusStatsWindow::reset);

    QHBoxLayout *bottom = new QHBoxLayout;
    bottom->addWidget(note, 1);
    bottom->addWidget(resetButton);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(table, 0);
    layout->addWidget(plot, 1);
    layout->addLayout(bottom);

    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &BusStatsWindow::framesReceived);
    connect(&timer, &QTimer::timeout, this, &BusStatsWindow::tick);
    timer.start(1000);
    interval.start();
    since.start();
    refreshBitrates();
    updateTable();
}

void BusStatsWindow::ensureBus(int idx)
{
    if (idx >= buses.size())
        buses.resize(idx + 1);
    while (plot->graphCount() < buses.size())
    {
        int n = plot->graphCount();
        QCPGraph *g = plot->addGraph();
        g->setName(tr("bus %1").arg(n));
        g->setPen(QPen(PALETTE[n % 6], 2));
    }
}

void BusStatsWindow::refreshBitrates()
{
    QList<CANConnection *> &conns = CANConManager::getInstance()->getConnections();
    int base = 0;
    for (CANConnection *conn : conns)
    {
        for (int i = 0; i < conn->getNumBuses(); i++)
        {
            ensureBus(base + i);
            CANBus bus;
            if (conn->getBusSettings(i, bus))
            {
                buses[base + i].nominal = bus.getSpeed() > 0 ? bus.getSpeed() : 0;
                buses[base + i].data = (bus.isCanFD() && bus.getDataRate() > 0) ? bus.getDataRate() : 0;
            }
        }
        base += conn->getNumBuses();
    }
}

/* Bus time of one frame, without bit stuffing, including the 3-bit
 * interframe space. Classic CAN: 47 bits + 8 per data byte for an 11-bit
 * identifier, 67 + 8 per byte for 29 bits. CAN FD: arbitration and the
 * frame end at the nominal rate, ESI..CRC delimiter at the data rate when
 * the bit rate switch is set. */
double BusStatsWindow::frameSeconds(const BusStatsFrame &f, quint32 nominal, quint32 data)
{
    if (!nominal)
        return 0;
    int len = f.payload().size();
    bool ext = f.hasExtendedFrameFormat();
    if (!f.hasFlexibleDataRateFormat())
    {
        if (f.frameType() == BusStatsFrame::RemoteRequestFrame)
            len = 0;
        return double((ext ? 67 : 47) + 8 * len) / nominal;
    }
    int arbitration = ext ? 36 : 17;                      /* SOF .. BRS */
    int crc = len <= 16 ? 17 : 21;
    int dataPhase = 1 + 4 + 8 * len + 4 + crc + 1;        /* ESI, DLC, data, stuff count, CRC, CRC delimiter */
    int tail = 2 + 7 + 3;                                  /* ACK, EOF, IFS */
    double dataRate = (f.hasBitrateSwitch() && data) ? data : nominal;
    return double(arbitration + tail) / nominal + dataPhase / dataRate;
}

void BusStatsWindow::framesReceived(CANConnection *conn, QVector<BusStatsFrame> &frames)
{
    Q_UNUSED(conn);
    for (const BusStatsFrame &f : frames)
    {
        int idx = frameBus(f);
        if (idx < 0 || idx > 63)
            continue;
        ensureBus(idx);
        Bus &b = buses[idx];

        if (f.frameType() == BusStatsFrame::ErrorFrame)
        {
            b.errors++;
            QByteArray d = f.payload();
            if (f.error() & BusStatsFrame::BusOffError)
                b.state = 3;
            else if (d.size() >= 2)
            {
                quint8 ctrl = quint8(d[1]);
                if (ctrl & 0x03)                               /* RX / TX buffer overflow */
                    b.overflows++;
                if (ctrl & 0x40) b.state = 0;                  /* back to error active */
                else if (ctrl & 0x30) b.state = 2;             /* RX / TX passive */
                else if (ctrl & 0x0C) b.state = 1;             /* RX / TX warning */
            }
            if (d.size() >= 8)
            {
                b.tec = quint8(d[6]);
                b.rec = quint8(d[7]);
            }
            continue;
        }

        b.frames++;
        b.intervalFrames++;
        b.intervalBusy += frameSeconds(f, b.nominal, b.data);
    }
}

void BusStatsWindow::tick()
{
    double elapsed = interval.restart() / 1000.0;
    if (elapsed <= 0)
        return;
    refreshBitrates();
    for (Bus &b : buses)
    {
        b.rate = b.intervalFrames / elapsed;
        b.load = b.nominal ? 100.0 * b.intervalBusy / elapsed : 0;
        if (b.load > b.peak)
            b.peak = b.load;
        b.intervalFrames = 0;
        b.intervalBusy = 0;
    }
    updateTable();
    updatePlot(since.elapsed() / 1000.0);
}

void BusStatsWindow::reset()
{
    for (Bus &b : buses)
    {
        quint32 nominal = b.nominal, data = b.data;
        b = Bus();
        b.nominal = nominal;
        b.data = data;
    }
    for (int i = 0; i < plot->graphCount(); i++)
        plot->graph(i)->data()->clear();
    since.restart();
    interval.restart();
    updateTable();
    updatePlot(0);
}

void BusStatsWindow::updateTable()
{
    table->setRowCount(buses.size());
    for (int i = 0; i < buses.size(); i++)
    {
        const Bus &b = buses[i];
        const QString cells[C_COUNT] = {
            QString::number(i),
            bitrateText(b.nominal),
            bitrateText(b.data),
            QString::number(b.rate, 'f', 0),
            b.nominal ? QString::number(b.load, 'f', 1) : QString("?"),
            b.nominal ? QString::number(b.peak, 'f', 1) : QString("?"),
            QString::number(b.frames),
            QString::number(b.errors),
            QString::number(b.overflows),
            b.state < 0 ? QString("-") : QString(STATE_NAMES[b.state]),
            b.tec < 0 ? QString("-") : QString::number(b.tec),
            b.rec < 0 ? QString("-") : QString::number(b.rec),
        };
        for (int c = 0; c < C_COUNT; c++)
        {
            QTableWidgetItem *item = table->item(i, c);
            if (!item)
            {
                item = new QTableWidgetItem;
                item->setTextAlignment(c == C_STATE ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignRight | Qt::AlignVCenter));
                table->setItem(i, c, item);
            }
            item->setText(cells[c]);
        }
        QTableWidgetItem *state = table->item(i, C_STATE);
        state->setForeground(b.state >= 2 ? QColor(0xd6, 0x27, 0x28) : (b.state == 1 ? QColor(0xff, 0x7f, 0x0e) : palette().color(QPalette::Text)));
    }
}

void BusStatsWindow::updatePlot(double t)
{
    for (int i = 0; i < buses.size() && i < plot->graphCount(); i++)
    {
        QCPGraph *g = plot->graph(i);
        g->addData(t, buses[i].load);
        g->data()->removeBefore(t - HISTORY_SECONDS);
    }
    plot->xAxis->setRange(t < HISTORY_SECONDS ? 0 : t - HISTORY_SECONDS, t < HISTORY_SECONDS ? HISTORY_SECONDS : t);
    double top = 10;
    for (const Bus &b : buses)
        if (b.peak > top) top = b.peak;
    plot->yAxis->setRange(0, top >= 50 ? 100 : (top >= 20 ? 50 : 20));
    plot->replot();
}
