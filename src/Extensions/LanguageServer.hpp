/*
 * Copyright (C) 2019-2021 Ashar Khan <ashar786khan@gmail.com>
 *
 * This file is part of CP Editor.
 *
 * CP Editor is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * I will not be responsible if CP Editor behaves in unexpected way and
 * causes your ratings to go down and or lose any important contest.
 *
 * Believe Software is "Software" and it isn't immune to bugs.
 *
 */

#ifndef LANGUAGE_SERVER_H
#define LANGUAGE_SERVER_H

#include "Editor/CodeEditor.hpp"
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

class MessageLogger;
class LSPClient;

namespace Extensions
{
class LanguageServer : public QObject
{
    Q_OBJECT

  public:
    explicit LanguageServer(QString const &lang);
    ~LanguageServer() override;

    void openDocument(QString const &path, Editor::CodeEditor *editor, MessageLogger *log);
    void closeDocument();
    void requestLinting();
    void requestCompletion(int line, int character, int documentRevision, int cursorPosition, bool manual);

    bool isDocumentOpen() const;

    void updateSettings();
    void updatePath(QString const &);

  private slots:
    void onLSPServerNotificationArrived(QString const &method, QJsonObject const &param);
    void onLSPServerResponseArrived(QString const &id, QJsonValue const &result);
    void onLSPServerRequestArrived(QString const &method, QJsonObject const &param, QJsonValue const &id);
    void onLSPServerErrorArrived(QString const &id, QJsonObject const &error);
    void onLSPServerProcessError(QProcess::ProcessError const &error);
    void onLSPServerProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onLSPServerNewStderr(const QString &content);

  private:
    void performConnection();
    void createClient();
    bool shouldCreateClient();
    void scheduleSemanticTokens();
    void requestSemanticTokens();
    void recoverSemanticDocument();
    void syncDocument(bool wantDiagnostics);
    void handleInitializeResponse(const QJsonValue &result);
    void handleSemanticTokensResponse(const QJsonValue &result);

    static Editor::CodeEditor::SeverityLevel lspSeverity(int in);
    void initializeLSP(QString const &filePath);

    QPointer<Editor::CodeEditor> m_editor;
    MessageLogger *logger = nullptr;
    LSPClient *lsp = nullptr;
    bool isInitialized = false;
    QString language;
    QString openFile;
    int lastSyncedRevision = -1;
    int lastDiagnosticsRevision = -1;
    QString initializationRequestId;
    QString latestCompletionRequestId;
    QPointer<Editor::CodeEditor> completionEditor;
    int completionRevision = -1;
    int completionCursorPosition = -1;
    bool completionWasManual = false;
    quint64 attachmentGeneration = 0;
    quint64 completionAttachmentGeneration = 0;
    QMetaObject::Connection completionConnection;

    QTimer semanticTokensTimer;
    QStringList semanticTokenTypes;
    QString latestSemanticTokensRequestId;
    QString semanticTokensUri;
    QPointer<Editor::CodeEditor> semanticTokensEditor;
    quint64 semanticTokensGeneration = 0;
    quint64 semanticTokensAttachmentGeneration = 0;
    bool semanticInvalidAstRecoveryAttempted = false;
    QMetaObject::Connection semanticChangeConnection;
};
} // namespace Extensions

#endif // !LANGUAGE_SERVER_H
