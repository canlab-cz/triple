#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Add the CANLAB USB2CAN Triple connection type to an upstream SavvyCAN checkout.

    python3 gui/savvycan/apply.py /path/to/SavvyCAN

Copies connections/triple_serial.[h,cpp] and the shared codec
(driver/triple_proto.[h,c]) into the checkout and makes the small edits that
register the new connection type (project file, type enum, factory, new
connection dialog, connection window). Safe to run twice.
Tested against SavvyCAN e439670 (2026-10) with Qt 5.15.
"""
import os
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
S = os.path.abspath(sys.argv[1])
R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def patch(rel, old, new):
    p = os.path.join(S, rel)
    s = open(p, encoding="utf-8").read()
    if new in s:
        print("already applied:", rel)
        return
    assert s.count(old) == 1, "%s: anchor count %d for %r" % (rel, s.count(old), old[:60])
    open(p, "w", encoding="utf-8").write(s.replace(old, new))
    print("patched", rel)


for f in ("triple_serial.h", "triple_serial.cpp"):
    shutil.copy(os.path.join(R, "gui", "savvycan", "connections", f), os.path.join(S, "connections", f))
shutil.copy(os.path.join(R, "driver", "include", "triple_proto.h"), os.path.join(S, "connections", "triple_proto.h"))
shutil.copy(os.path.join(R, "driver", "triple_proto.c"), os.path.join(S, "connections", "triple_proto.c"))
for f in ("busstatswindow.h", "busstatswindow.cpp", "canopenwindow.h", "canopenwindow.cpp",
          "savvycompat.h", "canidcalc.h", "canopen_eds.h", "cancalcwindow.h", "cancalcwindow.cpp",
          "responsemeterwindow.h", "responsemeterwindow.cpp", "datapanelwindow.h", "datapanelwindow.cpp",
          "canre.h", "aitools.h", "recapture.h", "signalfinderwindow.h", "signalfinderwindow.cpp",
          "aiassistantwindow.h", "aiassistantwindow.cpp", "dbcload.h", "canopen_block.h"):
    shutil.copy(os.path.join(R, "gui", "savvycan", "re", f), os.path.join(S, "re", f))
for f in ("pp2canlog.h", "pp2canlog.cpp", "pp2canlogfmt.h", "logprinter.h", "logprinter.cpp", "logprintlayout.h"):
    shutil.copy(os.path.join(R, "gui", "savvycan", f), os.path.join(S, f))
print("copied connection sources into", os.path.join(S, "connections"))

BS = " \\\n"  # backslash-newline continuation in the .pro file

patch("SavvyCAN.pro",
      "    connections/lawicel_serial.cpp" + BS,
      "    connections/lawicel_serial.cpp" + BS + "    connections/triple_serial.cpp" + BS + "    connections/triple_proto.c" + BS)
patch("SavvyCAN.pro",
      "    connections/lawicel_serial.h" + BS,
      "    connections/lawicel_serial.h" + BS + "    connections/triple_serial.h" + BS + "    connections/triple_proto.h" + BS)

if os.path.exists(os.path.join(S, "CMakeLists.txt")):
    patch("CMakeLists.txt",
          "    connections/lawicel_serial.cpp connections/lawicel_serial.h\n",
          "    connections/lawicel_serial.cpp connections/lawicel_serial.h\n"
          "    connections/triple_serial.cpp connections/triple_serial.h\n"
          "    connections/triple_proto.c connections/triple_proto.h\n")
    patch("CMakeLists.txt",
          "    re/temporalgraphwindow.cpp re/temporalgraphwindow.h\n",
          "    re/temporalgraphwindow.cpp re/temporalgraphwindow.h\n"
          "    re/busstatswindow.cpp re/busstatswindow.h\n")
    patch("CMakeLists.txt",
          "    re/busstatswindow.cpp re/busstatswindow.h\n",
          "    re/busstatswindow.cpp re/busstatswindow.h\n"
          "    re/canopenwindow.cpp re/canopenwindow.h\n")
    patch("CMakeLists.txt",
          "    framefileio.cpp framefileio.h\n",
          "    framefileio.cpp framefileio.h\n"
          "    pp2canlog.cpp pp2canlog.h pp2canlogfmt.h\n")
    patch("CMakeLists.txt",
          "    pp2canlog.cpp pp2canlog.h pp2canlogfmt.h\n",
          "    pp2canlog.cpp pp2canlog.h pp2canlogfmt.h\n"
          "    logprinter.cpp logprinter.h logprintlayout.h\n")
    patch("CMakeLists.txt",
          "    re/canopenwindow.cpp re/canopenwindow.h\n",
          "    re/canopenwindow.cpp re/canopenwindow.h\n"
          "    re/savvycompat.h re/canidcalc.h\n"
          "    re/cancalcwindow.cpp re/cancalcwindow.h\n"
          "    re/responsemeterwindow.cpp re/responsemeterwindow.h\n"
          "    re/datapanelwindow.cpp re/datapanelwindow.h\n")
    patch("CMakeLists.txt",
          "    re/datapanelwindow.cpp re/datapanelwindow.h\n",
          "    re/datapanelwindow.cpp re/datapanelwindow.h\n"
          "    re/canopen_eds.h\n")
    patch("CMakeLists.txt",
          "    re/canopen_eds.h\n",
          "    re/canopen_eds.h\n"
          "    re/canre.h re/aitools.h re/recapture.h\n"
          "    re/signalfinderwindow.cpp re/signalfinderwindow.h\n"
          "    re/aiassistantwindow.cpp re/aiassistantwindow.h\n")
    patch("CMakeLists.txt",
          "    re/aiassistantwindow.cpp re/aiassistantwindow.h\n",
          "    re/aiassistantwindow.cpp re/aiassistantwindow.h\n"
          "    re/dbcload.h\n")
    patch("CMakeLists.txt",
          "    re/dbcload.h\n",
          "    re/dbcload.h\n"
          "    re/canopen_block.h\n")

patch("SavvyCAN.pro",
      "    re/temporalgraphwindow.cpp" + BS,
      "    re/temporalgraphwindow.cpp" + BS + "    re/busstatswindow.cpp" + BS)
patch("SavvyCAN.pro",
      "    re/temporalgraphwindow.h" + BS,
      "    re/temporalgraphwindow.h" + BS + "    re/busstatswindow.h" + BS)
patch("SavvyCAN.pro",
      "    re/busstatswindow.cpp" + BS,
      "    re/busstatswindow.cpp" + BS + "    re/canopenwindow.cpp" + BS)
patch("SavvyCAN.pro",
      "    re/busstatswindow.h" + BS,
      "    re/busstatswindow.h" + BS + "    re/canopenwindow.h" + BS)
patch("SavvyCAN.pro",
      "    re/canopenwindow.cpp" + BS,
      "    re/canopenwindow.cpp" + BS + "    re/cancalcwindow.cpp" + BS + "    re/responsemeterwindow.cpp" + BS
      + "    re/datapanelwindow.cpp" + BS)
patch("SavvyCAN.pro",
      "    re/canopenwindow.h" + BS,
      "    re/canopenwindow.h" + BS + "    re/savvycompat.h" + BS + "    re/canidcalc.h" + BS + "    re/cancalcwindow.h" + BS
      + "    re/responsemeterwindow.h" + BS + "    re/datapanelwindow.h" + BS)

patch("SavvyCAN.pro",
      "    re/datapanelwindow.h" + BS,
      "    re/datapanelwindow.h" + BS + "    re/canopen_eds.h" + BS)
patch("SavvyCAN.pro",
      "    re/canopen_eds.h" + BS,
      "    re/canopen_eds.h" + BS + "    re/canre.h" + BS + "    re/aitools.h" + BS + "    re/recapture.h" + BS
      + "    re/signalfinderwindow.h" + BS + "    re/aiassistantwindow.h" + BS)
patch("SavvyCAN.pro",
      "    re/aiassistantwindow.h" + BS,
      "    re/aiassistantwindow.h" + BS + "    re/dbcload.h" + BS)
patch("SavvyCAN.pro",
      "    re/dbcload.h" + BS,
      "    re/dbcload.h" + BS + "    re/canopen_block.h" + BS)

patch("SavvyCAN.pro",
      "    framefileio.cpp" + BS,
      "    framefileio.cpp" + BS + "    pp2canlog.cpp" + BS)
patch("SavvyCAN.pro",
      "    framefileio.h" + BS,
      "    framefileio.h" + BS + "    pp2canlog.h" + BS + "    pp2canlogfmt.h" + BS)
patch("SavvyCAN.pro",
      "    pp2canlog.cpp" + BS,
      "    pp2canlog.cpp" + BS + "    logprinter.cpp" + BS)
patch("SavvyCAN.pro",
      "    logprinter.cpp" + BS,
      "    logprinter.cpp" + BS + "    re/signalfinderwindow.cpp" + BS + "    re/aiassistantwindow.cpp" + BS)
patch("SavvyCAN.pro",
      "    pp2canlogfmt.h" + BS,
      "    pp2canlogfmt.h" + BS + "    logprinter.h" + BS + "    logprintlayout.h" + BS)

# PP2CAN logs in the Load Log File dialog and autodetection (and therefore
# in Playback, the file sender). Filter and dispatch go after the last
# entry, which differs between master and QT6WIP.
FIO = "framefileio.cpp"
patch(FIO, '#include "framefileio.h"\n', '#include "framefileio.h"\n#include "pp2canlog.h"\n')
PP_FILTER = '    filters.append(QString(tr("PP2CAN Log (*.log *.LOG)")));\n'
PP_DISPATCH = ('        if (selectedNameFilter == tr("PP2CAN Log (*.log *.LOG)")) '
               'result = PP2CANLog::load(filename, frameCache);\n')
fio = open(os.path.join(S, FIO), encoding="utf-8").read()
if "CANWise Log (*.log)" in fio:                               # QT6WIP
    patch(FIO, '    filters.append(QString(tr("CANWise Log (*.log)")));                     // 27\n',
          '    filters.append(QString(tr("CANWise Log (*.log)")));                     // 27\n' + PP_FILTER)
    patch(FIO, "        if (selectedNameFilter == filters[27]) result = loadCanWise(filename, frameCache);\n",
          "        if (selectedNameFilter == filters[27]) result = loadCanWise(filename, frameCache);\n" + PP_DISPATCH)
else:                                                          # master
    patch(FIO, '    filters.append(QString(tr("Wireshark SocketCAN (*.pcap *.PCAP")));\n',
          '    filters.append(QString(tr("Wireshark SocketCAN (*.pcap *.PCAP")));\n' + PP_FILTER)
    patch(FIO, "        if (selectedNameFilter == filters[25]) result = loadWiresharkSocketCANFile(filename, frameCache);\n",
          "        if (selectedNameFilter == filters[25]) result = loadWiresharkSocketCANFile(filename, frameCache);\n" + PP_DISPATCH)
patch(FIO, '    qDebug() << "Attempting Canalyzer BLF";\n',
      '    if (PP2CANLog::isFile(filename) && PP2CANLog::load(filename, frames))\n'
      '        return true;\n\n'
      '    qDebug() << "Attempting Canalyzer BLF";\n')

# Bus Statistics window in the RE Tools menu
patch("mainwindow.cpp",
      '#include "connections/connectionwindow.h"\n',
      '#include "connections/connectionwindow.h"\n#include "re/busstatswindow.h"\n#include <QPointer>\n')
patch("mainwindow.cpp",
      "    //Automatically create the connection window so it can be updated even if we never opened it.\n",
      "    connect(ui->menu_RE_Tools->addAction(tr(\"Bus Statistics\")), &QAction::triggered, this, [this]() {\n"
      "        static QPointer<BusStatsWindow> busStatsWindow;\n"
      "        if (!busStatsWindow) busStatsWindow = new BusStatsWindow(this);\n"
      "        busStatsWindow->show();\n"
      "        busStatsWindow->raise();\n"
      "        busStatsWindow->activateWindow();\n"
      "    });\n\n"
      "    //Automatically create the connection window so it can be updated even if we never opened it.\n")

# CANopen window, menu entry just before Bus Statistics
patch("mainwindow.cpp",
      '#include "re/busstatswindow.h"\n#include <QPointer>\n',
      '#include "re/busstatswindow.h"\n#include <QPointer>\n#include "re/canopenwindow.h"\n')
patch("mainwindow.cpp",
      "    connect(ui->menu_RE_Tools->addAction(tr(\"Bus Statistics\")), &QAction::triggered, this, [this]() {\n",
      "    connect(ui->menu_RE_Tools->addAction(tr(\"CANopen\")), &QAction::triggered, this, [this]() {\n"
      "        static QPointer<CANopenWindow> canopenWindow;\n"
      "        if (!canopenWindow) canopenWindow = new CANopenWindow(this);\n"
      "        canopenWindow->show();\n"
      "        canopenWindow->raise();\n"
      "        canopenWindow->activateWindow();\n"
      "    });\n"
      "    connect(ui->menu_RE_Tools->addAction(tr(\"Bus Statistics\")), &QAction::triggered, this, [this]() {\n")

# Response Meter and CAN Calculator (RE Tools), Data Panel (Send Frames)
patch("mainwindow.cpp",
      '#include "re/canopenwindow.h"\n',
      '#include "re/canopenwindow.h"\n#include "re/cancalcwindow.h"\n#include "re/responsemeterwindow.h"\n'
      '#include "re/datapanelwindow.h"\n')
TOOL = ("    connect(ui->{menu}->addAction(tr(\"{title}\")), &QAction::triggered, this, [this]() {{\n"
        "        static QPointer<{cls}> w;\n"
        "        if (!w) w = new {cls}(this);\n"
        "        w->show();\n"
        "        w->raise();\n"
        "        w->activateWindow();\n"
        "    }});\n")
patch("mainwindow.cpp",
      "    connect(ui->menu_RE_Tools->addAction(tr(\"CANopen\")), &QAction::triggered, this, [this]() {\n",
      TOOL.format(menu="menuSend_Frames", title="Data Panel", cls="DataPanelWindow")
      + TOOL.format(menu="menu_RE_Tools", title="Response Meter", cls="ResponseMeterWindow")
      + TOOL.format(menu="menu_RE_Tools", title="CAN Calculator", cls="CanCalcWindow")
      + "    connect(ui->menu_RE_Tools->addAction(tr(\"CANopen\")), &QAction::triggered, this, [this]() {\n")

patch("connections/canconconst.h",
      "        CANLOGSERVER,\n        NONE\n",
      "        CANLOGSERVER,\n        TRIPLE,         /*!< CANLAB USB2CAN Triple over serial */\n        NONE\n")

patch("connections/canconfactory.cpp",
      '#include "canlogserver.h"\n',
      '#include "canlogserver.h"\n#include "triple_serial.h"\n')
patch("connections/canconfactory.cpp",
      "    case CANLOGSERVER:\n        return new CanLogServer(pPortName);\n",
      "    case CANLOGSERVER:\n        return new CanLogServer(pPortName);\n    case TRIPLE:\n        return new TripleSerial(pPortName);\n")

patch("connections/canconnectionmodel.cpp",
      '                        case CANCon::CANLOGSERVER: return "CanLogServer";\n',
      '                        case CANCon::CANLOGSERVER: return "CanLogServer";\n'
      '                        case CANCon::TRIPLE: return "USB2CAN Triple";\n')

patch("ui/newconnectiondialog.ui",
      '      <item row="7" column="0">',
      '      <item row="8" column="0">\n'
      '       <widget class="QRadioButton" name="rbTriple">\n'
      '        <property name="text">\n'
      '         <string>CANLAB USB2CAN Triple (serial)</string>\n'
      '        </property>\n'
      '       </widget>\n'
      '      </item>\n'
      '      <item row="7" column="0">')

patch("connections/newconnectiondialog.h",
      "    void selectCANlogserver();\n",
      "    void selectCANlogserver();\n    void selectTriple();\n")
patch("connections/newconnectiondialog.cpp",
      "    connect(ui->rbCanlogserver, &QAbstractButton::clicked, this, &NewConnectionDialog::handleConnTypeChanged);\n",
      "    connect(ui->rbCanlogserver, &QAbstractButton::clicked, this, &NewConnectionDialog::handleConnTypeChanged);\n"
      "    connect(ui->rbTriple, &QAbstractButton::clicked, this, &NewConnectionDialog::handleConnTypeChanged);\n")
patch("connections/newconnectiondialog.cpp",
      "    if (ui->rbCanlogserver->isChecked()) selectCANlogserver();\n}\n",
      "    if (ui->rbCanlogserver->isChecked()) selectCANlogserver();\n    if (ui->rbTriple->isChecked()) selectTriple();\n}\n")
patch("connections/newconnectiondialog.cpp",
      "void NewConnectionDialog::selectLawicel()\n",
      """void NewConnectionDialog::selectTriple()
{
    /* bitrates are set per bus in the connection window; the serial speed is irrelevant for CDC ACM */
    ui->lPort->setText("Serial Port:");
    ui->lblDeviceType->setHidden(true);
    ui->cbDeviceType->setHidden(true);
    ui->cbCANSpeed->setHidden(true);
    ui->cbSerialSpeed->setHidden(true);
    ui->lblCANSpeed->setHidden(true);
    ui->lblSerialSpeed->setHidden(true);
    ui->cbCanFd->setHidden(true);
    ui->cbDataRate->setHidden(true);
    ui->lblDataRate->setHidden(true);

    ui->cbPort->clear();
    ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < ports.count(); i++)
    {
        /* list the adapters first (USB 04d8:f0d7) */
        if (ports[i].vendorIdentifier() == 0x04d8 && ports[i].productIdentifier() == 0xf0d7)
            ui->cbPort->insertItem(0, ports[i].portName());
        else
            ui->cbPort->addItem(ports[i].portName());
    }
    ui->cbPort->setCurrentIndex(0);
}

