// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#include "aiassistantwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <functional>

#include "connections/canconmanager.h"
#include "dbcload.h"
#include "recapture.h"

namespace {
const char *MODELS[] = { "claude-fable-5-1", "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5" };
const char *DEFAULT_ENDPOINT = "https://api.anthropic.com";
const char *API_VERSION = "2023-06-01";

QString today() { return QDate::currentDate().toString(Qt::ISODate); }

QString kTokens(long v)
{
    return v >= 10000 ? QString::number(v / 1000.0, 'f', 0) + "k" : v >= 1000 ? QString::number(v / 1000.0, 'f', 1) + "k" : QString::number(v);
}

/* Ctrl+Enter sends */
class InputFilter : public QObject
{
public:
    InputFilter(QObject *parent, std::function<void()> send) : QObject(parent), send(std::move(send)) {}
protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::KeyPress)
        {
            QKeyEvent *k = static_cast<QKeyEvent *>(e);
            if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier))
            {
                send();
                return true;
            }
        }
        return QObject::eventFilter(o, e);
    }
private:
    std::function<void()> send;
};
}

AiAssistantWindow::AiAssistantWindow(const QVector<GuiFrame> *frameList, QWidget *parent)
    : QWidget(parent, Qt::Window), frames(frameList)
{
    setWindowTitle(tr("AI Assistant"));
    resize(1200, 860);
    clock.start();

    modelCombo = new QComboBox(this);
    modelCombo->setEditable(true);
    for (const char *m : MODELS) modelCombo->addItem(m);
    modelCombo->setMinimumWidth(200);
    QPushButton *settingsBtn = new QPushButton(tr("API key and limits..."), this);
    QPushButton *newBtn = new QPushButton(tr("New conversation"), this);
    usageLabel = new QLabel(this);
    usageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QHBoxLayout *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Model"), this));
    top->addWidget(modelCombo);
    top->addWidget(settingsBtn);
    top->addWidget(newBtn);
    top->addWidget(usageLabel, 1);

    transcript = new QTextBrowser(this);
    transcript->setOpenExternalLinks(true);
    transcript->setPlaceholderText(tr("Ask about the captured traffic, for example: \"Which IDs look like wheel speeds?\" or "
                                      "\"I pressed the brake pedal at about 12 s and released it at 15 s; find the brake signal.\""));

    dbcView = new QPlainTextEdit(this);
    dbcView->setReadOnly(true);
    dbcView->setPlaceholderText(tr("DBC proposals from the assistant appear here. Nothing is applied until you save them."));
    QPushButton *saveDbcBtn = new QPushButton(tr("Save DBC..."), this);
    QWidget *dbcBox = new QWidget(this);
    QVBoxLayout *dl = new QVBoxLayout(dbcBox);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->addWidget(new QLabel(tr("Proposed DBC"), this));
    dl->addWidget(dbcView);
    dl->addWidget(saveDbcBtn);

    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(transcript);
    split->addWidget(dbcBox);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    input = new QPlainTextEdit(this);
    input->setPlaceholderText(tr("Your question (Ctrl+Enter sends)"));
    input->setMaximumHeight(110);
    sendBtn = new QPushButton(tr("Send"), this);
    stopBtn = new QPushButton(tr("Stop"), this);
    stopBtn->setEnabled(false);
    QVBoxLayout *btns = new QVBoxLayout;
    btns->addWidget(sendBtn);
    btns->addWidget(stopBtn);
    QHBoxLayout *bottom = new QHBoxLayout;
    bottom->addWidget(input, 1);
    bottom->addLayout(btns);
    statusLabel = new QLabel(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(split, 1);
    layout->addLayout(bottom);
    layout->addWidget(statusLabel);

    nam = new QNetworkAccessManager(this);
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, &AiAssistantWindow::requestNext);
    connect(sendBtn, &QPushButton::clicked, this, &AiAssistantWindow::sendClicked);
    connect(stopBtn, &QPushButton::clicked, this, &AiAssistantWindow::stopClicked);
    connect(newBtn, &QPushButton::clicked, this, &AiAssistantWindow::newConversation);
    connect(settingsBtn, &QPushButton::clicked, this, &AiAssistantWindow::settingsClicked);
    connect(saveDbcBtn, &QPushButton::clicked, this, &AiAssistantWindow::saveDbc);
    connect(modelCombo, &QComboBox::currentTextChanged, this, [this](const QString &m) {
        model = m.trimmed();
        QSettings().setValue("AiAssistant/model", model);
    });
    input->installEventFilter(new InputFilter(this, [this]() { sendClicked(); }));

    loadSettings();
    updateUsageLabel();
}

