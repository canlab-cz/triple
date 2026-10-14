// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef SIGNALFINDERWINDOW_H
#define SIGNALFINDERWINDOW_H

/*
 * RE Tools > Signal Finder: automatic analysis of the captured frames
 * (re/canre.h). Per ID: timing, a bit flip heatmap, and the fields it
 * found (rolling counters, checksums, constants, flags, signal candidates
 * with byte order) with a plot of the selected one over time; checked
 * fields go into a DBC file, which can be loaded into SavvyCAN right
 * away. A second tab compares two time windows ("what changed when I
 * pressed the button").
 */

#include <QVector>
#include <QWidget>
#include <vector>

#include "canre.h"
#include "savvycompat.h"

class QComboBox;
class QCustomPlot;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

class SignalFinderWindow : public QWidget
{
    Q_OBJECT

public:
    SignalFinderWindow(const QVector<GuiFrame> *frames, QWidget *parent = nullptr);

private slots:
    void refresh();
    void idSelected();
    void fieldSelected();
    void pageChanged();
    void addToDbc();
    void saveDbc();
    void compare();

private:
    bool timeRange(double &t0, double &t1) const;
    void showAnalysis();

    const QVector<GuiFrame> *frames;
    canre::Capture cap;
    std::vector<canre::IdInfo> ids;
    canre::IdAnalysis current;
    std::vector<canre::DbcMessage> dbc;

    QLineEdit      *fromEdit;
    QLineEdit      *toEdit;
    QLabel         *summary;
    QComboBox      *pageCombo;
    canre::FrameKey currentKey;
    int muxStart = -1, muxLength = 0;
    QTableWidget   *idTable;
    QTableWidget   *heatmap;
    QTableWidget   *fieldTable;
    QPlainTextEdit *dbcView;
    QCustomPlot    *plot;
    QDoubleSpinBox *aStart, *aEnd, *bStart, *bEnd;
    QPlainTextEdit *diffView;
};

#endif // SIGNALFINDERWINDOW_H
