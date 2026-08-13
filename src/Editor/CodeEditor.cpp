/*
 * Copyright (C) 2019-2020 Ashar Khan <ashar786khan@gmail.com>
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

/* Some codes are from the examples of KDE/syntax-highlighting, which are licensed under the MIT License:

    Copyright (C) 2016 Volker Krause <vkrause@kde.org>

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "Editor/CodeEditor.hpp"
#include "Core/EventLogger.hpp"
#include "Editor/CodeEditorSideBar.hpp"
#include "Editor/HighLighter.hpp"
#include "Editor/KSHRepository.hpp"
#include "Editor/LanguageRepository.hpp"
#include "Settings/SettingsManager.hpp"
#include "generated/SettingsHelper.hpp"
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <QApplication>
#include <QAbstractItemView>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextStream>
#include <QToolTip>

namespace Editor
{
namespace
{
enum CompletionRole
{
    InsertTextRole = Qt::UserRole + 1,
    HasRangeRole,
    StartLineRole,
    StartCharacterRole,
    EndLineRole,
    EndCharacterRole
};

QString cleanCompletionLabel(QString label)
{
    label = label.trimmed();
    if (label.startsWith(QChar(0x2022)))
        label.remove(0, 1);
    return label.trimmed();
}

QString plainCompletionText(QString text, int insertTextFormat)
{
    if (insertTextFormat != 2)
        return text;

    // Be defensive if a server sends snippets despite the advertised plain
    // text capability. Preserve placeholder defaults and remove tab stops.
    const QRegularExpression placeholder(R"(\$\{\d+:([^{}]*)\})");
    while (placeholder.match(text).hasMatch())
        text.replace(placeholder, "\\1");
    text.remove(QRegularExpression(R"(\$\{\d+\})"));
    text.remove(QRegularExpression(R"(\$\d+)"));
    text.replace("\\$", "$");
    return text;
}

bool completionMatches(const QString &candidate, const QString &prefix)
{
    if (prefix.isEmpty())
        return true;
    if (candidate.startsWith(prefix, Qt::CaseInsensitive))
        return true;

    // Keep clangd's server order, but allow VS Code-style abbreviated input
    // such as "pb" -> "push_back" while cached results are filtered locally.
    int prefixIndex = 0;
    for (const QChar character : candidate)
    {
        if (character.toCaseFolded() == prefix.at(prefixIndex).toCaseFolded() && ++prefixIndex == prefix.size())
            return true;
    }
    return false;
}

class CompletionPopup final : public QListWidget
{
  public:
    explicit CompletionPopup(QWidget *parent = nullptr) : QListWidget(parent)
    {
        setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        setFocusPolicy(Qt::NoFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setTextElideMode(Qt::ElideRight);
        setUniformItemSizes(true);
        setMaximumHeight(320);
        setMinimumWidth(280);
    }
};
} // namespace

CodeEditor::CodeEditor(QWidget *widget) : QPlainTextEdit(widget)
{
    highlighter = new Highlighter(document());
    sideBar = new CodeEditorSidebar(this);
    languageRepo = new LanguageRepository(SettingsHelper::getDefaultLanguage(), this);
    completionPopup = new CompletionPopup(this);
    completionTimer = new QTimer(this);
    completionTimer->setSingleShot(true);
    completionTimer->setInterval(80);

    connect(document(), &QTextDocument::blockCountChanged, this, &CodeEditor::updateSidebarGeometry);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateSidebarArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightParentheses);
    connect(this, &QPlainTextEdit::selectionChanged, this, &CodeEditor::highlightOccurrences);
    connect(completionTimer, &QTimer::timeout, this, &CodeEditor::scheduleAutomaticCompletion);
    connect(completionPopup, &QListWidget::itemClicked, this, [this] { acceptCurrentCompletion(); });
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (applyingCompletion || !completionEnabled)
            return;
        if (!completionRefreshPending)
        {
            completionRefreshPending = true;
            QTimer::singleShot(0, this, [this] {
                completionRefreshPending = false;
                if (!applyingCompletion && completionEnabled)
                    refreshCompletionPopupFromCache();
            });
        }
        completionTimer->start();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!applyingCompletion && !completionRefreshPending && !refreshCompletionPopupFromCache())
            hideCompletionPopup();
    });
    connect(this, &QPlainTextEdit::selectionChanged, this, [this] {
        if (textCursor().hasSelection())
            clearCompletionSession();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { hideCompletionPopup(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { hideCompletionPopup(); });

    setCenterOnScroll(true);
    setMouseTracking(true);
}

void CodeEditor::setCompletionEnabled(bool enabled)
{
    completionEnabled = enabled;
    if (!enabled)
    {
        completionTimer->stop();
        clearCompletionSession();
    }
}

void CodeEditor::hideCompletionPopup()
{
    if (completionPopup != nullptr)
        completionPopup->hide();
}

void CodeEditor::setSemanticHighlights(const QVector<SemanticHighlight> &highlights, int documentRevision)
{
    if (highlighter != nullptr)
        highlighter->setSemanticHighlights(highlights, documentRevision);
}

void CodeEditor::clearSemanticHighlights()
{
    if (highlighter != nullptr)
        highlighter->clearSemanticHighlights();
}

void CodeEditor::clearCompletionSession()
{
    completionTimer->stop();
    hideCompletionPopup();
    completionItemsCache = QJsonArray();
    completionSessionLine = -1;
    completionSessionStart = -1;
    completionRenderedRevision = -1;
    completionRenderedCursorPosition = -1;
    completionRenderedPrefix.clear();
    completionSessionAllowsEmptyPrefix = false;
}

void CodeEditor::requestCompletion(bool manual)
{
    if (!completionEnabled || applyingCompletion || textCursor().hasSelection())
        return;

    const auto cursor = textCursor();
    emit completionRequested(cursor.blockNumber(), cursor.positionInBlock(), document()->revision(),
                             cursor.position(), manual);
}

void CodeEditor::scheduleAutomaticCompletion()
{
    if (!completionEnabled || applyingCompletion || !hasFocus() || textCursor().hasSelection())
        return;

    const auto cursor = textCursor();
    const QString left = cursor.block().text().left(cursor.positionInBlock());
    const bool memberTrigger = left.endsWith('.') || left.endsWith("->") || left.endsWith("::");
    const auto identifierMatch = QRegularExpression(R"([A-Za-z_][A-Za-z0-9_]*$)").match(left);
    const int prefixLength = identifierMatch.hasMatch() ? identifierMatch.capturedLength() : 0;

    if (memberTrigger || prefixLength >= 1)
        requestCompletion(false);
}

void CodeEditor::showCompletionItems(const QJsonArray &items, int documentRevision, int requestPosition, bool manual)
{
    if (!completionEnabled || !hasFocus() || textCursor().hasSelection() ||
        document()->revision() != documentRevision || textCursor().position() != requestPosition)
    {
        return;
    }

    if (items.isEmpty())
    {
        clearCompletionSession();
        return;
    }

    const auto cursor = textCursor();
    const QString left = cursor.block().text().left(cursor.positionInBlock());
    const auto identifierMatch = QRegularExpression(R"([A-Za-z_][A-Za-z0-9_]*$)").match(left);
    const QString prefix = identifierMatch.hasMatch() ? identifierMatch.captured() : QString();

    completionItemsCache = items;
    completionSessionLine = cursor.blockNumber();
    completionSessionStart = cursor.positionInBlock() - prefix.size();
    const QString beforePrefix = left.left(completionSessionStart);
    completionSessionAllowsEmptyPrefix = manual || beforePrefix.endsWith('.') || beforePrefix.endsWith("->") ||
                                         beforePrefix.endsWith("::");
    rebuildCompletionPopup(completionItemsCache, prefix);
}

bool CodeEditor::refreshCompletionPopupFromCache()
{
    if (!completionEnabled || completionItemsCache.isEmpty() || !hasFocus() || textCursor().hasSelection() ||
        completionSessionLine < 0 || completionSessionStart < 0)
    {
        return false;
    }

    const auto cursor = textCursor();
    if (cursor.blockNumber() != completionSessionLine || cursor.positionInBlock() < completionSessionStart)
    {
        clearCompletionSession();
        return false;
    }

    const QString prefix = cursor.block().text().mid(completionSessionStart,
                                                     cursor.positionInBlock() - completionSessionStart);
    static const QRegularExpression identifierPrefix(R"([A-Za-z0-9_]*)");
    const auto match = identifierPrefix.match(prefix);
    if (!match.hasMatch() || match.capturedLength() != prefix.size())
    {
        clearCompletionSession();
        return false;
    }

    if (prefix.isEmpty() && !completionSessionAllowsEmptyPrefix)
    {
        clearCompletionSession();
        return false;
    }

    const bool documentChanged = document()->revision() != completionRenderedRevision;
    const bool prefixChanged = prefix != completionRenderedPrefix;
    if (!documentChanged && cursor.position() != completionRenderedCursorPosition)
    {
        // A plain cursor movement is not part of the typing session. Discard the
        // cached edit range so completion in the middle of a word cannot leave
        // an old suffix behind (for example, "vectorc").
        clearCompletionSession();
        return false;
    }

    if (documentChanged || prefixChanged || !completionPopup->isVisible())
        rebuildCompletionPopup(completionItemsCache, prefix);
    return true;
}

void CodeEditor::rebuildCompletionPopup(const QJsonArray &items, const QString &prefix)
{
    completionPopup->clear();
    QSet<QString> seen;
    for (const QJsonValue &value : items)
    {
        if (!value.isObject())
            continue;

        const QJsonObject item = value.toObject();
        const QJsonObject textEdit = item.value("textEdit").toObject();
        QString insertText = textEdit.value("newText").toString();
        if (insertText.isEmpty())
            insertText = item.value("insertText").toString();
        if (insertText.isEmpty())
            insertText = cleanCompletionLabel(item.value("label").toString());
        insertText = plainCompletionText(insertText, item.value("insertTextFormat").toInt(1));
        if (insertText.isEmpty() || seen.contains(insertText))
            continue;
        seen.insert(insertText);

        QString displayName = item.value("filterText").toString();
        if (displayName.isEmpty())
            displayName = cleanCompletionLabel(item.value("label").toString());
        if (displayName.isEmpty())
            displayName = insertText;

        if (!completionMatches(displayName, prefix) && !completionMatches(insertText, prefix))
        {
            continue;
        }

        QString detail = item.value("detail").toString().simplified();
        QString displayText = displayName;
        if (!detail.isEmpty() && detail != displayName)
            displayText += "    " + detail;

        auto *row = new QListWidgetItem(displayText, completionPopup);
        row->setData(InsertTextRole, insertText);

        // Keep the start of this completion session stable while the user keeps
        // typing.  The end follows the current cursor so accepting a cached item
        // replaces "p", "pu", ... instead of appending to it.
        const auto cursor = textCursor();
        int replacementEnd = cursor.positionInBlock();
        const QString blockText = cursor.block().text();
        while (replacementEnd < blockText.size())
        {
            const QChar character = blockText.at(replacementEnd);
            if (!character.isLetterOrNumber() && character != '_')
                break;
            ++replacementEnd;
        }
        row->setData(HasRangeRole, true);
        row->setData(StartLineRole, completionSessionLine);
        row->setData(StartCharacterRole, completionSessionStart);
        row->setData(EndLineRole, cursor.blockNumber());
        row->setData(EndCharacterRole, replacementEnd);

        row->setToolTip(detail);

        if (completionPopup->count() >= 200)
            break;
    }

    if (completionPopup->count() == 0)
    {
        completionRenderedRevision = document()->revision();
        completionRenderedCursorPosition = textCursor().position();
        completionRenderedPrefix = prefix;
        completionPopup->hide();
        return;
    }

    completionRenderedRevision = document()->revision();
    completionRenderedCursorPosition = textCursor().position();
    completionRenderedPrefix = prefix;
    positionCompletionPopup();
}

void CodeEditor::positionCompletionPopup()
{
    if (completionPopup->count() == 0)
        return;

    completionPopup->setCurrentRow(0);
    completionPopup->setFont(font());
    const QColor background = getEditorColor(KSyntaxHighlighting::Theme::BackgroundColor);
    const QColor foreground = getTextColor(KSyntaxHighlighting::Theme::Normal);
    const QColor selection = getEditorColor(KSyntaxHighlighting::Theme::TextSelection);
    completionPopup->setStyleSheet(
        QString("QListWidget { background: %1; color: %2; border: 1px solid %3; padding: 2px; } "
                "QListWidget::item { padding: 3px 6px; } QListWidget::item:selected { background: %4; color: %2; }")
            .arg(background.name(), foreground.name(), foreground.darker(160).name(), selection.name()));

    int width = 320;
    const QFontMetrics metrics(completionPopup->font());
    for (int i = 0; i < completionPopup->count() && i < 50; ++i)
        width = qMax(width, metrics.horizontalAdvance(completionPopup->item(i)->text()) + 36);
    QPoint point = viewport()->mapToGlobal(cursorRect().bottomLeft());
    const QRect screen = QApplication::screenAt(point) != nullptr ? QApplication::screenAt(point)->availableGeometry()
                                                                  : QApplication::primaryScreen()->availableGeometry();
    const int popupWidth = qMin(qMin(width, 760), screen.width());
    const int rowHeight = qMax(completionPopup->sizeHintForRow(0), fontMetrics().height() + 6);
    const int popupHeight = qMin(qMin(completionPopup->count(), 12) * rowHeight + 6, screen.height());
    completionPopup->resize(popupWidth, popupHeight);

    if (point.y() + completionPopup->height() > screen.bottom() + 1)
        point.setY(viewport()->mapToGlobal(cursorRect().topLeft()).y() - completionPopup->height());
    point.setX(qBound(screen.left(), point.x(), screen.right() - completionPopup->width() + 1));
    point.setY(qBound(screen.top(), point.y(), screen.bottom() - completionPopup->height() + 1));
    completionPopup->move(point);
    completionPopup->show();
    completionPopup->raise();
}

void CodeEditor::acceptCurrentCompletion()
{
    auto *row = completionPopup->currentItem();
    if (row == nullptr)
        return;

    applyingCompletion = true;
    completionTimer->stop();
    completionPopup->hide();

    QTextCursor cursor(document());
    if (row->data(HasRangeRole).toBool())
    {
        const int startLine = row->data(StartLineRole).toInt();
        const int startCharacter = row->data(StartCharacterRole).toInt();
        const int endLine = row->data(EndLineRole).toInt();
        const int endCharacter = row->data(EndCharacterRole).toInt();
        const QTextBlock startBlock = document()->findBlockByNumber(startLine);
        const QTextBlock endBlock = document()->findBlockByNumber(endLine);
        if (startBlock.isValid() && endBlock.isValid())
        {
            cursor.setPosition(startBlock.position() + qMin(startCharacter, startBlock.length() - 1));
            cursor.setPosition(endBlock.position() + qMin(endCharacter, endBlock.length() - 1), QTextCursor::KeepAnchor);
        }
        else
        {
            cursor = textCursor();
        }
    }
    else
    {
        cursor = textCursor();
        const QString left = cursor.block().text().left(cursor.positionInBlock());
        const auto match = QRegularExpression(R"([A-Za-z_][A-Za-z0-9_]*$)").match(left);
        if (match.hasMatch())
            cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, match.capturedLength());
    }

    cursor.beginEditBlock();
    cursor.insertText(row->data(InsertTextRole).toString());
    cursor.endEditBlock();
    setTextCursor(cursor);
    applyingCompletion = false;
    clearCompletionSession();
}

void CodeEditor::applySettings(const QString &lang)
{
    LOG_INFO("Applying settings to a CodeEditor");

    language = lang;
    languageRepo->changeLanguage(lang);

    m_tabReplace = QString(SettingsHelper::getTabWidth(), ' ');
    setTabStopDistance(fontMetrics().horizontalAdvance(QString(SettingsHelper::getTabWidth() * 200, ' ')) / 200.0);

    setFont(SettingsHelper::getEditorFont());

    if (SettingsHelper::isWrapText())
        setWordWrapMode(QTextOption::WordWrap);
    else
        setWordWrapMode(QTextOption::NoWrap);

    setHighlightCurrentLine(isHighlightingCurrentLine());
    m_highlightingErrorLine = SettingsHelper::isHighlightErrorLine();

    setCenterOnScroll(SettingsHelper::isExtraBottomMargin());

    if (language.isEmpty())
        return;

    highlighter->setDefinition(
        KSyntaxHighlightingRepository::getSyntaxHighlightingRepository()->definitionForName(language));

    QString editorThemeName = SettingsHelper::getEditorTheme();
    if (editorThemeName == "Default" || !KSyntaxHighlightingRepository::themeNames().contains(editorThemeName))
    {
        bool isLight = getEditorColor(KSyntaxHighlighting::Theme::BackgroundColor).lightness() < 128;
        auto defaultTheme = KSyntaxHighlightingRepository::getSyntaxHighlightingRepository()->defaultTheme(
            isLight ? KSyntaxHighlighting::Repository::LightTheme : KSyntaxHighlighting::Repository::DarkTheme);
        setTheme(defaultTheme);
        SettingsHelper::setEditorTheme(defaultTheme.name());
    }
    else
    {
        setTheme(KSyntaxHighlightingRepository::getSyntaxHighlightingRepository()->theme(editorThemeName));
    }

    updateCursorWidth();

    if (vimCursor())
        return;

    parentheses.clear();

    auto list = SettingsManager::get(language + "/Parentheses").toList();

    for (auto const &var : list)
    {
        auto li = var.toList();
        if (li.length() != 5)
        {
            LOG_ERR(INFO_OF(li.length()));
            continue;
        }

        auto left = li[0].toChar();
        auto right = li[1].toChar();

        auto getFlag = [](Qt::CheckState state, bool def) {
            switch (state)
            {
            case Qt::Checked:
                return true;
            case Qt::PartiallyChecked:
                return def;
            case Qt::Unchecked:
                return false;
            default:
                Q_UNREACHABLE();
            }
        };

        bool autoComplete = getFlag(Qt::CheckState(li[2].toInt()), SettingsHelper::isAutoCompleteParentheses());
        bool autoRemove = getFlag(Qt::CheckState(li[3].toInt()), SettingsHelper::isAutoRemoveParentheses());
        bool tabJumpOut = getFlag(Qt::CheckState(li[4].toInt()), SettingsHelper::isTabJumpOutParentheses());

        parentheses.push_back({left, right, autoComplete, autoRemove, tabJumpOut});
    }
}

void CodeEditor::setTheme(const KSyntaxHighlighting::Theme &newTheme)
{
    theme = newTheme;

    if (theme.isValid())
    {

        setStyleSheet(QString("QPlainTextEdit { background-color: %1; selection-background-color: %2; color: %3; }")
                          .arg(getEditorColor(KSyntaxHighlighting::Theme::BackgroundColor).name(),
                               getEditorColor(KSyntaxHighlighting::Theme::TextSelection).name(),
                               getTextColor(KSyntaxHighlighting::Theme::Normal).name()));
    }

    highlighter->setTheme(theme);
    highlighter->rehighlight();
    highlightCurrentLine();
    highlightOccurrences();
    highlightParentheses();
    highlightAllSquiggle();
}

int CodeEditor::sidebarWidth() const
{
    int digits = 1;
    auto count = document()->blockCount();
    while (count >= 10)
    {
        ++digits;
        count /= 10;
    }
    return 4 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * (digits + 1) + fontMetrics().lineSpacing();
}

void CodeEditor::sidebarPaintEvent(QPaintEvent *event)
{
    QPainter painter(sideBar);
    painter.fillRect(event->rect(), getEditorColor(KSyntaxHighlighting::Theme::IconBorder));
    painter.setFont(font());

    auto block = firstVisibleBlock();
    auto blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());
    const int currentBlockNumber = textCursor().blockNumber();

    const auto foldingMarkerSize = fontMetrics().lineSpacing();

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            const auto number = QString::number(blockNumber + 1);
            painter.setPen(getEditorColor((blockNumber == currentBlockNumber)
                                              ? KSyntaxHighlighting::Theme::CurrentLineNumber
                                              : KSyntaxHighlighting::Theme::LineNumbers));
            painter.drawText(0, top, sideBar->width() - 2 - foldingMarkerSize, fontMetrics().height(), Qt::AlignRight,
                             number);
        }

        // folding marker
        if (block.isVisible() && isFoldable(block))
        {
            QPolygonF polygon;
            if (isFolded(block))
            {
                polygon << QPointF(foldingMarkerSize * 0.4, foldingMarkerSize * 0.25);
                polygon << QPointF(foldingMarkerSize * 0.4, foldingMarkerSize * 0.75);
                polygon << QPointF(foldingMarkerSize * 0.8, foldingMarkerSize * 0.5);
            }
            else
            {
                polygon << QPointF(foldingMarkerSize * 0.25, foldingMarkerSize * 0.4);
                polygon << QPointF(foldingMarkerSize * 0.75, foldingMarkerSize * 0.4);
                polygon << QPointF(foldingMarkerSize * 0.5, foldingMarkerSize * 0.8);
            }
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            //            painter.setBrush(getEditorColor(KSyntaxHighlighting::Theme::CodeFolding));
            painter.setBrush(getEditorColor(KSyntaxHighlighting::Theme::LineNumbers));
            painter.translate(sideBar->width() - foldingMarkerSize, top);
            painter.drawPolygon(polygon);
            painter.restore();
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CodeEditor::updateSidebarGeometry()
{
    setViewportMargins(sidebarWidth(), 0, 0, 0);
    const auto r = contentsRect();
    sideBar->setGeometry(QRect(r.left(), r.top(), sidebarWidth(), r.height()));
}

void CodeEditor::updateSidebarArea(const QRect &rect, int dy)
{
    if (dy)
        sideBar->scroll(0, dy);
    else
        sideBar->update(0, rect.y(), sideBar->width(), rect.height());
}

QTextBlock CodeEditor::blockAtPosition(int y) const
{
    auto block = firstVisibleBlock();
    if (!block.isValid())
        return QTextBlock();

    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());
    do
    {
        if (top <= y && y <= bottom)
            return block;
        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
    } while (block.isValid());
    return QTextBlock();
}

bool CodeEditor::isFoldable(const QTextBlock &block)
{
    return Editor::Highlighter::startsFoldingRegion(block);
}

bool CodeEditor::isFolded(const QTextBlock &block)
{
    if (!block.isValid())
        return false;
    const auto nextBlock = block.next();
    if (!nextBlock.isValid())
        return false;
    return !nextBlock.isVisible();
}

void CodeEditor::toggleFold(const QTextBlock &startBlock)
{
    // we also want to fold the last line of the region, therefore the ".next()"
    const auto endBlock = Editor::Highlighter::findFoldingRegionEnd(startBlock).next();

    if (isFolded(startBlock))
    {
        // unfold
        auto block = startBlock.next();
        while (block.isValid() && !block.isVisible())
        {
            block.setVisible(true);
            block.setLineCount(block.layout()->lineCount());
            block = block.next();
        }
    }
    else
    {
        // fold
        auto block = startBlock.next();
        while (block.isValid() && block != endBlock)
        {
            block.setVisible(false);
            block.setLineCount(0);
            block = block.next();
        }
    }

    // redraw document
    document()->markContentsDirty(startBlock.position(), endBlock.position() - startBlock.position() + 1);

    // update scrollbars
    emit document()->documentLayout()->documentSizeChanged(document()->documentLayout()->documentSize());
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    hideCompletionPopup();
    QPlainTextEdit::resizeEvent(e);
    updateSidebarGeometry();
}

void CodeEditor::changeEvent(QEvent *e)
{
    QPlainTextEdit::changeEvent(e);
    if (e->type() == QEvent::FontChange)
        updateSidebarGeometry();
}

void CodeEditor::paintEvent(QPaintEvent *e)
{
    if (m_vimCursor)
    {
        if (!m_cursorRect.isNull() && e->rect().intersects(m_cursorRect))
        {
            QRect rect = m_cursorRect;
            m_cursorRect = QRect();
            viewport()->update(rect);
        }

        // Draw text cursor.
        QRect rect = cursorRect();
        if (e->rect().intersects(rect))
        {
            QPainter painter(viewport());

            if (overwriteMode())
            {
                QFontMetrics fm(font());
                const int position = textCursor().position();
                const QChar c = document()->characterAt(position);
                rect.setWidth(fm.horizontalAdvance(c));
                painter.setPen(Qt::NoPen);

                painter.setBrush(getTextColor(KSyntaxHighlighting::Theme::Char));
                painter.setCompositionMode(QPainter::CompositionMode_Difference);
            }
            else
            {
                rect.setWidth(cursorWidth());
                painter.setPen(getTextColor(KSyntaxHighlighting::Theme::Char));
            }

            painter.drawRect(rect);
            m_cursorRect = rect;
        }
    }
    QPlainTextEdit::paintEvent(e);
}

void CodeEditor::focusOutEvent(QFocusEvent *e)
{
    // The completion list is a tooltip window and does not take focus. Hide it
    // when the editor genuinely loses focus (tab switch, settings dialog, etc.).
    clearCompletionSession();
    if (m_vimCursor)
    {
        setOverwriteMode(true); // makes a block cursor when focus is lost
    }
    QPlainTextEdit::focusOutEvent(e);
}

void CodeEditor::setHighlightCurrentLine(bool enabled)
{
    m_highlightingCurrentLine = enabled;
}

void CodeEditor::setVimCursor(bool value)
{
    m_vimCursor = value;

    setOverwriteMode(false);
    updateCursorWidth();
}

void CodeEditor::updateCursorWidth()
{
    if (m_vimCursor || overwriteMode())
    {
        // setting the width to 0 also works, but would flicker when swithed to the narrow cursor
        setCursorWidth(fontMetrics().maxWidth());
    }
    else
    {
        const auto oldRect = cursorRect();
        setCursorWidth(SettingsHelper::getCursorWidth());
        viewport()->update(oldRect); // prevent flickering
    }
}

bool CodeEditor::isHighlightingCurrentLine() const
{
    return m_highlightingCurrentLine;
}

bool CodeEditor::vimCursor() const
{
    return m_vimCursor;
}

void CodeEditor::wheelEvent(QWheelEvent *e)
{
    hideCompletionPopup();
    if (e->modifiers() == Qt::ControlModifier)
    {
        const auto sizes = QFontDatabase::standardSizes();
        if (sizes.isEmpty())
        {
            LOG_ERR("QFontDatabase::standardSizes() is empty");
            return;
        }
        int newSize = font().pointSize();
        if (e->angleDelta().y() > 0)
            newSize = qMin(newSize + 1, sizes.last());
        else if (e->angleDelta().y() < 0)
            newSize = qMax(newSize - 1, sizes.first());
        if (newSize != font().pointSize())
        {
            QFont newFont = font();
            newFont.setPointSize(newSize);
            setFont(newFont);
            emit fontChanged(newFont);
        }
    }
    else
        QPlainTextEdit::wheelEvent(e);
}

void CodeEditor::highlightOccurrences()
{
    occurrencesExtraSelections.clear();

    auto cursor = textCursor();
    if (cursor.hasSelection())
    {
        auto text = cursor.selectedText();
        if (QRegularExpression(
                R"((?:[_a-zA-Z][_a-zA-Z0-9]*)|(?<=\b|\s|^)(?i)(?:(?:(?:(?:(?:\d+(?:'\d+)*)?\.(?:\d+(?:'\d+)*)(?:e[+-]?(?:\d+(?:'\d+)*))?)|(?:(?:\d+(?:'\d+)*)\.(?:e[+-]?(?:\d+(?:'\d+)*))?)|(?:(?:\d+(?:'\d+)*)(?:e[+-]?(?:\d+(?:'\d+)*)))|(?:0x(?:[0-9a-f]+(?:'[0-9a-f]+)*)?\.(?:[0-9a-f]+(?:'[0-9a-f]+)*)(?:p[+-]?(?:\d+(?:'\d+)*)))|(?:0x(?:[0-9a-f]+(?:'[0-9a-f]+)*)\.?(?:p[+-]?(?:\d+(?:'\d+)*))))[lf]?)|(?:(?:(?:[1-9]\d*(?:'\d+)*)|(?:0[0-7]*(?:'[0-7]+)*)|(?:0x[0-9a-f]+(?:'[0-9a-f]+)*)|(?:0b[01]+(?:'[01]+)*))(?:u?l{0,2}|l{0,2}u?)))(?=\b|\s|$))")
                .match(text)
                .captured() == text)
        {
            auto *doc = document();
            cursor = doc->find(text, 0, QTextDocument::FindWholeWords | QTextDocument::FindCaseSensitively);
            while (!cursor.isNull())
            {
                if (cursor != textCursor())
                {
                    QTextEdit::ExtraSelection e;
                    e.cursor = cursor;
                    e.format.setBackground(getEditorColor(KSyntaxHighlighting::Theme::TextSelection));
                    occurrencesExtraSelections.push_back(e);
                }
                cursor = doc->find(text, cursor, QTextDocument::FindWholeWords | QTextDocument::FindCaseSensitively);
            }
        }
    }

    updateExtraSelections();
}

void CodeEditor::indent()
{
    addInEachLineOfSelection(QRegularExpression("^"), SettingsHelper::isReplaceTabs() ? m_tabReplace : "\t");
}

void CodeEditor::unindent()
{
    removeInEachLineOfSelection(QRegularExpression(QString("^(\t| {1,%1})").arg(SettingsHelper::getTabWidth())), true);
}

void CodeEditor::swapLineUp()
{
    auto cursor = textCursor();
    auto lines = toPlainText().remove('\r').split('\n');
    int selectionStart = cursor.selectionStart();
    int selectionEnd = cursor.selectionEnd();
    bool cursorAtEnd = cursor.position() == selectionEnd;
    cursor.setPosition(selectionStart);
    int lineStart = cursor.blockNumber();
    cursor.setPosition(selectionEnd);
    int lineEnd = cursor.blockNumber();

    if (lineStart == 0)
        return;
    selectionStart -= lines[lineStart - 1].length() + 1;
    selectionEnd -= lines[lineStart - 1].length() + 1;
    lines.move(lineStart - 1, lineEnd);

    cursor.select(QTextCursor::Document);
    cursor.insertText(lines.join('\n'));

    if (cursorAtEnd)
    {
        cursor.setPosition(selectionStart);
        cursor.setPosition(selectionEnd, QTextCursor::KeepAnchor);
    }
    else
    {
        cursor.setPosition(selectionEnd);
        cursor.setPosition(selectionStart, QTextCursor::KeepAnchor);
    }

    setTextCursor(cursor);
}

void CodeEditor::swapLineDown()
{
    auto cursor = textCursor();
    auto lines = toPlainText().remove('\r').split('\n');
    int selectionStart = cursor.selectionStart();
    int selectionEnd = cursor.selectionEnd();
    bool cursorAtEnd = cursor.position() == selectionEnd;
    cursor.setPosition(selectionStart);
    int lineStart = cursor.blockNumber();
    cursor.setPosition(selectionEnd);
    int lineEnd = cursor.blockNumber();

    if (lineEnd == document()->blockCount() - 1)
        return;
    selectionStart += lines[lineEnd + 1].length() + 1;
    selectionEnd += lines[lineEnd + 1].length() + 1;
    lines.move(lineEnd + 1, lineStart);

    cursor.select(QTextCursor::Document);
    cursor.insertText(lines.join('\n'));

    if (cursorAtEnd)
    {
        cursor.setPosition(selectionStart);
        cursor.setPosition(selectionEnd, QTextCursor::KeepAnchor);
    }
    else
    {
        cursor.setPosition(selectionEnd);
        cursor.setPosition(selectionStart, QTextCursor::KeepAnchor);
    }

    setTextCursor(cursor);
}

void CodeEditor::deleteLine()
{
    auto cursor = textCursor();
    int selectionStart = cursor.selectionStart();
    int selectionEnd = cursor.selectionEnd();
    cursor.setPosition(selectionStart);
    int lineStart = cursor.blockNumber();
    cursor.setPosition(selectionEnd);
    int lineEnd = cursor.blockNumber();
    int columnNumber = textCursor().columnNumber();
    cursor.movePosition(QTextCursor::Start);
    if (lineEnd == document()->blockCount() - 1)
    {
        if (lineStart == 0)
        {
            cursor.select(QTextCursor::Document);
        }
        else
        {
            cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineStart - 1);
            cursor.movePosition(QTextCursor::EndOfBlock);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        }
    }
    else
    {
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineStart);
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, lineEnd - lineStart + 1);
    }
    cursor.removeSelectedText();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                        qMin(columnNumber, cursor.block().text().length()));
    setTextCursor(cursor);
}

void CodeEditor::duplicate()
{
    auto cursor = textCursor();
    if (cursor.hasSelection()) // duplicate the selection
    {
        auto text = cursor.selectedText();
        bool cursorAtEnd = cursor.selectionEnd() == cursor.position();
        cursor.insertText(text + text);
        if (cursorAtEnd)
        {
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor, text.length());
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, text.length());
        }
        else
        {
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, text.length());
        }
    }
    else // duplicate the current line
    {
        int column = cursor.columnNumber();
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        auto text = cursor.selectedText();
        cursor.insertText(text + "\n" + text);
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, column);
    }
    setTextCursor(cursor);
}

void CodeEditor::toggleComment()
{
    const QString comment = languageRepo->singleLineCommentToken();

    if (!removeInEachLineOfSelection(QRegularExpression("^\\s*(" + comment + " ?)"), false))
    {
        addInEachLineOfSelection(QRegularExpression("\\S|^\\s*$"), comment + " ");
    }
}

void CodeEditor::toggleBlockComment()
{
    const QString commentStart = languageRepo->blockCommentTokens().first;
    const QString commentEnd = languageRepo->blockCommentTokens().second;

    if (commentStart.isEmpty() || commentEnd.isEmpty())
        return;

    auto cursor = textCursor();
    int startPos = cursor.selectionStart();
    int endPos = cursor.selectionEnd();
    bool cursorAtEnd = cursor.position() == endPos;
    auto text = cursor.selectedText();
    int pos1 = 0;
    int pos2 = 0;
    if (text.indexOf(commentStart) == 0 && text.length() >= commentStart.length() + commentEnd.length() &&
        text.lastIndexOf(commentEnd) + commentEnd.length() == text.length())
    {
        insertPlainText(text.mid(commentStart.length(), text.length() - commentStart.length() - commentEnd.length()));
        pos1 = startPos;
        pos2 = endPos - commentStart.length() - commentEnd.length();
    }
    else
    {
        insertPlainText(commentStart + text + commentEnd);
        pos1 = startPos;
        pos2 = endPos + commentStart.length() + commentEnd.length();
    }
    if (cursorAtEnd)
    {
        cursor.setPosition(pos1);
        cursor.setPosition(pos2, QTextCursor::KeepAnchor);
    }
    else
    {
        cursor.setPosition(pos2);
        cursor.setPosition(pos1, QTextCursor::KeepAnchor);
    }
    setTextCursor(cursor);
}

void CodeEditor::highlightParentheses()
{
    parenthesesExtraSelections.clear();

    auto currentSymbol = charUnderCursor();
    auto prevSymbol = charUnderCursor(-1);

    for (auto &p : parentheses)
    {
        int direction = 0;

        QChar counterSymbol;
        QChar activeSymbol;
        auto position = textCursor().position();

        if (p.left == currentSymbol)
        {
            direction = 1;
            counterSymbol = p.right;
            activeSymbol = currentSymbol;
        }
        else if (p.right == prevSymbol)
        {
            direction = -1;
            counterSymbol = p.left;
            activeSymbol = prevSymbol;
            position--;
        }
        else
        {
            continue;
        }

        auto counterStyle = highlighter->getFormat(position).textStyle();

        using TextStyle = KSyntaxHighlighting::Theme::TextStyle;

        auto needSame = [](const TextStyle &style) {
            return style == TextStyle::Comment || style == TextStyle::String || style == TextStyle::Char;
        };

        auto counter = 1;

        while (counter != 0 && position > 0 && position < (document()->characterCount() - 1))
        {
            // Moving position
            position += direction;

            auto activeStyle = highlighter->getFormat(position).textStyle();
            if (needSame(activeStyle) || needSame(counterStyle))
                if (activeStyle != counterStyle)
                    continue;

            auto character = document()->characterAt(position);
            // Checking symbol under position
            if (character == activeSymbol)
            {
                ++counter;
            }
            else if (character == counterSymbol)
            {
                --counter;
            }
        }

        // Found
        if (counter == 0)
        {
            QTextEdit::ExtraSelection selection;

            auto directionEnum = direction < 0 ? QTextCursor::MoveOperation::Left : QTextCursor::MoveOperation::Right;

            selection.format.setBackground(getEditorColor(KSyntaxHighlighting::Theme::BracketMatching));
            selection.cursor = textCursor();
            selection.cursor.clearSelection();
            selection.cursor.movePosition(directionEnum, QTextCursor::MoveMode::MoveAnchor,
                                          qAbs(textCursor().position() - position));

            selection.cursor.movePosition(QTextCursor::MoveOperation::Right, QTextCursor::MoveMode::KeepAnchor, 1);

            parenthesesExtraSelections.append(selection);

            selection.cursor = textCursor();
            selection.cursor.clearSelection();
            selection.cursor.movePosition(directionEnum, QTextCursor::MoveMode::KeepAnchor, 1);

            parenthesesExtraSelections.append(selection);
        }

        break;
    }

    updateExtraSelections();
}

void CodeEditor::highlightCurrentLine()
{
    currentLineExtraSelections.clear();

    if (m_highlightingCurrentLine && !isReadOnly())
    {
        QTextEdit::ExtraSelection selection;

        auto color = getEditorColor(KSyntaxHighlighting::Theme::CurrentLine);
        color.setAlpha(int(color.alpha() * 0.6));
        selection.format.setBackground(color);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();

        currentLineExtraSelections.append(selection);
    }

    updateExtraSelections();
}

int CodeEditor::getFirstVisibleBlock()
{
    // Detect the first block for which bounding rect - once translated
    // in absolute coordinated - is contained by the editor's text area

    // Costly way of doing but since "blockBoundingGeometry(...)" doesn't
    // exists for "QPlainTextEdit"...

    QTextCursor curs = QTextCursor(document());
    curs.movePosition(QTextCursor::Start);
    for (int i = 0; i < document()->blockCount(); ++i)
    {
        QTextBlock block = curs.block();

        QRect r1 = viewport()->geometry();
        QRect r2 = document()
                       ->documentLayout()
                       ->blockBoundingRect(block)
                       .translated(viewport()->geometry().x(),
                                   viewport()->geometry().y() - verticalScrollBar()->sliderPosition())
                       .toRect();

        if (r1.intersects(r2))
        {
            return i;
        }

        curs.movePosition(QTextCursor::NextBlock);
    }

    return 0;
}

void CodeEditor::keyPressEvent(QKeyEvent *e)
{
    if (completionEnabled && e->key() == Qt::Key_Space && e->modifiers() == Qt::ControlModifier)
    {
        completionTimer->stop();
        requestCompletion(true);
        return;
    }

    if (completionPopup->isVisible())
    {
        switch (e->key())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            if (e->modifiers() == Qt::NoModifier)
            {
                acceptCurrentCompletion();
                return;
            }
            hideCompletionPopup();
            break;
        case Qt::Key_Escape:
            if (e->modifiers() == Qt::NoModifier)
            {
                completionTimer->stop();
                clearCompletionSession();
                return;
            }
            break;
        case Qt::Key_Up:
        {
            if (e->modifiers() != Qt::NoModifier)
                break;
            const int row = completionPopup->currentRow();
            completionPopup->setCurrentRow(row <= 0 ? completionPopup->count() - 1 : row - 1);
            return;
        }
        case Qt::Key_Down:
        {
            if (e->modifiers() != Qt::NoModifier)
                break;
            const int row = completionPopup->currentRow();
            completionPopup->setCurrentRow(row + 1 >= completionPopup->count() ? 0 : row + 1);
            return;
        }
        case Qt::Key_PageUp:
            if (e->modifiers() == Qt::NoModifier)
            {
                completionPopup->setCurrentRow(qMax(0, completionPopup->currentRow() - 8));
                return;
            }
            break;
        case Qt::Key_PageDown:
            if (e->modifiers() == Qt::NoModifier)
            {
                completionPopup->setCurrentRow(qMin(completionPopup->count() - 1, completionPopup->currentRow() + 8));
                return;
            }
            break;
        default:
            break;
        }
    }

    /* if(m_vimCursor) */
    /* { */
    /*     QPlainTextEdit::keyPressEvent(e); */
    /*     proceedCompleterEnd(); */
    /*     return; */
    /* } */
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && e->modifiers() != Qt::NoModifier)
    {
        QKeyEvent pureEnter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
        if (e->modifiers() == Qt::ControlModifier)
        {
            moveCursor(QTextCursor::EndOfBlock);
            keyPressEvent(&pureEnter);
            return;
        }

        if (e->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
        {
            if (textCursor().blockNumber() == 0)
            {
                moveCursor(QTextCursor::StartOfBlock);
                insertPlainText("\n");
                moveCursor(QTextCursor::PreviousBlock);
                moveCursor(QTextCursor::EndOfBlock);
            }
            else
            {
                moveCursor(QTextCursor::PreviousBlock);
                moveCursor(QTextCursor::EndOfBlock);
                keyPressEvent(&pureEnter);
            }
            return;
        }
        if (e->modifiers() == Qt::ShiftModifier)
        {
            keyPressEvent(&pureEnter);
            return;
        }
    }

    if (e->key() == Qt::Key_Tab && e->modifiers() == Qt::NoModifier)
    {
        if (textCursor().hasSelection())
        {
            indent();
            return;
        }

        auto c = charUnderCursor();
        for (auto p : parentheses)
        {
            if (p.tabJumpOut && c == p.right)
            {
                moveCursor(QTextCursor::NextCharacter);
                return;
            }
        }

        if (SettingsHelper::isReplaceTabs())
        {
            insertPlainText(m_tabReplace);
            return;
        }
    }

    if (e->key() == Qt::Key_Backtab && e->modifiers() == Qt::ShiftModifier)
    {
        unindent();
        return;
    }

    if (e->key() == Qt::Key_Delete && e->modifiers() == Qt::ShiftModifier)
    {
        deleteLine();
        return;
    }

    // Auto indentation

    QString indentationSpaces =
        QRegularExpression("^\\s*").match(document()->findBlockByNumber(textCursor().blockNumber()).text()).captured();

    // Have Qt Edior like behaviour, if {|} and enter is pressed indent the two
    // parenthesis
    if (SettingsHelper::isAutoIndent() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
        e->modifiers() == Qt::NoModifier && charUnderCursor(-1) == '{' && charUnderCursor() == '}')
    {
        insertPlainText("\n" + indentationSpaces + (SettingsHelper::isReplaceTabs() ? m_tabReplace : "\t") + "\n" +
                        indentationSpaces);

        for (int i = 0; i <= indentationSpaces.length(); ++i)
            moveCursor(QTextCursor::MoveOperation::Left);

        return;
    }

    // Auto-indent for single "{" without "}"
    if (SettingsHelper::isAutoIndent() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
        e->modifiers() == Qt::NoModifier && charUnderCursor(-1) == '{')
    {
        insertPlainText("\n" + indentationSpaces + (SettingsHelper::isReplaceTabs() ? m_tabReplace : "\t"));
        setTextCursor(textCursor()); // scroll to the cursor
        return;
    }

    // Toggle Overwrite and insert mode in non-vim modes.
    if (e->key() == Qt::Key_Insert && !m_vimCursor)
    {
        setOverwriteMode(!overwriteMode());
        updateCursorWidth();
        return;
    }

    if (e->key() == Qt::Key_Backspace && e->modifiers() == Qt::NoModifier && !textCursor().hasSelection())
    {
        auto pre = charUnderCursor(-1);
        auto nxt = charUnderCursor();
        for (auto p : parentheses)
        {
            if (p.autoRemove && p.left == pre && p.right == nxt)
            {
                auto cursor = textCursor();
                cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
                cursor.removeSelectedText();
                setTextCursor(textCursor()); // scroll to the cursor
                return;
            }
        }

        if (textCursor().columnNumber() <= indentationSpaces.length() && textCursor().columnNumber() >= 1 &&
            !m_tabReplace.isEmpty())
        {
            auto cursor = textCursor();
            int realColumn = 0;
            int newIndentLength = 0;
            for (int i = 0; i < cursor.columnNumber(); ++i)
            {
                if (indentationSpaces[i] != '\t')
                    ++realColumn;
                else
                {
                    realColumn = (realColumn + m_tabReplace.length()) / m_tabReplace.length() * m_tabReplace.length();
                }
                if (realColumn % m_tabReplace.length() == 0 && i < cursor.columnNumber() - 1)
                {
                    newIndentLength = i + 1;
                }
            }
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor,
                                cursor.columnNumber() - newIndentLength);
            cursor.removeSelectedText();
            setTextCursor(textCursor()); // scroll to the cursor
            return;
        }
    }

    for (auto p : parentheses)
    {
        if (p.autoComplete)
        {
            auto cursor = textCursor();
            if (cursor.hasSelection())
            {
                if (p.left == e->text())
                {
                    // Add parentheses for selection
                    int startPos = cursor.selectionStart();
                    int endPos = cursor.selectionEnd();
                    bool cursorAtEnd = cursor.position() == endPos;
                    auto text = p.left + cursor.selectedText() + p.right;
                    insertPlainText(text);
                    if (cursorAtEnd)
                    {
                        cursor.setPosition(startPos + 1);
                        cursor.setPosition(endPos + 1, QTextCursor::KeepAnchor);
                    }
                    else
                    {
                        cursor.setPosition(endPos + 1);
                        cursor.setPosition(startPos + 1, QTextCursor::KeepAnchor);
                    }
                    setTextCursor(cursor);
                    return;
                }
            }
            else
            {
                if (p.right == e->text())
                {
                    auto symbol = charUnderCursor();

                    if (symbol == p.right)
                    {
                        moveCursor(QTextCursor::NextCharacter);
                        return;
                    }
                }

                if (p.left == e->text())
                {
                    insertPlainText(QString(p.left) + p.right);
                    moveCursor(QTextCursor::PreviousCharacter);
                    return;
                }
            }
        }
    }

    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && e->modifiers() == Qt::NoModifier)
    {
        insertPlainText("\n" + indentationSpaces.left(textCursor().columnNumber()));
        setTextCursor(textCursor()); // scroll to the cursor
        return;
    }

    if (e->key() == Qt::Key_Escape && textCursor().hasSelection())
    {
        auto cursor = textCursor();
        cursor.clearSelection();
        setTextCursor(cursor);
    }

    const auto shift = e->modifiers().testFlag(Qt::ShiftModifier);
    e->setModifiers(e->modifiers() & ~Qt::ShiftModifier); // to match Shift+Home to MoveToStartOfLine

    if (e->matches(QKeySequence::MoveToStartOfLine))
    {
        auto cursor = textCursor();
        const auto line = cursor.block().text();
        const auto leadingSpaceCount = QRegularExpression("^\\s*").match(line).capturedLength();
        const auto cursorPos = cursor.positionInBlock();
        const auto moveMode = shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
        if (cursorPos > leadingSpaceCount)
            cursor.movePosition(QTextCursor::PreviousCharacter, moveMode, cursorPos - leadingSpaceCount);
        else
            cursor.movePosition(QTextCursor::StartOfBlock, moveMode);
        setTextCursor(cursor);
        return;
    }

    if (e->matches(QKeySequence::MoveToEndOfLine))
    {
        auto cursor = textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        setTextCursor(cursor);
        return;
    }

    e->setModifiers(e->modifiers() | (shift ? Qt::ShiftModifier : Qt::NoModifier));

    QPlainTextEdit::keyPressEvent(e);
}