/* ---------------------------------------------------------------- settings */

void AiAssistantWindow::loadSettings()
{
    QSettings s;
    s.beginGroup("AiAssistant");
    model = s.value("model", MODELS[0]).toString();
    endpoint = s.value("endpoint", DEFAULT_ENDPOINT).toString();
    limits.maxOutputTokens = s.value("maxOutputTokens", limits.maxOutputTokens).toInt();
    limits.maxToolCalls = s.value("maxToolCalls", limits.maxToolCalls).toInt();
    limits.maxFramesPerResult = s.value("maxFramesPerResult", limits.maxFramesPerResult).toInt();
    limits.maxResultChars = s.value("maxResultChars", limits.maxResultChars).toInt();
    limits.sessionTokenBudget = s.value("sessionTokenBudget", qlonglong(limits.sessionTokenBudget)).toLongLong();
    limits.dailyTokenBudget = s.value("dailyTokenBudget", qlonglong(limits.dailyTokenBudget)).toLongLong();
    limits.maxRequestsPerMinute = s.value("maxRequestsPerMinute", limits.maxRequestsPerMinute).toInt();
    limits.allowTransmit = s.value("allowTransmit", false).toBool();
    limits.transmitAllowlist.clear();
    for (const QString &t : s.value("transmitAllowlist").toString().split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts))
    {
        uint32_t id;
        if (aitools::parseId(t.toStdString(), id)) limits.transmitAllowlist.insert(id);
    }
    limits.priceInputPerMTok = s.value("priceInputPerMTok", 0.0).toDouble();
    limits.priceOutputPerMTok = s.value("priceOutputPerMTok", 0.0).toDouble();
    if (s.value("usageDay").toString() == today())
    {
        budget.day = today().toStdString();
        budget.today.input = s.value("usageInput").toLongLong();
        budget.today.output = s.value("usageOutput").toLongLong();
        budget.today.cacheWrite = s.value("usageCacheWrite").toLongLong();
        budget.today.cacheRead = s.value("usageCacheRead").toLongLong();
        budget.today.requests = s.value("usageRequests").toLongLong();
    }
    s.endGroup();
    int i = modelCombo->findText(model);
    if (i < 0) modelCombo->addItem(model), i = modelCombo->count() - 1;
    modelCombo->setCurrentIndex(i);
}

void AiAssistantWindow::saveUsage()
{
    QSettings s;
    s.beginGroup("AiAssistant");
    s.setValue("usageDay", QString::fromStdString(budget.day));
    s.setValue("usageInput", qlonglong(budget.today.input));
    s.setValue("usageOutput", qlonglong(budget.today.output));
    s.setValue("usageCacheWrite", qlonglong(budget.today.cacheWrite));
    s.setValue("usageCacheRead", qlonglong(budget.today.cacheRead));
    s.setValue("usageRequests", qlonglong(budget.today.requests));
    s.endGroup();
}

QString AiAssistantWindow::apiKey() const
{
    if (!sessionKey.isEmpty()) return sessionKey;
    QString k = QSettings().value("AiAssistant/apiKey").toString();
    if (!k.isEmpty()) return k;
    return qEnvironmentVariable("ANTHROPIC_API_KEY");
}

