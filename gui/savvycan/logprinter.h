// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef LOGPRINTER_H
#define LOGPRINTER_H

/*
 * File > Print Log: prints the frame list as it is shown (visible columns
 * in their on-screen order, current filter) or only the selected rows,
 * through a print preview, so it also goes to PDF with any PDF printer.
 */

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QPainter;
class QPrinter;
class QRect;
class QTableView;
class QWidget;

class LogPrinter
{
public:
    /* Snapshot of the table taken when the dialog opens. */
    struct Table {
        QString title;
        QStringList headers;
        QVector<QStringList> rows;
    };

    static void printView(QWidget *parent, QTableView *view, const QString &title);

    /* Paints all pages; public so it can be used without the dialog. */
    static void paint(QPrinter *printer, const Table &table);

    /* Paints the pages into `area` of any painter; newPage() starts the
     * next page and returns false to stop. Returns the page count. */
    static int paintPages(QPainter &p, const QRect &area, const Table &table, const std::function<bool()> &newPage);

private:
    static bool setupPrinter(QPrinter &printer);
    static Table snapshot(QTableView *view, bool selectedOnly, int maxRows);
};

#endif // LOGPRINTER_H