bool CodeEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip)
    {
        auto *helpEvent = dynamic_cast<QHelpEvent *>(event);
        auto point = helpEvent->pos();
        point.setX(point.x() - sidebarWidth());
        QTextCursor cursor = cursorForPosition(point);

        auto lineNumber = cursor.blockNumber() + 1;

        QTextCursor copyCursor(cursor);
        copyCursor.movePosition(QTextCursor::StartOfBlock);

        auto blockPositionStart = cursor.positionInBlock() - copyCursor.positionInBlock();
        QPair<int, int> positionOfTooltip{lineNumber, blockPositionStart};

        QString text;
        for (auto const &e : squiggles)
        {
            if (e.start <= positionOfTooltip && e.stop >= positionOfTooltip)
            {
                if (text.isEmpty())
                    text = e.tooltip;
                else
                    text += "; " + e.tooltip;
            }
        }

        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(helpEvent->globalPos(), text);

        return true;
    }
    return QPlainTextEdit::event(event);
}

void CodeEditor::highlightAllSquiggle()
{
    squigglesExtraSelections.clear();
    squigglesLineExtraSelections.clear();
    for (const auto &info : squiggles)
    {
        highlightSquiggle(info);
    }
    updateExtraSelections();
}

void CodeEditor::highlightSquiggle(const SquiggleInformation &info)
{

    auto cursor = textCursor();

    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, info.start.first - 1);
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, info.start.second);

    if (info.stop.first > info.start.first)
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, info.stop.first - info.start.first);

    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, info.stop.second);

    QTextCharFormat newcharfmt = currentCharFormat();
    newcharfmt.setFontUnderline(true);

    QColor color = getEditorColor(KSyntaxHighlighting::Theme::BackgroundColor);

    switch (info.level)
    {
    case SeverityLevel::Error:
        newcharfmt.setUnderlineColor(getTextColor(KSyntaxHighlighting::Theme::Error));
        newcharfmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
        color = getEditorColor(KSyntaxHighlighting::Theme::MarkError);
        // newcharfmt.setUnderlineStyle(m_syntaxStyle->getFormat("Error").underlineStyle());
        break;
    case SeverityLevel::Warning:
        newcharfmt.setUnderlineColor(getTextColor(KSyntaxHighlighting::Theme::Warning));
        newcharfmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
        color = getEditorColor(KSyntaxHighlighting::Theme::MarkWarning);
        // newcharfmt.setUnderlineStyle(m_syntaxStyle->getFormat("Warning").underlineStyle());
        break;
    case SeverityLevel::Information:
    case SeverityLevel::Hint:
        newcharfmt.setUnderlineColor(getTextColor(KSyntaxHighlighting::Theme::Information));
        newcharfmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
        color = getEditorColor(KSyntaxHighlighting::Theme::MarkWarning);
    }

    squigglesExtraSelections.push_back({cursor, newcharfmt});

    if (m_highlightingErrorLine)
    {
        QTextEdit::ExtraSelection newlinefmt;
        color.setAlpha(int(color.alpha() * 0.2));
        newlinefmt.format.setBackground(color);
        newlinefmt.format.setProperty(QTextFormat::FullWidthSelection, true);
        newlinefmt.cursor = cursor;
        newlinefmt.cursor.clearSelection();
        squigglesLineExtraSelections.push_back(newlinefmt);
    }
}

