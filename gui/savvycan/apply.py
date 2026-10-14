#!/usr/bin/env python3
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
print("copied connection sources into", os.path.join(S, "connections"))

BS = " \\\n"  # backslash-newline continuation in the .pro file

patch("SavvyCAN.pro",
      "    connections/lawicel_serial.cpp" + BS,
      "    connections/lawicel_serial.cpp" + BS + "    connections/triple_serial.cpp" + BS + "    connections/triple_proto.c" + BS)
patch("SavvyCAN.pro",
      "    connections/lawicel_serial.h" + BS,
      "    connections/lawicel_serial.h" + BS + "    connections/triple_serial.h" + BS + "    connections/triple_proto.h" + BS)

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

patch("connections/connectionwindow.cpp",
      "        if (conn_p->getType() == CANCon::type::SERIALBUS || conn_p->getType() == CANCon::type::LAWICEL)\n",
      "        if (conn_p->getType() == CANCon::type::SERIALBUS || conn_p->getType() == CANCon::type::LAWICEL ||\n"
      "            (conn_p->getType() == CANCon::type::TRIPLE && offset == 2))\n")

print("done: rebuild SavvyCAN (qmake && make)")