void NewConnectionDialog::selectLawicel()
""")
patch("connections/newconnectiondialog.cpp",
      "        case CANCon::CANLOGSERVER:\n          ui->rbCanlogserver->setChecked(true);\n          break;\n",
      "        case CANCon::CANLOGSERVER:\n          ui->rbCanlogserver->setChecked(true);\n          break;\n"
      "        case CANCon::TRIPLE:\n          ui->rbTriple->setChecked(true);\n          break;\n")
patch("connections/newconnectiondialog.cpp",
      "        case CANCon::GVRET_SERIAL:\n        case CANCon::LAWICEL:\n",
      "        case CANCon::GVRET_SERIAL:\n        case CANCon::LAWICEL:\n        case CANCon::TRIPLE:\n")
patch("connections/newconnectiondialog.cpp",
      "    case CANCon::MQTT:\n    case CANCon::LAWICEL:\n",
      "    case CANCon::MQTT:\n    case CANCon::LAWICEL:\n    case CANCon::TRIPLE:\n")
patch("connections/newconnectiondialog.cpp",
      "    if (ui->rbCanlogserver->isChecked()) return CANCon::CANLOGSERVER;\n",
      "    if (ui->rbCanlogserver->isChecked()) return CANCon::CANLOGSERVER;\n"
      "    if (ui->rbTriple->isChecked()) return CANCon::TRIPLE;\n")

cw = open(os.path.join(S, "connections/connectionwindow.cpp"), encoding="utf-8").read()
if "isCanFDCapable()" in cw:
    print("connectionwindow.cpp: QT6WIP branch, FD controls follow CANBus::isCanFDCapable(), no edit needed")
else:
    patch("connections/connectionwindow.cpp",
          "        if (conn_p->getType() == CANCon::type::SERIALBUS || conn_p->getType() == CANCon::type::LAWICEL)\n",
          "        if (conn_p->getType() == CANCon::type::SERIALBUS || conn_p->getType() == CANCon::type::LAWICEL ||\n"
          "            (conn_p->getType() == CANCon::type::TRIPLE && offset == 2))\n")

# File > Save Log as PP2CAN... (after the Data Panel entry exists)
patch("mainwindow.cpp",
      '#include "re/datapanelwindow.h"\n',
      '#include "re/datapanelwindow.h"\n#include "pp2canlog.h"\n')
patch("mainwindow.cpp",
      "    connect(ui->menuSend_Frames->addAction(tr(\"Data Panel\")), &QAction::triggered, this, [this]() {\n",
      "    connect(ui->menuFile->addAction(tr(\"Save Log as PP2CAN...\")), &QAction::triggered, this, [this]() {\n"
      "        PP2CANLog::saveWithDialog(this, model->getListReference());\n"
      "    });\n"
      "    connect(ui->menuSend_Frames->addAction(tr(\"Data Panel\")), &QAction::triggered, this, [this]() {\n")

# File > Print Log..., placed with Save Log as PP2CAN before Preferences
patch("mainwindow.cpp",
      '#include "pp2canlog.h"\n',
      '#include "pp2canlog.h"\n#include "logprinter.h"\n')
patch("mainwindow.cpp",
      "    //Automatically create the connection window so it can be updated even if we never opened it.\n"
      "    connectionWindow = new ConnectionWindow();\n",
      "    //Automatically create the connection window so it can be updated even if we never opened it.\n"
      "    connectionWindow = new ConnectionWindow();\n"
      "    {\n"
      "        QAction *printLog = new QAction(tr(\"Print Log...\"), this);\n"
      "        connect(printLog, &QAction::triggered, this, [this]() {\n"
      "            LogPrinter::printView(this, ui->canFramesView, tr(\"SavvyCAN frame log\"));\n"
      "        });\n"
      "        QList<QAction *> fileActions = ui->menuFile->actions();\n"
      "        int pref = fileActions.indexOf(ui->actionPreferences);\n"
      "        QAction *before = pref > 0 && fileActions[pref - 1]->isSeparator() ? fileActions[pref - 1] : ui->actionPreferences;\n"
      "        for (QAction *a : fileActions)\n"
      "            if (a->text() == tr(\"Save Log as PP2CAN...\"))\n"
      "                ui->menuFile->insertAction(before, a);\n"
      "        ui->menuFile->insertAction(before, printLog);\n"
      "    }\n")

# RE Tools > Signal Finder and AI Assistant (after the Print Log block)
patch("mainwindow.cpp",
      '#include "logprinter.h"\n',
      '#include "logprinter.h"\n#include "re/signalfinderwindow.h"\n#include "re/aiassistantwindow.h"\n')
patch("mainwindow.cpp",
      "        ui->menuFile->insertAction(before, printLog);\n"
      "    }\n",
      "        ui->menuFile->insertAction(before, printLog);\n"
      "    }\n"
      "    connect(ui->menu_RE_Tools->addAction(tr(\"Signal Finder\")), &QAction::triggered, this, [this]() {\n"
      "        static QPointer<SignalFinderWindow> w;\n"
      "        if (!w) w = new SignalFinderWindow(model->getListReference(), this);\n"
      "        w->show();\n"
      "        w->raise();\n"
      "        w->activateWindow();\n"
      "    });\n"
      "    connect(ui->menu_RE_Tools->addAction(tr(\"AI Assistant\")), &QAction::triggered, this, [this]() {\n"
      "        static QPointer<AiAssistantWindow> w;\n"
      "        if (!w) w = new AiAssistantWindow(model->getListReference(), this);\n"
      "        w->show();\n"
      "        w->raise();\n"
      "        w->activateWindow();\n"
      "    });\n")

print("done: rebuild SavvyCAN (qmake && make)")