void CodeEditor::addSquiggle(SeverityLevel level, QPair<int, int> start, QPair<int, int> stop,
                             const QString &tooltipMessage)
{
    if (stop < start)
        return;
    squiggles.push_back({level, start, stop, tooltipMessage});
}

void CodeEditor::clearSquiggle()
{
    if (squiggles.empty())
        return;

    squiggles.clear();
    squigglesExtraSelections.clear();
    squigglesLineExtraSelections.clear();

    updateExtraSelections();
}

QChar CodeEditor::charUnderCursor(int offset) const
{
    auto block = textCursor().blockNumber();
    auto index = textCursor().positionInBlock();
    auto text = document()->findBlockByNumber(block).text();

    index += offset;

    if (index < 0 || index >= text.size())
    {
        return {};
    }

    return text[index];
}

bool CodeEditor::surroundedByCharInSingleLine(QChar c, int position, bool espace) const
{
    bool foundStart = false;
    bool foundStop = false;
    int start = position - 1;
    int stop = position + 1;
    QString code = document()->toPlainText();
    const int size = code.size();
    while (stop < size && start >= 0)
    {

        if (code[stop] == '\n' || code[start] == '\n')
            break;

        if (code[start] == c)
        {
            if (start > 0 && code[start - 1] == '\\' && espace)
                start--;
            else
                foundStart = true;
        }
        else
            start--;

        if (code[stop] == c)
        {
            if (stop < size && code[stop - 1] == '\\' && espace)
                stop++;
            else
                foundStop = true;
        }
        else
            stop++;
    }
    return foundStart && foundStop;
}