void AiAssistantWindow::settingsClicked()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("AI Assistant: API key and limits"));
    QSettings s;
    s.beginGroup("AiAssistant");

    QLineEdit *key = new QLineEdit(sessionKey.isEmpty() ? s.value("apiKey").toString() : sessionKey, &dlg);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(qEnvironmentVariableIsSet("ANTHROPIC_API_KEY") ? tr("using ANTHROPIC_API_KEY from the environment") : tr("sk-ant-..."));
    QCheckBox *showKey = new QCheckBox(tr("show"), &dlg);
    connect(showKey, &QCheckBox::toggled, key, [key](bool on) { key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    QCheckBox *remember = new QCheckBox(tr("Remember on this computer (stored unencrypted in SavvyCAN's settings)"), &dlg);
    remember->setChecked(!s.value("apiKey").toString().isEmpty());
    QHBoxLayout *keyRow = new QHBoxLayout;
    keyRow->addWidget(key, 1);
    keyRow->addWidget(showKey);
    QLineEdit *ep = new QLineEdit(endpoint, &dlg);

    auto ispin = [&dlg](int lo, int hi, int v, int step = 1) {
        QSpinBox *sp = new QSpinBox(&dlg);
        sp->setRange(lo, hi);
        sp->setSingleStep(step);
        sp->setValue(v);
        sp->setGroupSeparatorShown(true);
        return sp;
    };
    QSpinBox *maxOut = ispin(256, 64000, limits.maxOutputTokens, 256);
    QSpinBox *maxTools = ispin(1, 100, limits.maxToolCalls);
    QSpinBox *maxFrames = ispin(10, 5000, limits.maxFramesPerResult, 10);
    QSpinBox *maxChars = ispin(1000, 200000, limits.maxResultChars, 1000);
    QSpinBox *session = ispin(0, 1000000000, int(limits.sessionTokenBudget), 10000);
    QSpinBox *daily = ispin(0, 1000000000, int(limits.dailyTokenBudget), 100000);
    QSpinBox *rpm = ispin(0, 600, limits.maxRequestsPerMinute);
    session->setSpecialValueText(tr("no limit"));
    daily->setSpecialValueText(tr("no limit"));
    rpm->setSpecialValueText(tr("no limit"));
    QDoubleSpinBox *priceIn = new QDoubleSpinBox(&dlg);
    QDoubleSpinBox *priceOut = new QDoubleSpinBox(&dlg);
    for (QDoubleSpinBox *p : { priceIn, priceOut })
    {
        p->setRange(0, 1000);
        p->setDecimals(2);
        p->setPrefix("$ ");
        p->setSpecialValueText(tr("not shown"));
    }
    priceIn->setValue(limits.priceInputPerMTok);
    priceOut->setValue(limits.priceOutputPerMTok);
    QCheckBox *tx = new QCheckBox(tr("Allow the assistant to transmit frames (each frame still needs your confirmation)"), &dlg);
    tx->setChecked(limits.allowTransmit);
    QLineEdit *allow = new QLineEdit(s.value("transmitAllowlist").toString(), &dlg);
    allow->setPlaceholderText(tr("only these IDs (hex, comma separated); empty = any"));
    allow->setEnabled(tx->isChecked());
    connect(tx, &QCheckBox::toggled, allow, &QLineEdit::setEnabled);

    QFormLayout *api = new QFormLayout;
    api->addRow(tr("API key"), keyRow);
    api->addRow(QString(), remember);
    api->addRow(tr("Endpoint"), ep);
    QGroupBox *apiBox = new QGroupBox(tr("Anthropic API"), &dlg);
    apiBox->setLayout(api);

    QFormLayout *lim = new QFormLayout;
    lim->addRow(tr("Max tokens per reply"), maxOut);
    lim->addRow(tr("Max tool calls per question"), maxTools);
    lim->addRow(tr("Max frames / points per tool result"), maxFrames);
    lim->addRow(tr("Max characters per tool result"), maxChars);
    lim->addRow(tr("Session token budget"), session);
    lim->addRow(tr("Daily token budget"), daily);
    lim->addRow(tr("Max requests per minute"), rpm);
    lim->addRow(tr("Price per million input tokens"), priceIn);
    lim->addRow(tr("Price per million output tokens"), priceOut);
    QGroupBox *limBox = new QGroupBox(tr("Limits (checked before every request; token budgets count input, output and cache tokens)"), &dlg);
    limBox->setLayout(lim);

    QFormLayout *txl = new QFormLayout;
    txl->addRow(tx);
    txl->addRow(tr("Transmit allowlist"), allow);
    QGroupBox *txBox = new QGroupBox(tr("Bus access"), &dlg);
    txBox->setLayout(txl);

    QLabel *note = new QLabel(tr("Your questions and the tool results, which contain data from the capture, are sent to the "
                                 "endpoint above. Today: %1 tokens in %2 requests.")
                                  .arg(budget.today.total()).arg(budget.today.requests), &dlg);
    note->setWordWrap(true);
    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QVBoxLayout *v = new QVBoxLayout(&dlg);
    v->addWidget(apiBox);
    v->addWidget(limBox);
    v->addWidget(txBox);
    v->addWidget(note);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    QString k = key->text().trimmed();
    if (remember->isChecked() && !k.isEmpty()) { s.setValue("apiKey", k); sessionKey.clear(); }
    else { s.remove("apiKey"); sessionKey = k; }
    s.setValue("endpoint", ep->text().trimmed().isEmpty() ? QString(DEFAULT_ENDPOINT) : ep->text().trimmed());
    s.setValue("maxOutputTokens", maxOut->value());
    s.setValue("maxToolCalls", maxTools->value());
    s.setValue("maxFramesPerResult", maxFrames->value());
    s.setValue("maxResultChars", maxChars->value());
    s.setValue("sessionTokenBudget", session->value());
    s.setValue("dailyTokenBudget", daily->value());
    s.setValue("maxRequestsPerMinute", rpm->value());
    s.setValue("priceInputPerMTok", priceIn->value());
    s.setValue("priceOutputPerMTok", priceOut->value());
    s.setValue("allowTransmit", tx->isChecked());
    s.setValue("transmitAllowlist", allow->text().trimmed());
    s.endGroup();
    loadSettings();
    updateUsageLabel();
}

void AiAssistantWindow::updateUsageLabel()
{
    QString t = tr("Session %1 tokens (in %2, out %3, cache %4) | today %5")
                    .arg(kTokens(budget.session.total()), kTokens(budget.session.input), kTokens(budget.session.output),
                         kTokens(budget.session.cacheWrite + budget.session.cacheRead), kTokens(budget.today.total()));
    if (limits.dailyTokenBudget > 0) t += tr(" of %1").arg(kTokens(limits.dailyTokenBudget));
    if (limits.priceInputPerMTok > 0 || limits.priceOutputPerMTok > 0)
        t += tr(" | about $%1 session, $%2 today").arg(budget.cost(limits, budget.session), 0, 'f', 2).arg(budget.cost(limits, budget.today), 0, 'f', 2);
    usageLabel->setText(t);
}

/* -------------------------------------------------------------- transcript */

void AiAssistantWindow::appendTranscript(const QString &markdown)
{
    transcriptMd += markdown + "\n\n";
    transcript->setMarkdown(transcriptMd);
    transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
}

void AiAssistantWindow::newConversation()
{
    if (busy) stopClicked();
    history = QJsonArray();
    transcriptMd.clear();
    transcript->clear();
    statusLabel->clear();
}

/* ---------------------------------------------------------------- requests */

void AiAssistantWindow::sendClicked()
{
    QString q = input->toPlainText().trimmed();
    if (busy || q.isEmpty()) return;
    if (apiKey().isEmpty())
    {
        settingsClicked();
        if (apiKey().isEmpty()) return;
    }
    fillCapture(cap, *frames);                  /* the tools see the frames captured up to now */
    if (cap.size() == 0)
        appendTranscript(tr("*Note: the capture is empty; the assistant has no frames to work with.*"));
    input->clear();
    appendTranscript("**You:** " + q);
    QJsonObject msg{ { "role", "user" }, { "content", q } };
    history.append(msg);
    toolCalls = 0;
    limitHit = false;
    busy = true;
    sendBtn->setEnabled(false);
    stopBtn->setEnabled(true);
    requestNext();
}

void AiAssistantWindow::requestNext()
{
    if (!busy) return;
    QJsonArray tools = QJsonDocument::fromJson(QByteArray::fromStdString(aitools::toolDefinitions(limits))).array();
    if (!tools.isEmpty())
    {
        QJsonObject last = tools.last().toObject();
        last["cache_control"] = QJsonObject{ { "type", "ephemeral" } };     /* tools and system prompt are cached */
        tools.replace(tools.size() - 1, last);
    }
    QJsonArray system{ QJsonObject{ { "type", "text" }, { "text", QString::fromStdString(aitools::systemPrompt()) },
                                    { "cache_control", QJsonObject{ { "type", "ephemeral" } } } } };
    QJsonObject body{ { "model", model }, { "system", system }, { "tools", tools }, { "messages", history } };
    long estimate = aitools::estimateTokens(size_t(QJsonDocument(body).toJson(QJsonDocument::Compact).size()));

    long long waitMs = 0;
    std::string why = budget.check(limits, clock.elapsed(), today().toStdString(), estimate, waitMs);
    if (!why.empty())
    {
        if (waitMs > 0)
        {
            statusLabel->setText(tr("Waiting %1 s: %2").arg((waitMs + 999) / 1000).arg(QString::fromStdString(why)));
            retryTimer.start(int(waitMs) + 50);
            return;
        }
        closeDanglingToolUse("not sent: " + QString::fromStdString(why));
        finishQuestion(tr("Stopped by your limits: %1. Raise them under \"API key and limits\" to continue.").arg(QString::fromStdString(why)));
        return;
    }
    int maxTokens = budget.maxTokens(limits, estimate);
    if (maxTokens < 256)
    {
        closeDanglingToolUse("not sent: token budget nearly used up");
        finishQuestion(tr("Stopped by your limits: less than 256 tokens of budget left for a reply."));
        return;
    }
    body["max_tokens"] = maxTokens;

    QNetworkRequest req(QUrl(endpoint + "/v1/messages"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key", apiKey().toUtf8());
    req.setRawHeader("anthropic-version", API_VERSION);
    req.setTransferTimeout(300000);
    budget.requestSent(clock.elapsed());
    statusLabel->setText(tr("Asking %1 (about %2 input tokens, at most %3 output tokens)...").arg(model).arg(estimate).arg(maxTokens));
    reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, &AiAssistantWindow::replyFinished);
}

void AiAssistantWindow::replyFinished()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != reply || !busy) return;            /* stopped meanwhile */
    reply = nullptr;

    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray raw = r->readAll();
    QJsonObject resp = QJsonDocument::fromJson(raw).object();
    if (r->error() != QNetworkReply::NoError || status != 200)
    {
        QString msg = resp.value("error").toObject().value("message").toString();
        if (msg.isEmpty()) msg = r->errorString();
        QString hint;
        if (status == 401) hint = tr(" Check the API key.");
        else if (status == 429) hint = tr(" The API's own rate limit; try again later or lower the requests per minute.");
        else if (status == 529) hint = tr(" The API is overloaded; try again later.");
        closeDanglingToolUse("request failed");
        finishQuestion(tr("API error %1: %2.%3").arg(status).arg(msg, hint));
        return;
    }

    QJsonObject u = resp.value("usage").toObject();
    aitools::Usage usage;
    usage.input = u.value("input_tokens").toInt();
    usage.output = u.value("output_tokens").toInt();
    usage.cacheWrite = u.value("cache_creation_input_tokens").toInt();
    usage.cacheRead = u.value("cache_read_input_tokens").toInt();
    usage.requests = 1;
    budget.addUsage(today().toStdString(), usage);
    saveUsage();
    updateUsageLabel();

    QJsonArray content = resp.value("content").toArray();
    history.append(QJsonObject{ { "role", "assistant" }, { "content", content } });
    QJsonArray results;
    bool stopNow = false;
    for (const QJsonValue &v : content)                 /* in the order the API returned them */
    {
        QJsonObject b = v.toObject();
        if (b.value("type").toString() == "text" && !b.value("text").toString().trimmed().isEmpty())
            appendTranscript("**Claude:** " + b.value("text").toString().trimmed());
        else if (b.value("type").toString() == "tool_use")
            results.append(runTool(b.value("id").toString(), b.value("name").toString(), b.value("input").toObject(), stopNow));
    }

    QString stop = resp.value("stop_reason").toString();
    if (stop == "tool_use" && !results.isEmpty())
    {
        history.append(QJsonObject{ { "role", "user" }, { "content", results } });
        if (stopNow)
        {
            finishQuestion(tr("Stopped: the assistant kept calling tools after the limit of %1 tool calls per question.").arg(limits.maxToolCalls));
            return;
        }
        requestNext();
        return;
    }
    if (stop == "max_tokens")
        finishQuestion(tr("The reply was cut at the limit of %1 tokens per reply.").arg(limits.maxOutputTokens));
    else
        finishQuestion();
}

