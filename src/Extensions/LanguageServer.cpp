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

Editor::SemanticHighlightKind semanticHighlightKind(const QString &tokenType)
{
    using Kind = Editor::SemanticHighlightKind;

    if (tokenType == "namespace")
        return Kind::Namespace;
    if (tokenType == "type" || tokenType == "concept")
        return Kind::Type;
    if (tokenType == "class")
        return Kind::Class;
    if (tokenType == "enum")
        return Kind::Enum;
    if (tokenType == "interface")
        return Kind::Interface;
    if (tokenType == "struct")
        return Kind::Struct;
    if (tokenType == "typeParameter")
        return Kind::TypeParameter;
    if (tokenType == "parameter")
        return Kind::Parameter;
    if (tokenType == "variable")
        return Kind::Variable;
    if (tokenType == "property")
        return Kind::Property;
    if (tokenType == "enumMember")
        return Kind::EnumMember;
    if (tokenType == "event")
        return Kind::Event;
    if (tokenType == "function")
        return Kind::Function;
    if (tokenType == "method")
        return Kind::Method;
    if (tokenType == "macro")
        return Kind::Macro;
    if (tokenType == "label")
        return Kind::Label;
    if (tokenType == "keyword" || tokenType == "modifier")
        return Kind::Keyword;
    if (tokenType == "number")
        return Kind::Number;
    if (tokenType == "operator")
        return Kind::Operator;
    if (tokenType == "bracket")
        return Kind::Bracket;
    if (tokenType == "comment")
        return Kind::Comment;
    return Kind::Unknown;
}
} // namespace

LanguageServer::LanguageServer(QString const &lang)
{
    LOG_INFO(INFO_OF(lang));
    this->language = lang;
    semanticTokensTimer.setInterval(150);
    semanticTokensTimer.setSingleShot(true);
    connect(&semanticTokensTimer, &QTimer::timeout, this, &LanguageServer::requestSemanticTokens);
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
    semanticTokensTimer.stop();
    latestSemanticTokensRequestId.clear();
    semanticTokensEditor = nullptr;
    semanticTokensRevision = -1;
    semanticTokensUri.clear();
    lastSyncedRevision = -1;
    lastDiagnosticsRevision = -1;
    disconnect(completionConnection);
    disconnect(semanticChangeConnection);
    completionConnection =
        connect(editor, &Editor::CodeEditor::completionRequested, this, &LanguageServer::requestCompletion);
    editor->setCompletionEnabled(SettingsManager::get("LSP/Use Autocomplete " + language).toBool());
    editor->clearSemanticHighlights();

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
    lastSyncedRevision = m_editor->document()->revision();

    if (language == "C++")
    {
        // QSyntaxHighlighter formatting also emits contentsChanged(). Listen
        // to the granular signal and ignore format-only updates, otherwise a
        // semantic response would rehighlight the document and schedule the
        // next semantic request forever.
        semanticChangeConnection = connect(editor->document(), &QTextDocument::contentsChange, this,
                                           [this](int, int charsRemoved, int charsAdded) {
                                               if (charsRemoved > 0 || charsAdded > 0)
                                                   scheduleSemanticTokens();
                                           });
        scheduleSemanticTokens();
    }
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

    semanticTokensTimer.stop();
    disconnect(completionConnection);
    disconnect(semanticChangeConnection);
    if (m_editor != nullptr)
    {
        m_editor->hideCompletionPopup();
        m_editor->setCompletionEnabled(false);
        m_editor->clearSemanticHighlights();
    }
    ++attachmentGeneration;
    latestCompletionRequestId.clear();
    completionEditor = nullptr;
    latestSemanticTokensRequestId.clear();
    semanticTokensEditor = nullptr;
    semanticTokensRevision = -1;
    semanticTokensUri.clear();
    lastSyncedRevision = -1;
    lastDiagnosticsRevision = -1;

    openFile = "";
    logger = nullptr;
    m_editor = nullptr;
}

void LanguageServer::requestLinting()
{
    if (m_editor == nullptr || !isDocumentOpen())
        return;

    syncDocument(true);
}

void LanguageServer::requestCompletion(int line, int character, int documentRevision, int cursorPosition, bool manual)
{
    if (m_editor == nullptr || !isDocumentOpen() || lsp == nullptr ||
        !SettingsManager::get("LSP/Use Autocomplete " + language).toBool())
    {
        return;
    }

    const std::string uri = localFileUri(openFile);
    // Completion has the shortest debounce. It synchronizes immediately; the
    // later semantic-token request reuses this same document revision.
    syncDocument(false);

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
    initializationRequestId.clear();
    semanticTokenTypes.clear();
    latestSemanticTokensRequestId.clear();
    auto program = SettingsManager::get("LSP/Path " + language).toString();
    auto args = QProcess::splitCommand(SettingsManager::get("LSP/Args " + language).toString().trimmed());
    lsp = new LSPClient(program, args);
}

void LanguageServer::scheduleSemanticTokens()
{
    if (language != "C++" || lsp == nullptr || m_editor.isNull() || !isDocumentOpen())
        return;

    semanticTokensTimer.start();
}

