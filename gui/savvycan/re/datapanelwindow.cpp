// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "datapanelwindow.h"
#include "savvycompat.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include "connections/canconmanager.h"

DataPanelWindow::DataPanelWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Data Panel"));

    /* --- frame settings ------------------------------------------------- */
    busSpin = new QSpinBox(this);
    busSpin->setRange(0, 63);
    idEdit = new QLineEdit("123", this);
    idEdit->setMaximumWidth(110);
    idEdit->setToolTip(tr("identifier, hex"));
    extBox = new QCheckBox(tr("extended"), this);
    lenSpin = new QSpinBox(this);
    lenSpin->setRange(0, 8);
    lenSpin->setValue(8);
    onChangeBox = new QCheckBox(tr("send on change"), this);
    onChangeBox->setChecked(true);
    periodicBox = new QCheckBox(tr("periodic"), this);
    intervalSpin = new QSpinBox(this);
    intervalSpin->setRange(1, 60000);
    intervalSpin->setValue(100);
    intervalSpin->setSuffix(" ms");
    QPushButton *sendBtn = new QPushButton(tr("Send"), this);
    status = new QLabel(this);

    QHBoxLayout *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Bus"), this));
    top->addWidget(busSpin);
    top->addWidget(new QLabel(tr("ID"), this));
    top->addWidget(idEdit);
    top->addWidget(extBox);
    top->addWidget(new QLabel(tr("Length"), this));
    top->addWidget(lenSpin);
    top->addSpacing(12);
    top->addWidget(onChangeBox);
    top->addWidget(periodicBox);
    top->addWidget(intervalSpin);
    top->addWidget(sendBtn);
    top->addStretch(1);

    /* --- byte rows ------------------------------------------------------ */
    QGroupBox *bytesBox = new QGroupBox(tr("Data bytes (bits shown 7..0)"), this);
    QGridLayout *grid = new QGridLayout(bytesBox);
    for (int b = 7; b >= 0; b--)
        grid->addWidget(new QLabel(QString::number(b), this), 0, 3 + (7 - b), Qt::AlignHCenter);
    for (int i = 0; i < 8; i++)
    {
        int row = i + 1;
        grid->addWidget(new QLabel(QString("D%1").arg(i), this), row, 0);
        slider[i] = new QSlider(Qt::Horizontal, this);
        slider[i]->setRange(0, 255);
        slider[i]->setMinimumWidth(220);
        spin[i] = new QSpinBox(this);
        spin[i]->setRange(0, 255);
        spin[i]->setDisplayIntegerBase(16);
        spin[i]->setPrefix("0x");
        grid->addWidget(slider[i], row, 1);
        grid->addWidget(spin[i], row, 2);
        for (int b = 7; b >= 0; b--)
        {
            bit[i][b] = new QCheckBox(this);
            grid->addWidget(bit[i][b], row, 3 + (7 - b), Qt::AlignHCenter);
            connect(bit[i][b], &QCheckBox::toggled, this, [this, i, b](bool on) {
                if (busy) return;
                setByte(i, on ? (bytes[i] | (1 << b)) : (bytes[i] & ~(1 << b)));
            });
        }
        connect(slider[i], &QSlider::valueChanged, this, [this, i](int v) { if (!busy) setByte(i, v); });
        connect(spin[i], QOverload<int>::of(&QSpinBox::valueChanged), this, [this, i](int v) { if (!busy) setByte(i, v); });
    }

    /* --- 16-bit value and ASCII ----------------------------------------- */
    slider16 = new QSlider(Qt::Horizontal, this);
    slider16->setRange(0, 65535);
    spin16 = new QSpinBox(this);
    spin16->setRange(0, 65535);
    start16 = new QSpinBox(this);
    start16->setRange(0, 6);
    start16->setPrefix("D");
    order16 = new QComboBox(this);
    order16->addItems({ tr("little endian (Intel)"), tr("big endian (Motorola)") });
    connect(slider16, &QSlider::valueChanged, this, [this](int v) { if (!busy) { busy = true; spin16->setValue(v); busy = false; apply16(); } });
    connect(spin16, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { if (!busy) { busy = true; slider16->setValue(v); busy = false; apply16(); } });
    connect(start16, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { apply16(); });
    connect(order16, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { apply16(); });

    QGroupBox *box16 = new QGroupBox(tr("16-bit value"), this);
    QHBoxLayout *row16 = new QHBoxLayout(box16);
    row16->addWidget(slider16, 1);
    row16->addWidget(spin16);
    row16->addWidget(new QLabel(tr("at"), this));
    row16->addWidget(start16);
    row16->addWidget(order16);

    asciiEdit = new QLineEdit(this);
    asciiEdit->setPlaceholderText(tr("text, up to the frame length"));
    QPushButton *asciiBtn = new QPushButton(tr("Fill from text"), this);
    connect(asciiBtn, &QPushButton::clicked, this, [this]() {
        QByteArray t = asciiEdit->text().toLatin1();
        busy = true;
        for (int i = 0; i < 8; i++)
            bytes[i] = i < t.size() ? quint8(t[i]) : 0;
        busy = false;
        refreshWidgets();
        changed();
    });
    QGroupBox *asciiBox = new QGroupBox(tr("ASCII"), this);
    QHBoxLayout *asciiRow = new QHBoxLayout(asciiBox);
    asciiRow->addWidget(asciiEdit, 1);
    asciiRow->addWidget(asciiBtn);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(bytesBox);
    layout->addWidget(box16);
    layout->addWidget(asciiBox);
    layout->addWidget(status);

    connect(sendBtn, &QPushButton::clicked, this, &DataPanelWindow::sendNow);
    connect(lenSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &DataPanelWindow::lengthChanged);
    connect(periodicBox, &QCheckBox::toggled, this, [this](bool on) {
        if (on) periodTimer.start(intervalSpin->value()); else periodTimer.stop();
    });
    connect(intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (periodTimer.isActive()) periodTimer.start(v);
    });
    periodTimer.setTimerType(Qt::PreciseTimer);   /* default coarse timers run in 15.6 ms steps on Windows */
    connect(&periodTimer, &QTimer::timeout, this, &DataPanelWindow::sendNow);
    changeTimer.setSingleShot(true);
    connect(&changeTimer, &QTimer::timeout, this, &DataPanelWindow::sendNow);

    refreshWidgets();
    lengthChanged();
}