void AiAssistantWindow::finishQuestion(const QString &note)
{
    busy = false;
    retryTimer.stop();
    sendBtn->setEnabled(true);
    stopBtn->setEnabled(false);
    statusLabel->setText(note);
    if (!note.isEmpty())
        appendTranscript("*" + note + "*");
}

void AiAssistantWindow::stopClicked()
{
    if (!busy) return;
    retryTimer.stop();
    if (reply)
    {
        QNetworkReply *r = reply;
        reply = nullptr;
        r->abort();
    }
    closeDanglingToolUse("cancelled by the user");
    finishQuestion(tr("Stopped."));
}

/* An assistant turn with tool_use must be followed by its tool_result, or
 * the next request is rejected; close it if the loop ends early. */
void AiAssistantWindow::closeDanglingToolUse(const QString &why)
{
    if (history.isEmpty()) return;
    QJsonObject last = history.last().toObject();
    if (last.value("role").toString() != "assistant") return;
    QJsonArray results;
    for (const QJsonValue &v : last.value("content").toArray())
        if (v.toObject().value("type").toString() == "tool_use")
            results.append(QJsonObject{ { "type", "tool_result" }, { "tool_use_id", v.toObject().value("id").toString() },
                                        { "content", why }, { "is_error", true } });
    if (!results.isEmpty())
        history.append(QJsonObject{ { "role", "user" }, { "content", results } });
}

