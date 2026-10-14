// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "signalfinderwindow.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

#include "recapture.h"

namespace {
QTableWidgetItem *numItem(double v, int decimals = 0)
{
    QTableWidgetItem *it = new QTableWidgetItem;
    it->setData(Qt::DisplayRole, decimals ? QVariant(QString::number(v, 'f', decimals).toDouble()) : QVariant(qlonglong(v)));
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}
QString keyStr(const canre::FrameKey &k) { return QString::fromStdString(canre::keyText(k)); }
}

SignalFinderWindow::SignalFinderWindow(const QVector<GuiFrame> *frameList, QWidget *parent)
    : QWidget(parent, Qt::Window), frames(frameList)
{
    setWindowTitle(tr("Signal Finder"));
    resize(1300, 820);

    fromEdit = new QLineEdit(this);
    toEdit = new QLineEdit(this);
    fromEdit->setPlaceholderText(tr("start"));
    toEdit->setPlaceholderText(tr("end"));
    fromEdit->setMaximumWidth(90);
    toEdit->setMaximumWidth(90);
    QPushButton *refreshBtn = new QPushButton(tr("Analyze capture"), this);
    refreshBtn->setToolTip(tr("Takes the frames captured so far; the time range limits the per-ID analysis"));
    summary = new QLabel(this);
    QHBoxLayout *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Time range (s, empty = all):"), this));
    top->addWidget(fromEdit);
    top->addWidget(new QLabel("-", this));
    top->addWidget(toEdit);
    top->addWidget(refreshBtn);
    top->addWidget(summary, 1);

    /* --- IDs and fields ---------------------------------------------------- */
    idTable = new QTableWidget(0, 8, this);
    idTable->setHorizontalHeaderLabels({ tr("ID"), tr("Bus"), tr("Frames"), tr("Period ms"), tr("Jitter ms"), tr("Len"),
                                         tr("Changing bytes"), tr("Distinct") });
    idTable->verticalHeader()->setVisible(false);
    idTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    idTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    idTable->setSelectionMode(QAbstractItemView::SingleSelection);
    idTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    idTable->setSortingEnabled(true);

    heatmap = new QTableWidget(0, 8, this);
    heatmap->setHorizontalHeaderLabels({ "7", "6", "5", "4", "3", "2", "1", "0" });
    heatmap->setEditTriggers(QAbstractItemView::NoEditTriggers);
    heatmap->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    heatmap->setToolTip(tr("Bit flip rate: how often the bit changes from one frame to the next (rows = bytes)"));

    fieldTable = new QTableWidget(0, 9, this);
    fieldTable->setHorizontalHeaderLabels({ tr("Use"), tr("Name"), tr("Kind"), tr("Start bit"), tr("Length"), tr("Byte order"),
                                            tr("Raw range"), tr("Values"), tr("Note") });
    fieldTable->verticalHeader()->setVisible(false);
    fieldTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    fieldTable->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);

    QPushButton *addBtn = new QPushButton(tr("Add checked fields to DBC"), this);
    QPushButton *saveBtn = new QPushButton(tr("Save DBC..."), this);
    dbcView = new QPlainTextEdit(this);
    dbcView->setReadOnly(true);
    dbcView->setPlaceholderText(tr("DBC built from the checked fields; names are editable in the table"));
    QHBoxLayout *dbcRow = new QHBoxLayout;
    dbcRow->addWidget(addBtn);
    dbcRow->addWidget(saveBtn);
    dbcRow->addStretch(1);

    QWidget *fieldsBox = new QWidget(this);
    QVBoxLayout *fl = new QVBoxLayout(fieldsBox);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->addWidget(new QLabel(tr("Detected fields (Intel start = LSB, Motorola start = MSB, DBC numbering):"), this));
    fl->addWidget(fieldTable, 3);
    fl->addLayout(dbcRow);
    fl->addWidget(dbcView, 2);

    QSplitter *right = new QSplitter(Qt::Vertical, this);
    right->addWidget(heatmap);
    right->addWidget(fieldsBox);
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 3);
    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(idTable);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    /* --- compare windows ---------------------------------------------------- */
    auto spin = [this]() {
        QDoubleSpinBox *s = new QDoubleSpinBox(this);
        s->setDecimals(3);
        s->setRange(0, 1e7);
        s->setSuffix(" s");
        return s;
    };
    aStart = spin(); aEnd = spin(); bStart = spin(); bEnd = spin();
    QPushButton *cmpBtn = new QPushButton(tr("Compare"), this);
    QHBoxLayout *cmpRow = new QHBoxLayout;
    cmpRow->addWidget(new QLabel(tr("Window A"), this));
    cmpRow->addWidget(aStart);
    cmpRow->addWidget(aEnd);
    cmpRow->addSpacing(20);
    cmpRow->addWidget(new QLabel(tr("Window B"), this));
    cmpRow->addWidget(bStart);
    cmpRow->addWidget(bEnd);
    cmpRow->addWidget(cmpBtn);
    cmpRow->addStretch(1);
    diffView = new QPlainTextEdit(this);
    diffView->setReadOnly(true);
    diffView->setFont(QFont("Monospace"));
    diffView->setPlaceholderText(tr("Capture a baseline, then do something (press a button, turn a knob) and compare the two "
                                    "time ranges: IDs that appear or vanish, rate changes, and the bits that behave differently."));
    QWidget *cmpPage = new QWidget(this);
    QVBoxLayout *cl = new QVBoxLayout(cmpPage);
    cl->addLayout(cmpRow);
    cl->addWidget(diffView);

    QTabWidget *tabs = new QTabWidget(this);
    tabs->addTab(split, tr("IDs and fields"));
    tabs->addTab(cmpPage, tr("Compare time windows"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(tabs);

    connect(refreshBtn, &QPushButton::clicked, this, &SignalFinderWindow::refresh);
    connect(idTable, &QTableWidget::itemSelectionChanged, this, &SignalFinderWindow::idSelected);
    connect(addBtn, &QPushButton::clicked, this, &SignalFinderWindow::addToDbc);
    connect(saveBtn, &QPushButton::clicked, this, &SignalFinderWindow::saveDbc);
    connect(cmpBtn, &QPushButton::clicked, this, &SignalFinderWindow::compare);

    refresh();
}

bool SignalFinderWindow::timeRange(double &t0, double &t1) const
{
    bool ok0 = true, ok1 = true;
    t0 = fromEdit->text().trimmed().isEmpty() ? -1 : fromEdit->text().toDouble(&ok0);
    t1 = toEdit->text().trimmed().isEmpty() ? -1 : toEdit->text().toDouble(&ok1);
    return ok0 && ok1;
}

void SignalFinderWindow::refresh()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    fillCapture(cap, *frames);
    ids = canre::listIds(cap);
    QApplication::restoreOverrideCursor();

    summary->setText(tr("%1 frames, %2 s, %3 IDs").arg(cap.size()).arg(cap.spanSeconds(), 0, 'f', 3).arg(ids.size()));
    idTable->setSortingEnabled(false);
    idTable->setRowCount(int(ids.size()));
    for (int r = 0; r < int(ids.size()); r++)
    {
        const canre::IdInfo &i = ids[size_t(r)];
        QTableWidgetItem *idItem = new QTableWidgetItem(keyStr(i.key).section('@', 0, 0));
        idItem->setData(Qt::UserRole, r);
        idTable->setItem(r, 0, idItem);
        idTable->setItem(r, 1, numItem(i.key.bus));
        idTable->setItem(r, 2, numItem(double(i.count)));
        idTable->setItem(r, 3, numItem(i.periodMs, 1));
        idTable->setItem(r, 4, numItem(i.jitterMs, 1));
        QString lens;
        for (uint8_t l : i.lengths) lens += (lens.isEmpty() ? "" : "/") + QString::number(l);
        idTable->setItem(r, 5, new QTableWidgetItem(lens));
        idTable->setItem(r, 6, numItem(i.changingBytes));
        idTable->setItem(r, 7, numItem(i.uniquePayloads));
    }
    idTable->setSortingEnabled(true);

    double span = cap.spanSeconds();
    for (QDoubleSpinBox *s : { aStart, aEnd, bStart, bEnd }) s->setMaximum(std::max(span, 0.001));
    if (aEnd->value() == 0 && bEnd->value() == 0)
    {
        aStart->setValue(0); aEnd->setValue(span / 2);
        bStart->setValue(span / 2); bEnd->setValue(span);
    }
}

