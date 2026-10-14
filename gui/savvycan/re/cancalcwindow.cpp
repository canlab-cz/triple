// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "cancalcwindow.h"
#include "canidcalc.h"

#include <QCheckBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

using namespace canidcalc;

namespace {
QString hx(quint32 v, int w) { return QString::number(v, 16).toUpper().rightJustified(w, '0'); }
bool parseHex(const QLineEdit *e, quint32 &v)
{
    bool ok;
    QString t = e->text().trimmed();
    if (t.startsWith("0x", Qt::CaseInsensitive))
        t = t.mid(2);
    v = t.toUInt(&ok, 16);
    return ok;
}
}

QLineEdit *CanCalcWindow::hexEdit(int width)
{
    QLineEdit *e = new QLineEdit(this);
    e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    e->setMaximumWidth(width);
    return e;
}

CanCalcWindow::CanCalcWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("CAN Calculator"));

    idEdit = hexEdit(140);
    decEdit = hexEdit(140);
    extended = new QCheckBox(tr("extended (29-bit)"), this);
    extended->setChecked(true);
    rtr = new QCheckBox(tr("RTR"), this);
    binLabel = new QLabel(this);
    binLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    binLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QGroupBox *idBox = new QGroupBox(tr("Identifier"), this);
    QFormLayout *idForm = new QFormLayout(idBox);
    QHBoxLayout *idRow = new QHBoxLayout;
    idRow->addWidget(idEdit);
    idRow->addWidget(extended);
    idRow->addWidget(rtr);
    idRow->addStretch(1);
    idForm->addRow(tr("Hex"), idRow);
    idForm->addRow(tr("Decimal"), decEdit);
    idForm->addRow(tr("Binary"), binLabel);

    baseEdit = hexEdit(80);
    extEdit = hexEdit(100);
    QGroupBox *partsBox = new QGroupBox(tr("11 + 18 bit (base and extension, hex)"), this);
    QHBoxLayout *partsRow = new QHBoxLayout(partsBox);
    partsRow->addWidget(new QLabel(tr("base ID[28:18]"), this));
    partsRow->addWidget(baseEdit);
    partsRow->addWidget(new QLabel(tr("extension ID[17:0]"), this));
    partsRow->addWidget(extEdit);
    partsRow->addStretch(1);

    const char *mcpNames[4] = { "SIDH", "SIDL", "EID8", "EID0" };
    QGroupBox *mcpBox = new QGroupBox(tr("MCP2510 / MCP2515 registers (hex)"), this);
    QHBoxLayout *mcpRow = new QHBoxLayout(mcpBox);
    for (int i = 0; i < 4; i++)
    {
        mcp[i] = hexEdit(50);
        mcpRow->addWidget(new QLabel(mcpNames[i], this));
        mcpRow->addWidget(mcp[i]);
    }
    mcpRow->addStretch(1);

    QGroupBox *sjaBox = new QGroupBox(tr("SJA1000 PeliCAN identifier registers (hex, ID3/ID4 extended only)"), this);
    QHBoxLayout *sjaRow = new QHBoxLayout(sjaBox);
    for (int i = 0; i < 4; i++)
    {
        sja[i] = hexEdit(50);
        sjaRow->addWidget(new QLabel(QString("ID%1").arg(i + 1), this));
        sjaRow->addWidget(sja[i]);
    }
    sjaRow->addStretch(1);

    status = new QLabel(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(idBox);
    layout->addWidget(partsBox);
    layout->addWidget(mcpBox);
    layout->addWidget(sjaBox);
    layout->addWidget(status);
    layout->addStretch(1);

    connect(idEdit, &QLineEdit::textEdited, this, [this]() { recompute(FROM_ID); });
    connect(decEdit, &QLineEdit::textEdited, this, [this]() {
        bool ok;
        quint32 v = decEdit->text().trimmed().toUInt(&ok, 10);
        if (ok) { busy = true; idEdit->setText(hx(v, extended->isChecked() ? 8 : 3)); busy = false; recompute(FROM_ID); }
    });
    connect(baseEdit, &QLineEdit::textEdited, this, [this]() { recompute(FROM_PARTS); });
    connect(extEdit, &QLineEdit::textEdited, this, [this]() { recompute(FROM_PARTS); });
    for (int i = 0; i < 4; i++)
    {
        connect(mcp[i], &QLineEdit::textEdited, this, [this]() { recompute(FROM_MCP); });
        connect(sja[i], &QLineEdit::textEdited, this, [this]() { recompute(FROM_SJA); });
    }
    connect(extended, &QCheckBox::toggled, this, [this]() { recompute(FROM_FLAGS); });
    connect(rtr, &QCheckBox::toggled, this, [this]() { recompute(FROM_FLAGS); });

    idEdit->setText("1ABCDEF0");
    recompute(FROM_ID);
    resize(620, 0);
}