/* ------------------------------------------------------------------- tools */

QJsonObject AiAssistantWindow::runTool(const QString &id, const QString &name, const QJsonObject &in, bool &stopNow)
{
    QString text;
    bool error = false;
    toolCalls++;
    if (toolCalls > limits.maxToolCalls)
    {
        text = tr("Tool call limit reached (%1 per question, set by the user). Answer with what you have.").arg(limits.maxToolCalls);
        error = true;
        if (limitHit) stopNow = true;           /* second time over the limit */
        limitHit = true;
    }
    else if (name == "propose_dbc")
        text = proposeDbc(in, error);
    else if (name == "send_frame")
        text = sendFrame(in);
    else
    {
        if (captureStale)
        {
            fillCapture(cap, *frames);
            captureStale = false;
        }
        aitools::Args a;
        for (auto it = in.begin(); it != in.end(); ++it)
        {
            if (it.value().isString()) a.str[it.key().toStdString()] = it.value().toString().toStdString();
            else if (it.value().isDouble()) a.num[it.key().toStdString()] = it.value().toDouble();
            else if (it.value().isBool()) a.flag[it.key().toStdString()] = it.value().toBool();
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        aitools::Result r = aitools::run(name.toStdString(), a, cap, limits);
        QApplication::restoreOverrideCursor();
        text = QString::fromStdString(r.text);
        error = r.error;
    }
    QStringList args;
    for (auto it = in.begin(); it != in.end(); ++it)
        if (!it.value().isArray() && !it.value().isObject())
            args << it.key() + "=" + it.value().toVariant().toString();
    appendTranscript(QString("> tool `%1` %2 -> %3").arg(name, args.join(", "),
                                                          error ? tr("error: ") + text.left(160) : tr("%1 characters").arg(text.size())));
    QJsonObject res{ { "type", "tool_result" }, { "tool_use_id", id }, { "content", text } };
    if (error) res["is_error"] = true;
    return res;
}

QString AiAssistantWindow::proposeDbc(const QJsonObject &in, bool &error)
{
    int added = 0;
    QStringList problems;
    std::vector<canre::IdInfo> ids = canre::listIds(cap);
    for (const QJsonValue &mv : in.value("messages").toArray())
    {
        QJsonObject mo = mv.toObject();
        QString idText = mo.value("id").toString();
        uint32_t id;
        if (!aitools::parseId(idText.toStdString(), id))
        {
            problems << tr("message id '%1' is not a hex CAN ID").arg(idText);
            continue;
        }
        canre::FrameKey key;
        key.id = id;
        key.ext = id > 0x7FF;
        int capturedLen = 8;
        for (const canre::IdInfo &i : ids)
            if (i.key.id == id)
            {
                key = i.key;
                if (!i.lengths.empty()) capturedLen = *i.lengths.rbegin();   /* longest length seen */
                break;
            }
        std::vector<canre::DbcSignal> sigs;
        for (const QJsonValue &sv : mo.value("signals").toArray())
        {
            QJsonObject so = sv.toObject();
            canre::DbcSignal s;
            s.name = so.value("name").toString().toStdString();
            s.startBit = so.value("start_bit").toInt(-1);
            s.length = so.value("length").toInt(0);
            s.bigEndian = so.value("byte_order").toString() == "motorola";
            s.isSigned = so.value("signed").toBool();
            s.factor = so.value("factor").toDouble(1);
            s.offset = so.value("offset").toDouble(0);
            s.minV = so.value("min").toDouble(0);
            s.maxV = so.value("max").toDouble(0);
            s.unit = so.value("unit").toString().toStdString();
            s.comment = so.value("comment").toString().toStdString();
            if (s.name.empty() || s.length < 1 || s.length > 64 || s.startBit < 0 || s.startBit > 511)
            {
                problems << tr("signal '%1' of %2: needs a name, start_bit 0..511 and length 1..64").arg(QString::fromStdString(s.name), idText);
                continue;
            }
            sigs.push_back(s);
        }
        if (sigs.empty())
        {
            problems << tr("message %1 has no valid signals").arg(idText);
            continue;
        }
        canre::DbcMessage *m = nullptr;
        for (canre::DbcMessage &x : proposals) if (x.key == key) m = &x;
        if (!m)
        {
            canre::DbcMessage nm;
            nm.key = key;
            nm.length = capturedLen;
            proposals.push_back(nm);
            m = &proposals.back();
        }
        m->name = mo.value("name").toString("MSG_" + idText).toStdString();
        m->length = mo.value("length").toInt(m->length);
        for (const canre::DbcSignal &s : sigs)
        {
            auto same = std::find_if(m->signals_.begin(), m->signals_.end(), [&](const canre::DbcSignal &o) { return o.name == s.name; });
            if (same != m->signals_.end()) *same = s;
            else m->signals_.push_back(s);
            added++;
        }
    }
    if (!proposals.empty())
        dbcView->setPlainText(QString::fromStdString(canre::toDbc(proposals)));
    error = added == 0;
    QString text = tr("%1 signals shown to the user in the proposal panel; they decide whether to save it.").arg(added);
    if (!problems.isEmpty())
        text += tr(" Not accepted: ") + problems.join("; ") + ".";
    return text;
}

QString AiAssistantWindow::sendFrame(const QJsonObject &in)
{
    if (!limits.allowTransmit)
        return tr("refused: transmitting is disabled in the user's settings");
    uint32_t id;
    if (!aitools::parseId(in.value("id").toString().toStdString(), id))
        return tr("refused: invalid id");
    if (!limits.transmitAllowlist.empty() && !limits.transmitAllowlist.count(id))
        return tr("refused: ID %1 is not on the user's transmit allowlist").arg(id, 0, 16);
    QByteArray data = QByteArray::fromHex(in.value("data").toString().toLatin1());
    if (data.size() > 64)
        return tr("refused: more than 64 data bytes");
    int bus = in.value("bus").toInt();
    bool ext = in.value("extended").toBool(id > 0x7FF);
    QString frameText = QString("bus %1, ID %2%3, data %4").arg(bus).arg(id, ext ? 8 : 3, 16, QChar('0')).arg(ext ? " (extended)" : "")
                            .arg(QString(data.toHex(' ').toUpper()));
    QMessageBox::StandardButton b = QMessageBox::question(
        this, tr("AI Assistant wants to transmit"),
        tr("Transmit this frame on the bus?\n\n%1\n\nReason given: %2").arg(frameText, in.value("reason").toString()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (b != QMessageBox::Yes)
        return tr("the user refused to transmit %1").arg(frameText);
    CANConManager::getInstance()->sendFrame(guiMakeFrame(bus, id, ext, data));
    captureStale = true;                        /* the next data tool sees the reaction */
    return tr("sent %1; the capture ended at %2 s when it was sent, frames after that are the reaction")
        .arg(frameText).arg(cap.spanSeconds(), 0, 'f', 3);
}

void AiAssistantWindow::saveDbc()
{
    if (proposals.empty())
    {
        QMessageBox::information(this, tr("AI Assistant"), tr("No DBC proposal yet."));
        return;
    }
    QString status = saveAndLoadDbc(this, QByteArray::fromStdString(canre::toDbc(proposals)));
    if (!status.isEmpty()) statusLabel->setText(status);
}
