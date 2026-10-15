// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef CANCALCWINDOW_H
#define CANCALCWINDOW_H

/*
 * CAN Calculator: one identifier shown as 29/11-bit, as 11+18-bit parts,
 * and as MCP2510/MCP2515 and SJA1000 (PeliCAN) register values. Editing
 * any field updates all others. Conversions in canidcalc.h.
 */

#include <QWidget>

class QLineEdit;
class QCheckBox;
class QLabel;

class CanCalcWindow : public QWidget
{
    Q_OBJECT

public:
    explicit CanCalcWindow(QWidget *parent = nullptr);

private:
    enum Source { FROM_ID, FROM_PARTS, FROM_MCP, FROM_SJA, FROM_FLAGS };
    void recompute(Source src);
    void showId(quint32 id);
    QLineEdit *hexEdit(int width);

    QLineEdit *idEdit, *decEdit, *baseEdit, *extEdit;
    QLineEdit *mcp[4];
    QLineEdit *sja[4];
    QCheckBox *extended, *rtr;
    QLabel    *binLabel, *status;
    bool       busy = false;
};

#endif // CANCALCWINDOW_H
