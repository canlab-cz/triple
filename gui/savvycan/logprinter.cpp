// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "logprinter.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDateTime>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QMessageBox>
#include <QPainter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSet>
#include <QTableView>

#include <algorithm>
#include <vector>

#include "logprintlayout.h"

namespace {
const int ASK_ABOVE_ROWS = 5000;      /* confirm before printing more frames than this */
const int ROWS_PER_PAGE_GUESS = 55;   /* landscape A4 / Letter at 8 pt, for the estimate */
const int WIDTH_SAMPLE_ROWS = 5000;   /* rows measured for the natural column widths */

int textWidth(const QFontMetrics &fm, const QString &text)
{
    int w = 0;
    for (const QString &line : text.split('\n'))
        w = qMax(w, fm.horizontalAdvance(line));
    return w;
}
}

LogPrinter::Table LogPrinter::snapshot(QTableView *view, bool selectedOnly, int maxRows)
{
    Table t;
    QAbstractItemModel *m = view->model();
    QHeaderView *hh = view->horizontalHeader();

    QVector<int> cols;                                  /* visible columns, on-screen order */
    for (int v = 0; v < hh->count(); v++)
    {
        int c = hh->logicalIndex(v);
        if (c >= 0 && !view->isColumnHidden(c))
            cols.append(c);
    }
    for (int c : cols)
        t.headers.append(m->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());

    QVector<int> rows;
    if (selectedOnly && view->selectionModel())
    {
        QSet<int> seen;
        for (const QModelIndex &i : view->selectionModel()->selectedIndexes())
            seen.insert(i.row());
        rows = QVector<int>(seen.begin(), seen.end());
        std::sort(rows.begin(), rows.end());
    }
    else
    {
        int n = m->rowCount();
        rows.reserve(n);
        for (int r = 0; r < n; r++)
            rows.append(r);
    }
    if (maxRows >= 0 && rows.size() > maxRows)
        rows.resize(maxRows);

    t.rows.reserve(rows.size());
    for (int r : rows)
    {
        QStringList cells;
        for (int c : cols)
            cells.append(m->index(r, c).data(Qt::DisplayRole).toString().trimmed());
        t.rows.append(cells);
    }
    return t;
}

/*
 * Landscape with 10 mm margins on the default printer. A network printer
 * that cannot be reached reports no page; then the first installed printer
 * that does (e.g. a PDF printer) is used, and the preview's printer dialog
 * still lets the user choose another one.
 */
bool LogPrinter::setupPrinter(QPrinter &printer)
{
    QStringList names{ printer.printerName() };
    for (const QPrinterInfo &info : QPrinterInfo::availablePrinters())
        if (!names.contains(info.printerName()))
            names.append(info.printerName());
    for (const QString &name : names)
    {
        if (name != printer.printerName())
            printer.setPrinterName(name);
        if (!printer.pageLayout().isValid() || printer.pageLayout().fullRect(QPageLayout::Millimeter).width() < 50)
            printer.setPageSize(QPageSize(QLocale().measurementSystem() == QLocale::ImperialUSSystem
                                              ? QPageSize::Letter : QPageSize::A4));
        printer.setPageMargins(QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);
        printer.setPageOrientation(QPageLayout::Landscape);
        if (printer.isValid() && printer.width() > 0 && printer.height() > 0)
            return true;
    }
    return false;
}