void CodeEditor::insertFromMimeData(const QMimeData *source)
{
    insertPlainText(source->text());
}

bool CodeEditor::removeInEachLineOfSelection(const QRegularExpression &regex, bool force)
{
    auto cursor = textCursor();
    auto lines = toPlainText().remove('\r').split('\n');
    int selectionStart = cursor.selectionStart();
    int selectionEnd = cursor.selectionEnd();
    bool cursorAtEnd = cursor.position() == selectionEnd;
    cursor.setPosition(selectionStart);
    int lineStart = cursor.blockNumber();
    cursor.setPosition(selectionEnd);
    int lineEnd = cursor.blockNumber();
    QString newText;
    QTextStream stream(&newText);
    int deleteTotal = 0;
    int deleteFirst = 0;
    for (int i = lineStart; i <= lineEnd; ++i)
    {
        auto line = lines[i];
        auto match = regex.match(line).captured(1);
        int len = match.length();
        if (len == 0 && !force)
            return false;
        if (i == lineStart)
            deleteFirst = len;
        deleteTotal += len;
        stream << line.remove(line.indexOf(match), len);
        if (i != lineEnd)
            stream << Qt::endl;
    }
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineStart);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, lineEnd - lineStart);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.insertText(newText);
    cursor.setPosition(qMax(0, selectionStart - deleteFirst));
    if (cursor.blockNumber() < lineStart)
    {
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineStart - cursor.blockNumber());
        cursor.movePosition(QTextCursor::StartOfBlock);
    }
    int pos = cursor.position();
    cursor.setPosition(selectionEnd - deleteTotal);
    if (cursor.blockNumber() < lineEnd)
    {
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineEnd - cursor.blockNumber());
        cursor.movePosition(QTextCursor::StartOfBlock);
    }
    int pos2 = cursor.position();
    if (cursorAtEnd)
    {
        cursor.setPosition(pos);
        cursor.setPosition(pos2, QTextCursor::KeepAnchor);
    }
    else
    {
        cursor.setPosition(pos2);
        cursor.setPosition(pos, QTextCursor::KeepAnchor);
    }
    setTextCursor(cursor);
    return true;
}

