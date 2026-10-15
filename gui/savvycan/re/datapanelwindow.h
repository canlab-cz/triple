// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef DATAPANELWINDOW_H
#define DATAPANELWINDOW_H

/*
 * Data Panel: one classic CAN frame edited live. Per data byte a slider,
 * a hex spin box and eight bit check boxes; a 16-bit value slider that
 * writes two bytes in little or big endian order; and a text field that
 * fills the data bytes with ASCII. The frame is sent on every change
 * and/or periodically. Covers the "data sender", "bit sender" and "ASCII
 * sender" tools of PP2CAN.
 */

#include <QWidget>
#include <QTimer>

class QSpinBox;
class QSlider;
class QCheckBox;
class QLineEdit;
class QComboBox;
class QLabel;

class DataPanelWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DataPanelWindow(QWidget *parent = nullptr);

private slots:
    void sendNow();
    void lengthChanged();

private:
    void setByte(int i, int value);
    void refreshWidgets();
    void changed();
    void apply16();

    QSpinBox  *busSpin, *lenSpin, *intervalSpin;
    QLineEdit *idEdit, *asciiEdit;
    QCheckBox *extBox, *onChangeBox, *periodicBox;
    QLabel    *status;

    QSlider   *slider[8];
    QSpinBox  *spin[8];
    QCheckBox *bit[8][8];

    QSlider   *slider16;
    QSpinBox  *spin16, *start16;
    QComboBox *order16;

    quint8  bytes[8] = {};
    bool    busy = false;
    int     sentCount = 0;
    QTimer  periodTimer;
    QTimer  changeTimer;
};

#endif // DATAPANELWINDOW_H