void CanCalcWindow::recompute(Source src)
{
    if (busy)
        return;
    bool ext = extended->isChecked();
    quint32 id = 0;
    bool ok = true;

    switch (src)
    {
    case FROM_ID:
    case FROM_FLAGS:
        ok = parseHex(idEdit, id);
        break;
    case FROM_PARTS:
    {
        quint32 b = 0, e = 0;
        ok = parseHex(baseEdit, b) && parseHex(extEdit, e) && b <= 0x7FF && e <= 0x3FFFF;
        id = join(b, e);
        if (ok && !ext) { busy = true; extended->setChecked(true); busy = false; ext = true; }
        break;
    }
    case FROM_MCP:
    {
        quint32 v[4];
        for (int i = 0; i < 4 && ok; i++)
            ok = parseHex(mcp[i], v[i]) && v[i] <= 0xFF;
        if (ok)
        {
            Mcp m = { quint8(v[0]), quint8(v[1]), quint8(v[2]), quint8(v[3]) };
            bool e;
            id = fromMcp(m, e);
            busy = true; extended->setChecked(e); busy = false;
            ext = e;
        }
        break;
    }
    case FROM_SJA:
    {
        Sja s = {};
        s.count = ext ? 4 : 2;
        for (int i = 0; i < s.count && ok; i++)
        {
            quint32 v;
            ok = parseHex(sja[i], v) && v <= 0xFF;
            s.id[i] = quint8(v);
        }
        if (ok)
        {
            bool r;
            id = fromSja(s, ext, r);
            busy = true; rtr->setChecked(r); busy = false;
        }
        break;
    }
    }

    quint32 max = ext ? 0x1FFFFFFF : 0x7FF;
    if (!ok || id > max)
    {
        status->setText(ok ? tr("identifier too large for %1 frames (max 0x%2)").arg(ext ? "extended" : "standard", hx(max, 0))
                           : tr("enter hex numbers"));
        return;
    }
    status->setText(QString());
    busy = true;
    if (src != FROM_ID)
        idEdit->setText(hx(id, ext ? 8 : 3));
    showId(id);
    busy = false;
}

void CanCalcWindow::showId(quint32 id)
{
    bool ext = extended->isChecked();
    if (!decEdit->hasFocus())
        decEdit->setText(QString::number(id));
    QString bin = QString::number(id, 2).rightJustified(ext ? 29 : 11, '0');
    for (int i = bin.size() - 4; i > 0; i -= 4)
        bin.insert(i, ' ');
    binLabel->setText(bin);

    baseEdit->setEnabled(ext);
    extEdit->setEnabled(ext);
    if (!baseEdit->hasFocus()) baseEdit->setText(ext ? hx(baseOf(id), 3) : QString());
    if (!extEdit->hasFocus()) extEdit->setText(ext ? hx(extOf(id), 5) : QString());

    Mcp m = toMcp(id, ext);
    const quint8 mv[4] = { m.sidh, m.sidl, m.eid8, m.eid0 };
    for (int i = 0; i < 4; i++)
        if (!mcp[i]->hasFocus())
            mcp[i]->setText(hx(mv[i], 2));

    Sja s = toSja(id, ext, rtr->isChecked());
    for (int i = 0; i < 4; i++)
    {
        sja[i]->setEnabled(i < s.count);
        if (!sja[i]->hasFocus())
            sja[i]->setText(i < s.count ? hx(s.id[i], 2) : QString());
    }
}
