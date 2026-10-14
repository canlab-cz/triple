// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "pp2canlog.h"
#include "pp2canlogfmt.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>

namespace PP2CANLog {

bool isFile(const QString &filename)
{
    QFile f(filename);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray l1 = f.readLine(256).trimmed();
    QByteArray l2 = f.readLine(256).trimmed();
    return pp2canlog::isHeader(l1.toStdString(), l2.toStdString());
}

bool load(const QString &filename, QVector<GuiFrame> *frames)
{
    QFile f(filename);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    const qint64 DAY = 24ll * 3600 * 1000000;
    qint64 dayOffset = 0, last = -1;
    int parsed = 0, lineNo = 0;
    pp2canlog::Frame pf;

    while (!f.atEnd())
    {
        QByteArray line = f.readLine();
        lineNo++;
        if (!pp2canlog::parseLine(line.constData(), size_t(line.size()), pf))
        {
            if (lineNo <= 5)
                continue;                       /* header */
            if (line.trimmed().isEmpty())
                continue;
            if (parsed == 0 && lineNo > 50)
                return false;                   /* not a PP2CAN log after all */
            continue;                           /* skip a damaged line, keep going */
        }
        qint64 t = pf.timeUs + dayOffset;
        if (last >= 0 && t + DAY / 2 < last)    /* passed midnight */
        {
            dayOffset += DAY;
            t += DAY;
        }
        last = t;

        GuiFrame fr = guiMakeFrame(0, pf.id, pf.ext,
                                   QByteArray(reinterpret_cast<const char *>(pf.data), pf.rtr ? 0 : pf.dlc));
        if (pf.rtr)
            fr.setFrameType(GuiFrame::RemoteRequestFrame);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        fr.setReceived(pf.rx);
#else
        fr.isReceived = pf.rx;
#endif
        fr.setTimeStamp(GuiFrame::TimeStamp(0, t));
        frames->append(fr);
        parsed++;
    }
    return parsed > 0;
}

bool save(const QString &filename, const QVector<GuiFrame> *frames, const QString &speed)
{
    QFile f(filename);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    bool anyRx = false;
    for (const GuiFrame &fr : *frames)
        if (guiFrameReceived(fr))
        {
            anyRx = true;
            break;
        }
    QDateTime now = QDateTime::currentDateTime();
    std::string date = now.toString("d-M-yyyy  HH-mm").toStdString();
    f.write(QByteArray::fromStdString(pp2canlog::header(anyRx || frames->isEmpty(), date, speed.toStdString())));

    uint32_t seq = 1;
    for (const GuiFrame &fr : *frames)
    {
        if (fr.frameType() != GUI_DATA_FRAME && fr.frameType() != GuiFrame::RemoteRequestFrame)
            continue;
        pp2canlog::Frame pf;
        pf.rx = guiFrameReceived(fr);
        pf.seq = seq++;
        pf.timeUs = guiFrameMicros(fr);
        pf.ext = fr.hasExtendedFrameFormat();
        pf.rtr = fr.frameType() == GuiFrame::RemoteRequestFrame;
        pf.id = fr.frameId();
        QByteArray d = fr.payload();
        if (d.size() > 8)
            continue;                           /* CAN FD frames do not fit the format */
        pf.dlc = uint8_t(d.size());
        memcpy(pf.data, d.constData(), size_t(d.size()));
        f.write(QByteArray::fromStdString(pp2canlog::formatLine(pf)));
        f.write("\r\n");
    }
    return true;
}

void saveWithDialog(QWidget *parent, const QVector<GuiFrame> *frames)
{
    QSettings settings;
    QString file = QFileDialog::getSaveFileName(parent, QObject::tr("Save Log as PP2CAN"),
                                                settings.value("FileIO/LoadSaveDirectory").toString(),
                                                QObject::tr("PP2CAN Log (*.log *.LOG)"));
    if (file.isEmpty())
        return;
    if (!file.contains('.'))
        file += ".log";
    bool ok;
    QString speed = QInputDialog::getText(parent, QObject::tr("Save Log as PP2CAN"),
                                          QObject::tr("CAN speed for the header (for example 250k)"),
                                          QLineEdit::Normal, "250k", &ok);
    if (!ok)
        return;
    if (!save(file, frames, speed))
        QMessageBox::warning(parent, QObject::tr("Save Log as PP2CAN"), QObject::tr("Could not write %1").arg(file));
}

}
