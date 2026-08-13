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
#include "LanguageServer.hpp"
#include "Core/EventLogger.hpp"
#include "Core/MessageLogger.hpp"
#include "Settings/SettingsManager.hpp"
#include "Util/Util.hpp"
#include "third_party/lsp-cpp/include/LSPClient.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

namespace Extensions
{
namespace
{
std::string localFileUri(const QString &path)
{
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString(QUrl::FullyEncoded).toStdString();
}
} // namespace

LanguageServer::LanguageServer(QString const &lang)
{
    LOG_INFO(INFO_OF(lang));
    this->language = lang;
    if (shouldCreateClient())
    {
        createClient();
        performConnection();
    }
}

LanguageServer::~LanguageServer()
{
    if (lsp != nullptr)
    {
        LOG_INFO("Killing LSP");
        lsp->shutdown();
        lsp->exit();
        delete lsp;
    }
}

void LanguageServer::openDocument(QString const &path, Editor::CodeEditor *editor, MessageLogger *log)
{
    if (isDocumentOpen())
    {
        LOG_WARN("openDocument called without closing the previous document. Closing it now");
        closeDocument();
    }

    m_editor = editor;
    openFile = path;
    logger = log;
    ++attachmentGeneration;
    completionEditor = nullptr;
    latestCompletionRequestId.clear();
    disconnect(completionConnection);
    completionConnection = connect(editor, &Editor::CodeEditor::completionRequested, this,
                                   &LanguageServer::requestCompletion);
    editor->setCompletionEnabled(SettingsManager::get("LSP/Use Autocomplete " + language).toBool());

    if (lsp == nullptr)
        return;

    if (!isInitialized)
    {
        initializeLSP(path);
        isInitialized = true;
    }

    std::string uri = localFileUri(path);
    std::string code = m_editor->toPlainText().toStdString();
    std::string lang;

    if (language == "Java")
        lang = "java";
    else if (language == "Python")
        lang = "python";
    else
    {
        LOG_WARN_IF(language != "C++", "Unknown language " << language);
        lang = "cpp";
    }

    lsp->didOpen(uri, code, lang);
}

void LanguageServer::closeDocument()
{
    const bool hadAttachment = !m_editor.isNull() || !openFile.isEmpty();
    if (isDocumentOpen())
    {
        std::string uri = localFileUri(openFile);
        lsp->didClose(uri);
    }
    else if (hadAttachment)
    {
        LOG_WARN("Cannot notify the language server that the document closed; no live document is open");
    }

    disconnect(completionConnection);
    if (m_editor != nullptr)
    {
        m_editor->hideCompletionPopup();
        m_editor->setCompletionEnabled(false);
    }
    ++attachmentGeneration;
    latestCompletionRequestId.clear();
    completionEditor = nullptr;

    openFile = "";
    logger = nullptr;
    m_editor = nullptr;
}

void LanguageServer::requestLinting()
{
    if (m_editor == nullptr || !isDocumentOpen())
        return;

    std::vector<TextDocumentContentChangeEvent> changes;
    TextDocumentContentChangeEvent e;
    e.text = m_editor->toPlainText().toStdString();
    changes.push_back(e);

    std::string uri = localFileUri(openFile);
    lsp->didChange(uri, changes, true);
}

void LanguageServer::requestCompletion(int line, int character, int documentRevision, int cursorPosition, bool manual)
{
    if (m_editor == nullptr || !isDocumentOpen() || lsp == nullptr ||
        !SettingsManager::get("LSP/Use Autocomplete " + language).toBool())
    {
        return;
    }

    // Always synchronize the current text immediately before completion. The
    // linting timer may not have elapsed yet (or linting may be disabled).
    std::vector<TextDocumentContentChangeEvent> changes;
    TextDocumentContentChangeEvent change;
    change.text = m_editor->toPlainText().toStdString();
    changes.push_back(change);
    const std::string uri = localFileUri(openFile);
    lsp->didChange(uri, changes, false);

    Position position;
    position.line = line;
    position.character = character;
    CompletionContext context;
    context.triggerKind = CompletionTriggerKind::Invoked;

    latestCompletionRequestId = QString::fromStdString(lsp->completion(uri, position, context));
    completionEditor = m_editor;
    completionRevision = documentRevision;
    completionCursorPosition = cursorPosition;
    completionWasManual = manual;
    completionAttachmentGeneration = attachmentGeneration;
}

bool LanguageServer::isDocumentOpen() const
{
    return !openFile.isEmpty() && lsp != nullptr;
}

void LanguageServer::updateSettings()
{
    QPointer<Editor::CodeEditor> editorToReopen = m_editor;
    MessageLogger *loggerToReopen = logger;
    const QString pathToReopen = openFile;

    // Detach the old editor connection before replacing the client. This is
    // required even when the old client failed to start or both features were
    // just disabled.
    if (!m_editor.isNull() || !openFile.isEmpty())
        closeDocument();

    if (lsp != nullptr)
    {
        LOG_INFO("Killing LSP");
        lsp->shutdown();
        lsp->exit();
        delete lsp;
        lsp = nullptr;
    }
    isInitialized = false;

    if (!editorToReopen.isNull())
        editorToReopen->clearSquiggle();

    if (shouldCreateClient())
    {
        createClient();
        performConnection();
        LOG_INFO("Recreated Language server Process");
        if (!editorToReopen.isNull() && !pathToReopen.isEmpty())
        {
            openDocument(pathToReopen, editorToReopen, loggerToReopen);
            LOG_INFO("Reopened document after restart");
        }
    }
}

void LanguageServer::updatePath(QString const &newPath)
{
    if (lsp == nullptr || (openFile == newPath))
        return;
    MessageLogger *tmpLogger = logger;
    QPointer<Editor::CodeEditor> tmpEditor = m_editor;
    closeDocument();
    if (!tmpEditor.isNull())
        openDocument(newPath, tmpEditor, tmpLogger);
}

// Private methods
bool LanguageServer::shouldCreateClient()
{
    return SettingsManager::get("LSP/Use Linting " + language).toBool() ||
           SettingsManager::get("LSP/Use Autocomplete " + language).toBool();
}

void LanguageServer::createClient()
{
    delete lsp;
    auto program = SettingsManager::get("LSP/Path " + language).toString();
    auto args = QProcess::splitCommand(SettingsManager::get("LSP/Args " + language).toString().trimmed());
    lsp = new LSPClient(program, args);
}

void LanguageServer::performConnection()
{
    if (lsp == nullptr)
    {
        LOG_WARN("Skipping establishement of connections as lsp client is nullptr");
        return;
    }
    connect(lsp, &LSPClient::onError, this, &LanguageServer::onLSPServerErrorArrived);
    connect(lsp, &LSPClient::onRequest, this, &LanguageServer::onLSPServerRequestArrived);
    connect(lsp, &LSPClient::onServerError, this, &LanguageServer::onLSPServerProcessError);
    connect(lsp, &LSPClient::onResponse, this, &LanguageServer::onLSPServerResponseArrived);
    connect(lsp, &LSPClient::onNotify, this, &LanguageServer::onLSPServerNotificationArrived);
    connect(lsp, &LSPClient::onServerFinished, this, &LanguageServer::onLSPServerProcessFinished);
    connect(lsp, &LSPClient::newStderr, this, &LanguageServer::onLSPServerNewStderr);

    LOG_INFO("All language server connections have been established");
}

Editor::CodeEditor::SeverityLevel LanguageServer::lspSeverity(int in)
{
    switch (in)
    {
    case 1:
        return Editor::CodeEditor ::SeverityLevel::Error;
    case 2:
        return Editor::CodeEditor::SeverityLevel::Warning;
    case 3:
        return Editor::CodeEditor::SeverityLevel::Information;
    case 4:
        return Editor::CodeEditor::SeverityLevel::Hint;
    default:
        return Editor::CodeEditor::SeverityLevel::Error;
    }
    // Nothing matched
    return Editor::CodeEditor::SeverityLevel::Error;
}

void LanguageServer::initializeLSP(QString const &filePath)
{
    QFileInfo info(filePath);
    std::string uri = QUrl::fromLocalFile(info.absoluteDir().absolutePath()).toString(QUrl::FullyEncoded).toStdString();
    option<DocumentUri> rootUri(uri);
    lsp->initialize(rootUri);
}
// ---------------------------- LSP SLOTS ------------------------

void LanguageServer::onLSPServerNotificationArrived(QString const &method, QJsonObject const &param)
{
    if (method == "textDocument/publishDiagnostics" && !m_editor.isNull()) // Linting
    {
        const QString notificationUri = param.value("uri").toString();
        const QString activeUri = QString::fromStdString(localFileUri(openFile));
        if (notificationUri != activeUri)
            return;

        m_editor->clearSquiggle();
        QJsonArray doc = QJsonDocument::fromVariant(param.toVariantMap()).object()["diagnostics"].toArray();
        for (auto e : doc)
        {
            QString tooltip = e.toObject()["message"].toString();
            Editor::CodeEditor::SeverityLevel level = lspSeverity(e.toObject()["severity"].toInt());

            auto beg = e.toObject()["range"].toObject()["start"].toObject();
            auto end = e.toObject()["range"].toObject()["end"].toObject();

            QPair<int, int> start;
            QPair<int, int> stop;

            start.first = beg["line"].toInt() + 1;
            start.second = beg["character"].toInt();

            stop.first = end["line"].toInt() + 1;
            stop.second = end["character"].toInt();

            m_editor->addSquiggle(
                level, start, stop,
                tooltip.remove(" (fix available)")); // We do not provide quick fix so remove this text.
        }
        m_editor->highlightAllSquiggle();
    }
}

void LanguageServer::onLSPServerResponseArrived(QString const &id, QJsonValue const &result)
{
    if (!id.startsWith("textDocument/completion:"))
    {
        LOG_INFO("Response from Server has arrived");
        return;
    }

    if (id != latestCompletionRequestId || completionEditor.isNull() || completionEditor != m_editor ||
        completionAttachmentGeneration != attachmentGeneration || !completionEditor->hasFocus() ||
        completionEditor->document()->revision() != completionRevision ||
        completionEditor->textCursor().position() != completionCursorPosition)
    {
        return;
    }

    QJsonArray items;
    if (result.isArray())
        items = result.toArray();
    else if (result.isObject())
        items = result.toObject().value("items").toArray();

    completionEditor->showCompletionItems(items, completionRevision, completionCursorPosition, completionWasManual);
}

void LanguageServer::onLSPServerRequestArrived(QString const &method, // NOLINT: It can be made static.
                                               QJsonObject const &param, QJsonValue const &id)
{
    LOG_INFO("Request from Sever has arrived. " << INFO_OF(method));
}

void LanguageServer::onLSPServerErrorArrived(QString const &id, QJsonObject const &error)
{
    const int errorCode = error.value("code").toInt();
    if (id.startsWith("textDocument/completion:") &&
        (id != latestCompletionRequestId || errorCode == -32800 || errorCode == -32801))
    {
        // Superseded completion requests are routinely cancelled by language
        // servers while the user keeps typing. They are not user-facing errors.
        return;
    }

    QString ERR;
    ERR = QJsonDocument::fromVariant(error.toVariantMap()).toJson();

    LOG_ERR("ID is \n" << id);
    LOG_ERR("ERR is \n" << ERR);

    if (logger != nullptr)
        logger->error(tr("Language Server [%1]").arg(language),
                      tr("Language server sent an error. Please check log for details."));
}

void LanguageServer::onLSPServerProcessError(QProcess::ProcessError const &error)
{
    LOG_WARN_IF(error == QProcess::Crashed, "LSP Process errored out " << INFO_OF(error));
    LOG_ERR_IF(error != QProcess::Crashed, "LSP Process errored out " << INFO_OF(error));
    if (logger == nullptr)
        return;
    switch (error)
    {
    case QProcess::FailedToStart:
        logger->error(tr("Language Server [%1]").arg(language),
                      tr("Failed to start LSP Process. Have you set the path to the Language Server program at %1?")
                          .arg(SettingsManager::getPathText("LSP/Path " + language)),
                      false);
        break;
    case QProcess::Crashed:
        break;
    case QProcess::Timedout:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process timed out"));
        break;
    case QProcess::ReadError:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process Read Error"));
        break;
    case QProcess::WriteError:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process Write Error"));
        break;
    case QProcess::UnknownError:
        logger->error(tr("Language Server [%1]").arg(language), tr("An unknown error has occurred in LSP Process"));
        break;
    }
}

void LanguageServer::onLSPServerProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    LOG_INFO_IF(exitCode == 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
    LOG_WARN_IF(exitCode != 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
}

void LanguageServer::onLSPServerNewStderr(const QString &content) // NOLINT: It can be made static
{
    LOG_INFO(content);
}
} // namespace Extensions