void LogPrinter::printView(QWidget *parent, QTableView *view, const QString &title)
{
    if (!view || !view->model())
        return;
    int total = view->model()->rowCount();
    int selected = 0;
    if (view->selectionModel())
    {
        QSet<int> seen;
        for (const QModelIndex &i : view->selectionModel()->selectedIndexes())
            seen.insert(i.row());
        selected = seen.size();
    }

    bool selectedOnly = false;
    if (selected > 1)
    {
        QMessageBox box(QMessageBox::Question, QObject::tr("Print Log"),
                        QObject::tr("Print the %1 selected frames or all %2 frames?").arg(selected).arg(total),
                        QMessageBox::Cancel, parent);
        QPushButton *sel = box.addButton(QObject::tr("Selected"), QMessageBox::AcceptRole);
        QPushButton *all = box.addButton(QObject::tr("All"), QMessageBox::AcceptRole);
        box.setDefaultButton(sel);
        box.exec();
        if (box.clickedButton() != sel && box.clickedButton() != all)
            return;
        selectedOnly = box.clickedButton() == sel;
    }
    int count = selectedOnly ? selected : total;
    if (count > ASK_ABOVE_ROWS
        && QMessageBox::question(parent, QObject::tr("Print Log"),
                                 QObject::tr("%1 frames are about %2 pages. Continue?")
                                     .arg(count).arg(count / ROWS_PER_PAGE_GUESS + 1),
                                 QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    Table table = snapshot(view, selectedOnly, -1);
    QApplication::restoreOverrideCursor();
    table.title = selectedOnly ? QObject::tr("%1, %2 selected frames").arg(title).arg(table.rows.size())
                               : QObject::tr("%1, %2 frames").arg(title).arg(table.rows.size());

    QPrinter printer(QPrinter::HighResolution);
    if (!setupPrinter(printer))
    {
        QMessageBox::warning(parent, QObject::tr("Print Log"), QObject::tr("No usable printer was found."));
        return;
    }
    printer.setDocName(title);
    QPrintPreviewDialog dlg(&printer, parent);
    dlg.setWindowTitle(QObject::tr("Print Log"));
    dlg.resize(1100, 800);
    QObject::connect(&dlg, &QPrintPreviewDialog::paintRequested, &dlg, [&table](QPrinter *p) { paint(p, table); });
    dlg.exec();
}

void LogPrinter::paint(QPrinter *printer, const Table &table)
{
    QPainter p(printer);
    if (!p.isActive())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    paintPages(p, QRect(0, 0, printer->width(), printer->height()), table, [printer]() { return printer->newPage(); });
    QApplication::restoreOverrideCursor();
}

int LogPrinter::paintPages(QPainter &p, const QRect &area, const Table &table, const std::function<bool()> &newPage)
{

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(8);
    QFont bold = font;
    bold.setBold(true);
    QFont titleFont = QApplication::font();
    titleFont.setPointSizeF(10);
    titleFont.setBold(true);

    p.setFont(font);
    const QFontMetrics fm = p.fontMetrics();
    const int lineH = fm.height();
    const int pad = fm.horizontalAdvance(QLatin1Char(' ')) * 2;
    const int cellFlags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere;

    p.setFont(titleFont);
    const int titleH = p.fontMetrics().height() * 3 / 2;
    p.setFont(bold);
    const QFontMetrics fmBold = p.fontMetrics();
    const int headerH = lineH + lineH / 2;
    const int footerH = lineH * 2;
    const int rowsH = area.height() - titleH - headerH - footerH;
    if (rowsH < lineH * 3)
        return 0;

    /* column widths from the header and a sample of the rows */
    const int ncol = table.headers.size();
    std::vector<int> natural(size_t(ncol), 0);
    for (int c = 0; c < ncol; c++)
        natural[size_t(c)] = fmBold.horizontalAdvance(table.headers[c]) + pad;
    int step = qMax(1, table.rows.size() / WIDTH_SAMPLE_ROWS);
    for (int r = 0; r < table.rows.size(); r += step)
        for (int c = 0; c < ncol && c < table.rows[r].size(); c++)
            natural[size_t(c)] = qMax(natural[size_t(c)], textWidth(fm, table.rows[r][c]) + pad);
    std::vector<int> widths = logprint::fitColumns(natural, area.width(), fm.horizontalAdvance("00000") + pad);

    /* row heights (wrapped cells) and pages */
    std::vector<int> heights(size_t(table.rows.size()), lineH);
    for (int r = 0; r < table.rows.size(); r++)
        for (int c = 0; c < ncol && c < table.rows[r].size(); c++)
        {
            const QString &s = table.rows[r][c];
            int w = widths[size_t(c)] - pad;
            if (s.contains('\n') || fm.horizontalAdvance(s) > w)
                heights[size_t(r)] = qMax(heights[size_t(r)], fm.boundingRect(QRect(0, 0, w, 1 << 20), cellFlags, s).height());
        }
    std::vector<int> starts = logprint::paginate(heights, rowsH);
    const int pages = int(starts.size());
    const QString printed = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");

    for (int pg = 0; pg < pages; pg++)
    {
        if (pg && !newPage())
            return pg;
        int y = 0;
        /* set the pen explicitly: the print preview records only state that
         * changed, and replays unrecorded state with its own (theme coloured,
         * white in a dark theme) pen */
        p.setPen(Qt::NoPen);
        p.setPen(QPen(Qt::black, qMax(1, lineH / 16)));
        p.setBrush(Qt::NoBrush);
        p.setFont(titleFont);
        p.drawText(QRect(0, 0, area.width(), titleH), Qt::AlignLeft | Qt::AlignTop, table.title);
        p.setFont(font);
        p.drawText(QRect(0, 0, area.width(), titleH), Qt::AlignRight | Qt::AlignTop, printed);
        y += titleH;

        p.setFont(bold);
        int x = 0;
        for (int c = 0; c < ncol; c++)
        {
            p.drawText(QRect(x, y, widths[size_t(c)] - pad / 2, headerH), Qt::AlignLeft | Qt::AlignVCenter, table.headers[c]);
            x += widths[size_t(c)];
        }
        y += headerH;
        p.drawLine(0, y - 1, area.width(), y - 1);

        p.setFont(font);
        int end = pg + 1 < pages ? starts[size_t(pg + 1)] : table.rows.size();
        for (int r = starts[size_t(pg)]; r < end; r++)
        {
            int h = qMin(heights[size_t(r)], rowsH);
            if (r % 2)
                p.fillRect(QRect(0, y, area.width(), h), QColor(238, 238, 238));
            x = 0;
            for (int c = 0; c < ncol && c < table.rows[r].size(); c++)
            {
                QRect cell(x, y, widths[size_t(c)] - pad / 2, h);
                p.save();
                p.setClipRect(cell);
                p.drawText(cell, cellFlags, table.rows[r][c]);
                p.restore();
                x += widths[size_t(c)];
            }
            y += h;
        }

        int fy = area.height() - footerH + lineH / 2;
        p.drawLine(0, fy - lineH / 4, area.width(), fy - lineH / 4);
        p.drawText(QRect(0, fy, area.width(), lineH), Qt::AlignLeft, QObject::tr("SavvyCAN"));
        p.drawText(QRect(0, fy, area.width(), lineH), Qt::AlignRight,
                   QObject::tr("Page %1 of %2").arg(pg + 1).arg(pages));
    }
    return pages;
}