void DataPanelWindow::setByte(int i, int value)
{
    bytes[i] = quint8(value);
    refreshWidgets();
    changed();
}

void DataPanelWindow::apply16()
{
    int v = spin16->value();
    int at = start16->value();
    bool big = order16->currentIndex() == 1;
    bytes[at] = quint8(big ? (v >> 8) : v);
    bytes[at + 1] = quint8(big ? v : (v >> 8));
    refreshWidgets();
    changed();
}

void DataPanelWindow::refreshWidgets()
{
    busy = true;
    for (int i = 0; i < 8; i++)
    {
        slider[i]->setValue(bytes[i]);
        spin[i]->setValue(bytes[i]);
        for (int b = 0; b < 8; b++)
            bit[i][b]->setChecked(bytes[i] & (1 << b));
    }
    busy = false;
}

void DataPanelWindow::lengthChanged()
{
    int len = lenSpin->value();
    for (int i = 0; i < 8; i++)
    {
        slider[i]->setEnabled(i < len);
        spin[i]->setEnabled(i < len);
        for (int b = 0; b < 8; b++)
            bit[i][b]->setEnabled(i < len);
    }
    start16->setMaximum(qMax(0, len - 2));
}

void DataPanelWindow::changed()
{
    /* coalesce slider drags: at most one frame per 10 ms */
    if (onChangeBox->isChecked() && !changeTimer.isActive())
        changeTimer.start(10);
}

void DataPanelWindow::sendNow()
{
    bool ok;
    QString t = idEdit->text().trimmed();
    if (t.startsWith("0x", Qt::CaseInsensitive))
        t = t.mid(2);
    quint32 id = t.toUInt(&ok, 16);
    bool ext = extBox->isChecked() || id > 0x7FF;
    if (!ok || id > 0x1FFFFFFF)
    {
        status->setText(tr("ID is a hex number"));
        return;
    }
    QByteArray data(reinterpret_cast<const char *>(bytes), lenSpin->value());
    CANConManager::getInstance()->sendFrame(guiMakeFrame(busSpin->value(), id, ext, data));
    sentCount++;
    status->setText(tr("sent %1 frames, last: %2 [%3] %4").arg(sentCount)
                        .arg(QString::number(id, 16).toUpper()).arg(data.size()).arg(QString(data.toHex(' ').toUpper())));
}
