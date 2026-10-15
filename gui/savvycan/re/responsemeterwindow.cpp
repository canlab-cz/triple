// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "responsemeterwindow.h"

#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtMath>

#include "connections/canconmanager.h"

namespace {
const int TABLE_ROWS = 50;
QString ms(qint64 us) { return QString::number(us / 1000.0, 'f', 3) + " ms"; }
}

ResponseMeterWindow::ResponseMeterWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Response Meter"));
    resize(640, 640);

    busSpin = new QSpinBox(this);
    busSpin->setRange(0, 63);
    reqIdEdit = new QLineEdit("605", this);
    respIdEdit = new QLineEdit("585", this);
    reqIdEdit->setToolTip(tr("hex; more than 3 digits or above 7FF means a 29-bit identifier"));
    respIdEdit->setToolTip(reqIdEdit->toolTip());
    modeCombo = new QComboBox(this);
    modeCombo->addItems({ tr("Watch: request sent by another node"), tr("Send: this window sends the request") });
    reqDataEdit = new QLineEdit("40 00 10 00 00 00 00 00", this);
    reqDataEdit->setToolTip(tr("request data, hex bytes (send mode)"));
    periodSpin = new QSpinBox(this);
    periodSpin->setRange(5, 60000);
    periodSpin->setValue(100);
    periodSpin->setSuffix(" ms");
    countSpin = new QSpinBox(this);
    countSpin->setRange(0, 1000000);
    countSpin->setValue(100);
    countSpin->setSpecialValueText(tr("continuous"));
    timeoutSpin = new QSpinBox(this);
    timeoutSpin->setRange(1, 60000);
    timeoutSpin->setValue(1000);
    timeoutSpin->setSuffix(" ms");

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("Bus"), busSpin);
    form->addRow(tr("Request ID"), reqIdEdit);
    form->addRow(tr("Response ID"), respIdEdit);
    form->addRow(tr("Mode"), modeCombo);
    form->addRow(tr("Request data"), reqDataEdit);
    form->addRow(tr("Period"), periodSpin);
    form->addRow(tr("Requests"), countSpin);
    form->addRow(tr("Timeout"), timeoutSpin);

    startBtn = new QPushButton(tr("Start"), this);
    QPushButton *resetBtn = new QPushButton(tr("Reset"), this);
    connect(startBtn, &QPushButton::clicked, this, &ResponseMeterWindow::startStop);
    connect(resetBtn, &QPushButton::clicked, this, &ResponseMeterWindow::reset);
    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addWidget(startBtn);
    buttons->addWidget(resetBtn);
    buttons->addStretch(1);

    statsLabel = new QLabel(this);
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    noteLabel = new QLabel(this);
    noteLabel->setWordWrap(true);

    table = new QTableWidget(0, 2, this);
    table->setHorizontalHeaderLabels({ tr("#"), tr("Delay") });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(statsLabel);
    layout->addWidget(noteLabel);
    layout->addWidget(table, 1);

    auto modeChanged = [this]() {
        bool send = modeCombo->currentIndex() == 1;
        reqDataEdit->setEnabled(send);
        periodSpin->setEnabled(send);
        countSpin->setEnabled(send);
        noteLabel->setText(send
            ? tr("Send mode: request time from the host clock, response time from the frame timestamp; the clock "
                 "offset (a few ms with USB adapters) is included in the result.")
            : tr("Watch mode: both times from frame timestamps (1 us with hardware timestamping). Frames sent "
                 "by this program are not seen unless the device echoes them."));
    };
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, modeChanged);
    modeChanged();

    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &ResponseMeterWindow::framesReceived);
    sendTimer.setTimerType(Qt::PreciseTimer);
    connect(&sendTimer, &QTimer::timeout, this, &ResponseMeterWindow::sendRequest);
    connect(&checkTimer, &QTimer::timeout, this, &ResponseMeterWindow::tick);
    checkTimer.start(20);
    updateStats();
}

bool ResponseMeterWindow::parseId(const QLineEdit *e, quint32 &id, bool &ext) const
{
    bool ok;
    QString t = e->text().trimmed();
    if (t.startsWith("0x", Qt::CaseInsensitive))
        t = t.mid(2);
    id = t.toUInt(&ok, 16);
    ext = id > 0x7FF || t.size() > 3;
    return ok && id <= 0x1FFFFFFF;
}