void SignalFinderWindow::idSelected()
{
    QList<QTableWidgetItem *> sel = idTable->selectedItems();
    if (sel.isEmpty()) return;
    QTableWidgetItem *idItem = idTable->item(sel.first()->row(), 0);
    int idx = idItem->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= int(ids.size())) return;
    double t0, t1;
    if (!timeRange(t0, t1))
    {
        summary->setText(tr("time range: numbers in seconds"));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    current = canre::analyzeId(cap, ids[size_t(idx)].key, t0, t1);
    QApplication::restoreOverrideCursor();

    heatmap->setRowCount(current.length);
    QStringList rows;
    for (int b = 0; b < current.length; b++)
    {
        rows << tr("byte %1").arg(b);
        for (int k = 7; k >= 0; k--)
        {
            double rate = current.flipRate[size_t(b * 8 + k)];
            double one = current.oneRate[size_t(b * 8 + k)];
            QTableWidgetItem *it = new QTableWidgetItem(rate > 0 ? QString::number(rate, 'f', 2) : (one > 0.5 ? "1" : "0"));
            double v = std::sqrt(rate);
            it->setBackground(QColor(255, int(255 * (1 - v)), int(255 * (1 - v))));
            it->setForeground(Qt::black);
            it->setTextAlignment(Qt::AlignCenter);
            it->setToolTip(tr("byte %1 bit %2 (position %3): flips %4, set in %5 % of frames")
                               .arg(b).arg(k).arg(b * 8 + k).arg(rate, 0, 'f', 3).arg(one * 100, 0, 'f', 1));
            heatmap->setItem(b, 7 - k, it);
        }
    }
    heatmap->setVerticalHeaderLabels(rows);

    fieldTable->setRowCount(int(current.fields.size()));
    for (int r = 0; r < int(current.fields.size()); r++)
    {
        const canre::Field &f = current.fields[size_t(r)];
        QTableWidgetItem *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        use->setCheckState(f.kind == canre::FieldKind::Signal || f.kind == canre::FieldKind::Counter ||
                                   f.kind == canre::FieldKind::Checksum ? Qt::Checked : Qt::Unchecked);
        fieldTable->setItem(r, 0, use);
        fieldTable->setItem(r, 1, new QTableWidgetItem(QString("%1_%2").arg(canre::kindName(f.kind)).arg(f.startBit)));
        auto ro = [](const QString &t) { QTableWidgetItem *i = new QTableWidgetItem(t); i->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); return i; };
        fieldTable->setItem(r, 2, ro(canre::kindName(f.kind)));
        fieldTable->setItem(r, 3, ro(QString::number(f.startBit)));
        fieldTable->setItem(r, 4, ro(QString::number(f.length)));
        fieldTable->setItem(r, 5, ro(f.length == 1 || (f.startBit / 8 == (f.startBit + f.length - 1) / 8 && !f.bigEndian) ? "-" : f.bigEndian ? "Motorola" : "Intel"));
        fieldTable->setItem(r, 6, ro(QString("%1 .. %2").arg(qulonglong(f.minRaw)).arg(qulonglong(f.maxRaw))));
        fieldTable->setItem(r, 7, ro(QString::number(f.unique)));
        fieldTable->setItem(r, 8, ro(QString::fromStdString(f.note)));
    }
    summary->setText(tr("%1: %2 frames of %3 bytes analyzed").arg(keyStr(current.key)).arg(current.frames).arg(current.length));
}