void CodeEditor::addInEachLineOfSelection(const QRegularExpression &regex, const QString &str)
{
    auto cursor = textCursor();
    auto lines = toPlainText().remove('\r').split('\n');
    int selectionStart = cursor.selectionStart();
    int selectionEnd = cursor.selectionEnd();
    bool cursorAtEnd = cursor.position() == selectionEnd;
    cursor.setPosition(selectionStart);
    int lineStart = cursor.blockNumber();
    cursor.setPosition(selectionEnd);
    int lineEnd = cursor.blockNumber();
    QString newText;
    QTextStream stream(&newText);
    for (int i = lineStart; i <= lineEnd; ++i)
    {
        auto line = lines[i];
        stream << line.insert(line.indexOf(regex), str);
        if (i != lineEnd)
            stream << Qt::endl;
    }
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, lineStart);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, lineEnd - lineStart);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.insertText(newText);
    int pos = selectionStart + str.length();
    int pos2 = selectionEnd + str.length() * (lineEnd - lineStart + 1);
    if (cursorAtEnd)
    {
        cursor.setPosition(pos);
        cursor.setPosition(pos2, QTextCursor::KeepAnchor);
    }
    else
    {
        cursor.setPosition(pos2);
        cursor.setPosition(pos, QTextCursor::KeepAnchor);
    }
    setTextCursor(cursor);
}

void CodeEditor::updateExtraSelections()
{
    QPlainTextEdit::setExtraSelections(squigglesLineExtraSelections + currentLineExtraSelections +
                                       parenthesesExtraSelections + occurrencesExtraSelections +
                                       squigglesExtraSelections);
}

QColor CodeEditor::getEditorColor(KSyntaxHighlighting::Theme::EditorColorRole role)
{
    return QColor::fromRgba(theme.editorColor(role));
}

QColor CodeEditor::getTextColor(KSyntaxHighlighting::Theme::TextStyle style)
{
    return QColor::fromRgba(theme.textColor(style));
}
} // namespace Editor
