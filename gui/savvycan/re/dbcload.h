// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef DBCLOAD_H
#define DBCLOAD_H

/* Saves DBC text and, if the user wants, loads it into SavvyCAN's DBC
 * handler so the frame list decodes the signals (Interpret Frames). A file
 * loaded earlier from the same path is replaced, not loaded twice. */

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QWidget>

#include "dbc/dbchandler.h"

/* Returns a status line for the window, empty if the user cancelled. */
inline QString saveAndLoadDbc(QWidget *parent, const QByteArray &dbcText)
{
    QSettings settings;
    QString path = QFileDialog::getSaveFileName(parent, QObject::tr("Save DBC"), settings.value("SignalFinder/dbcPath").toString(),
                                                QObject::tr("DBC (*.dbc)"));
    if (path.isEmpty())
        return QString();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(parent, QObject::tr("Save DBC"), QObject::tr("Cannot write %1").arg(path));
        return QObject::tr("could not write %1").arg(path);
    }
    f.write(dbcText);
    f.close();
    settings.setValue("SignalFinder/dbcPath", path);

    if (QMessageBox::question(parent, QObject::tr("Save DBC"),
                              QObject::tr("Load it into SavvyCAN now, so the frame list decodes the signals (Interpret Frames)?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
        return QObject::tr("saved %1").arg(path);

    DBCHandler *h = DBCHandler::getReference();
    QString target = QFileInfo(path).absoluteFilePath();
    for (int i = h->getFileCount() - 1; i >= 0; i--)
    {
        DBCFile *old = h->getFileByIdx(i);
        if (old && QFileInfo(old->getFullFilename()).absoluteFilePath() == target)
            h->removeDBCFile(i);
    }
    if (!h->loadDBCFile(path))
        return QObject::tr("saved %1, but SavvyCAN could not load it").arg(path);
    return QObject::tr("saved and loaded %1; tick Interpret Frames in the main window to see the signals").arg(path);
}

#endif // DBCLOAD_H