void SignalFinderWindow::addToDbc()
{
    if (current.frames == 0) return;
    canre::DbcMessage *m = nullptr;
    for (canre::DbcMessage &x : dbc)
        if (x.key == current.key) m = &x;
    if (!m)
    {
        canre::DbcMessage nm;
        nm.key = current.key;
        nm.name = "MSG_" + canre::keyText(current.key).substr(0, canre::keyText(current.key).find('@'));
        nm.length = current.length;
        dbc.push_back(nm);
        m = &dbc.back();
    }
    for (int r = 0; r < fieldTable->rowCount() && r < int(current.fields.size()); r++)
    {
        if (fieldTable->item(r, 0)->checkState() != Qt::Checked) continue;
        const canre::Field &f = current.fields[size_t(r)];
        canre::DbcSignal s;
        s.name = fieldTable->item(r, 1)->text().toStdString();
        s.startBit = f.startBit;
        s.length = f.length;
        s.bigEndian = f.bigEndian;
        s.minV = double(f.minRaw);
        s.maxV = double(f.maxRaw);
        s.comment = std::string(canre::kindName(f.kind)) + (f.note.empty() ? "" : ": " + f.note);
        auto same = std::find_if(m->signals_.begin(), m->signals_.end(), [&](const canre::DbcSignal &o) { return o.name == s.name; });
        if (same != m->signals_.end()) *same = s;
        else m->signals_.push_back(s);
    }
    dbcView->setPlainText(QString::fromStdString(canre::toDbc(dbc)));
}

void SignalFinderWindow::saveDbc()
{
    if (dbc.empty())
    {
        QMessageBox::information(this, tr("Signal Finder"), tr("Add fields to the DBC first."));
        return;
    }
    QSettings settings;
    QString path = QFileDialog::getSaveFileName(this, tr("Save DBC"), settings.value("SignalFinder/dbcPath").toString(), tr("DBC (*.dbc)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, tr("Signal Finder"), tr("Cannot write %1").arg(path));
        return;
    }
    f.write(QByteArray::fromStdString(canre::toDbc(dbc)));
    settings.setValue("SignalFinder/dbcPath", path);
    summary->setText(tr("saved %1; load it with File > DBC File Manager").arg(path));
}

void SignalFinderWindow::compare()
{
    if (aEnd->value() <= aStart->value() || bEnd->value() <= bStart->value())
    {
        diffView->setPlainText(tr("Each window needs start < end."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    canre::WindowDiff w = canre::diffWindows(cap, aStart->value(), aEnd->value(), bStart->value(), bEnd->value(), 60);
    QApplication::restoreOverrideCursor();
    diffView->setPlainText(QString::fromStdString(canre::describe(w)));
}