void LanguageServer::requestSemanticTokens()
{
    if (language != "C++" || lsp == nullptr || m_editor.isNull() || !isDocumentOpen() || semanticTokenTypes.isEmpty())
        return;

    const int revision = m_editor->document()->revision();
    const QString uriString = QString::fromStdString(localFileUri(openFile));
    const std::string uri = uriString.toStdString();

    // Avoid sending a second full document when completion already synchronized
    // this revision a few milliseconds earlier.
    syncDocument(false);

    latestSemanticTokensRequestId = QString::fromStdString(lsp->semanticTokens(uri));
    semanticTokensEditor = m_editor;
    semanticTokensRevision = revision;
    semanticTokensUri = uriString;
    semanticTokensAttachmentGeneration = attachmentGeneration;
}

void LanguageServer::syncDocument(bool wantDiagnostics)
{
    if (lsp == nullptr || m_editor.isNull() || !isDocumentOpen())
        return;

    const int revision = m_editor->document()->revision();
    if (revision == lastSyncedRevision && (!wantDiagnostics || revision == lastDiagnosticsRevision))
        return;

    std::vector<TextDocumentContentChangeEvent> changes;
    TextDocumentContentChangeEvent change;
    change.text = m_editor->toPlainText().toStdString();
    changes.push_back(std::move(change));
    lsp->didChange(localFileUri(openFile), changes, wantDiagnostics);
    lastSyncedRevision = revision;
    if (wantDiagnostics)
        lastDiagnosticsRevision = revision;
}

void LanguageServer::handleInitializeResponse(const QJsonValue &result)
{
    semanticTokenTypes.clear();

    const QJsonObject capabilities = result.toObject().value("capabilities").toObject();
    const QJsonObject provider = capabilities.value("semanticTokensProvider").toObject();
    const QJsonArray tokenTypes = provider.value("legend").toObject().value("tokenTypes").toArray();
    for (const QJsonValue &tokenType : tokenTypes)
    {
        if (tokenType.isString())
            semanticTokenTypes.push_back(tokenType.toString());
    }

    // Complete the standard LSP initialization handshake. clangd tolerated
    // older CP Editor versions omitting this notification, but semantic-token
    // requests should only begin after initialization is complete.
    if (lsp != nullptr)
        lsp->initialized();

    if (!semanticTokenTypes.isEmpty() && !m_editor.isNull() && isDocumentOpen())
        semanticTokensTimer.start();
}

void LanguageServer::handleSemanticTokensResponse(const QJsonValue &result)
{
    if (semanticTokensEditor.isNull() || semanticTokensEditor != m_editor ||
        semanticTokensAttachmentGeneration != attachmentGeneration || semanticTokensUri.isEmpty() ||
        semanticTokensUri != QString::fromStdString(localFileUri(openFile)) ||
        semanticTokensEditor->document()->revision() != semanticTokensRevision)
    {
        return;
    }

    const QJsonArray data = result.toObject().value("data").toArray();
    if (data.size() % 5 != 0)
        return;
    QVector<Editor::SemanticHighlight> highlights;
    highlights.reserve(data.size() / 5);
    int line = 0;
    int character = 0;

    for (int i = 0; i + 4 < data.size(); i += 5)
    {
        const int deltaLine = data.at(i).toInt(-1);
        const int deltaStart = data.at(i + 1).toInt(-1);
        if (deltaLine < 0 || deltaStart < 0)
            return;

        if (deltaLine == 0)
            character += deltaStart;
        else
        {
            line += deltaLine;
            character = deltaStart;
        }

        const int length = data.at(i + 2).toInt(-1);
        const int tokenTypeIndex = data.at(i + 3).toInt(-1);
        const int tokenModifiers = data.at(i + 4).toInt();
        if (length <= 0 || tokenTypeIndex < 0 || tokenTypeIndex >= semanticTokenTypes.size())
            continue;

        const Editor::SemanticHighlightKind kind = semanticHighlightKind(semanticTokenTypes.at(tokenTypeIndex));
        if (kind == Editor::SemanticHighlightKind::Unknown)
            continue;

        Editor::SemanticHighlight highlight;
        highlight.line = line;
        highlight.start = character;
        highlight.length = length;
        highlight.kind = kind;
        highlight.modifiers = static_cast<quint32>(tokenModifiers);
        highlights.push_back(highlight);
    }

    semanticTokensEditor->setSemanticHighlights(highlights, semanticTokensRevision);
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
    initializationRequestId = QString::fromStdString(lsp->initialize(rootUri));
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
    if (!initializationRequestId.isEmpty() && id == initializationRequestId)
    {
        handleInitializeResponse(result);
        return;
    }

    if (id.startsWith("textDocument/semanticTokens/full:"))
    {
        if (id == latestSemanticTokensRequestId)
            handleSemanticTokensResponse(result);
        return;
    }

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
    if (id.startsWith("textDocument/semanticTokens/full:") &&
        (id != latestSemanticTokensRequestId || errorCode == -32800 || errorCode == -32801))
    {
        return;
    }
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
    semanticTokensTimer.stop();
    latestSemanticTokensRequestId.clear();
    if (!m_editor.isNull())
        m_editor->clearSemanticHighlights();
    LOG_INFO_IF(exitCode == 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
    LOG_WARN_IF(exitCode != 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
}

void LanguageServer::onLSPServerNewStderr(const QString &content) // NOLINT: It can be made static
{
    LOG_INFO(content);
}
} // namespace Extensions