qint64 ResponseMeterWindow::hostMicros() const
{
    return QDateTime::currentMSecsSinceEpoch() * 1000ll - qint64(CANConManager::getInstance()->getTimeBasis());
}

void ResponseMeterWindow::startStop()
{
    if (running)
    {
        running = false;
        sendTimer.stop();
        pending = false;
        startBtn->setText(tr("Start"));
        return;
    }
    if (!parseId(reqIdEdit, reqId, reqExt) || !parseId(respIdEdit, respId, respExt))
    {
        statsLabel->setText(tr("request and response IDs are hex numbers"));
        return;
    }
    running = true;
    pending = false;
    sent = 0;
    startBtn->setText(tr("Stop"));
    if (modeCombo->currentIndex() == 1)
    {
        sendTimer.start(periodSpin->value());
        sendRequest();
    }
}

void ResponseMeterWindow::reset()
{
    delays.clear();
    timeouts = 0;
    sent = 0;
    pending = false;
    table->setRowCount(0);
    updateStats();
}

void ResponseMeterWindow::sendRequest()
{
    if (!running)
        return;
    if (countSpin->value() && sent >= countSpin->value())
    {
        if (!pending)
            startStop();
        return;
    }
    if (pending)
        return;                     /* still waiting; the timeout check handles it */
    QByteArray data = QByteArray::fromHex(reqDataEdit->text().remove(' ').toLatin1()).left(8);
    CANConManager::getInstance()->sendFrame(guiMakeFrame(busSpin->value(), reqId, reqExt, data));
    pending = true;
    pendingUs = hostMicros();
    pendingSince.start();
    sent++;
}

void ResponseMeterWindow::framesReceived(CANConnection *conn, QVector<GuiFrame> &frames)
{
    Q_UNUSED(conn);
    if (!running)
        return;
    bool watch = modeCombo->currentIndex() == 0;
    for (const GuiFrame &f : frames)
    {
        if (guiFrameBus(f) != busSpin->value() || f.frameType() != GUI_DATA_FRAME)
            continue;
        quint32 id = f.frameId();
        bool ext = f.hasExtendedFrameFormat();
        if (watch && id == reqId && ext == reqExt)
        {
            if (pending)
                timeouts++;         /* new request before the previous one was answered */
            pending = true;
            pendingUs = guiFrameMicros(f);
            pendingSince.start();
        }
        else if (id == respId && ext == respExt && pending)
        {
            pending = false;
            addMeasurement(guiFrameMicros(f) - pendingUs);
        }
    }
    updateStats();
}

void ResponseMeterWindow::tick()
{
    if (running && pending && pendingSince.elapsed() > timeoutSpin->value())
    {
        pending = false;
        timeouts++;
        updateStats();
    }
}

void ResponseMeterWindow::addMeasurement(qint64 delayUs)
{
    delays.append(delayUs);
    table->insertRow(0);
    table->setItem(0, 0, new QTableWidgetItem(QString::number(delays.size())));
    table->setItem(0, 1, new QTableWidgetItem(ms(delayUs)));
    if (table->rowCount() > TABLE_ROWS)
        table->setRowCount(TABLE_ROWS);
}

void ResponseMeterWindow::updateStats()
{
    if (delays.isEmpty())
    {
        statsLabel->setText(tr("responses: 0   timeouts: %1").arg(timeouts));
        return;
    }
    qint64 mn = delays.first(), mx = delays.first();
    double sum = 0;
    for (qint64 d : delays)
    {
        mn = qMin(mn, d);
        mx = qMax(mx, d);
        sum += d;
    }
    double avg = sum / delays.size();
    double var = 0;
    for (qint64 d : delays)
        var += (d - avg) * (d - avg);
    double sd = delays.size() > 1 ? qSqrt(var / (delays.size() - 1)) : 0;
    statsLabel->setText(tr("responses: %1   timeouts: %2\nlast %3   min %4   avg %5   max %6   std dev %7")
                            .arg(delays.size()).arg(timeouts)
                            .arg(ms(delays.last()), ms(mn), ms(qint64(avg)), ms(mx), ms(qint64(sd))));
}
