// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
#ifndef AIASSISTANTWINDOW_H
#define AIASSISTANTWINDOW_H

/*
 * RE Tools > AI Assistant: a chat with Claude (Anthropic Messages API, the
 * user's own API key) that reverse engineers the captured traffic with the
 * tools of re/aitools.h: overview, per-ID analysis, raw frames, decoded
 * signal values, time window comparison, DBC proposals, and - only when
 * the user allows it - transmitting frames, each one confirmed.
 *
 * Every use of the API is bounded by user settings: tokens per reply, tool
 * calls per question, data per tool result, session and daily token
 * budgets, requests per minute. Usage is counted from the API's own token
 * figures; the daily count survives restarts.
 */

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <vector>

#include "aitools.h"
#include "canre.h"
#include "savvycompat.h"

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

class AiAssistantWindow : public QWidget
{
    Q_OBJECT

public:
    AiAssistantWindow(const QVector<GuiFrame> *frames, QWidget *parent = nullptr);

private slots:
    void sendClicked();
    void stopClicked();
    void newConversation();
    void settingsClicked();
    void requestNext();
    void replyFinished();
    void saveDbc();

private:
    void loadSettings();
    void saveUsage();
    void appendTranscript(const QString &markdown);
    void finishQuestion(const QString &note = QString());
    QJsonObject runTool(const QString &id, const QString &name, const QJsonObject &input, bool &stopNow);
    QString proposeDbc(const QJsonObject &input, bool &error);
    QString sendFrame(const QJsonObject &input);
    void updateUsageLabel();
    QString apiKey() const;
    void closeDanglingToolUse(const QString &why);

    const QVector<GuiFrame> *frames;
    canre::Capture cap;
    aitools::Limits limits;
    aitools::Budget budget;
    QString model, endpoint, sessionKey;
    std::vector<canre::DbcMessage> proposals;

    QJsonArray history;           /* Messages API conversation */
    int toolCalls = 0;            /* in the current question */
    bool limitHit = false;
    bool busy = false;
    bool captureStale = false;    /* a frame was transmitted since the snapshot */
    QNetworkAccessManager *nam;
    QPointer<QNetworkReply> reply;
    QTimer retryTimer;
    QElapsedTimer clock;
    QString transcriptMd;

    QComboBox      *modelCombo;
    QLabel         *usageLabel;
    QLabel         *statusLabel;
    QTextBrowser   *transcript;
    QPlainTextEdit *input;
    QPushButton    *sendBtn;
    QPushButton    *stopBtn;
    QPlainTextEdit *dbcView;
};

#endif // AIASSISTANTWINDOW_H
