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

#include "HighLighter.hpp"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/FoldingRegion>
#include <KSyntaxHighlighting/Format>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>
#include <QThread>
#include <algorithm>
#include <utility>

namespace KSH = KSyntaxHighlighting;

namespace Editor
{

KSH::FoldingRegion Highlighter::foldingRegion(const QTextBlock &startBlock)
{
    auto *const data = dynamic_cast<TextBlockUserData *>(startBlock.userData());
    if (!data)
    {
        return KSH::FoldingRegion();
    }
    for (int i = data->foldingRegions.size() - 1; i >= 0; --i)
    {
        if (data->foldingRegions.at(i).type() == KSH::FoldingRegion::Begin)
        {
            return data->foldingRegions.at(i);
        }
    }
    return KSH::FoldingRegion();
}

Highlighter::Highlighter(QObject *parent) : QSyntaxHighlighter(parent)
{
}

Highlighter::Highlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{
}

Highlighter::~Highlighter() = default;

void Highlighter::setDefinition(const KSyntaxHighlighting::Definition &def)
{
    m_formatsIdToIndex.clear();
    m_semanticHighlights.clear();
    m_semanticRevision = -1;
    m_isCxxDefinition = def.name() == QLatin1String("C++") || def.name() == QLatin1String("ISO C++");
    AbstractHighlighter::setDefinition(def);

    auto definitions = def.includedDefinitions();

    if (!def.isValid() || (definitions.isEmpty() && def.formats().isEmpty()))
    {
        // dummy properties + formats
        m_formats.resize(1);
        m_formatsIdToIndex.insert(std::make_pair(m_formats[0].id(), 0));

        // be done, all below is just for the real highlighting variants
        return;
    }

    for (const auto &includedDefinition : definitions)
    {
        const auto formats = includedDefinition.formats();
        for (const auto &format : formats)
        {
            // register format id => internal attributes, we want no clashs
            const auto nextId = m_formats.size();
            m_formatsIdToIndex.insert(std::make_pair(format.id(), short(nextId)));
            m_formats.push_back(format);
        }
    }

    rehighlight();
}

bool Highlighter::startsFoldingRegion(const QTextBlock &startBlock)
{
    return foldingRegion(startBlock).type() == KSH::FoldingRegion::Begin;
}

QTextBlock Highlighter::findFoldingRegionEnd(const QTextBlock &startBlock)
{
    const auto region = foldingRegion(startBlock);

    auto block = startBlock;
    int depth = 1;
    while (block.isValid())
    {
        block = block.next();
        auto *const data = dynamic_cast<TextBlockUserData *>(block.userData());
        if (!data)
        {
            continue;
        }
        for (auto it = data->foldingRegions.constBegin(); it != data->foldingRegions.constEnd(); ++it)
        {
            if ((*it).id() != region.id())
            {
                continue;
            }
            if ((*it).type() == KSH::FoldingRegion::End)
            {
                --depth;
            }
            else if ((*it).type() == KSH::FoldingRegion::Begin)
            {
                ++depth;
            }
            if (depth == 0)
            {
                return block;
            }
        }
    }

    return QTextBlock();
}

void Highlighter::highlightBlock(const QString &text)
{

    KSH::State state;
    int cxxBracketDepth = 0;
    if (currentBlock().position() > 0)
    {
        const auto prevBlock = currentBlock().previous();
        auto *const prevData = dynamic_cast<TextBlockUserData *>(prevBlock.userData());
        if (prevData)
        {
            state = prevData->state;
            cxxBracketDepth = prevData->cxxBracketDepth;
        }
    }
    foldingRegions.clear();
    m_attributes.clear();

    state = highlightLine(text, state);

    if (m_isCxxDefinition)
        applyCxxFallback(text, cxxBracketDepth);
    applySemanticHighlights(currentBlock().blockNumber());

    auto *data = dynamic_cast<TextBlockUserData *>(currentBlockUserData());
    if (!data)
    { // first time we highlight this
        data = new TextBlockUserData;
        data->state = state;
        data->foldingRegions = foldingRegions;
        data->attributes = m_attributes;
        data->cxxBracketDepth = cxxBracketDepth;
        setCurrentBlockUserData(data);
        return;
    }

    data->attributes = m_attributes;

    if (data->state == state && data->foldingRegions == foldingRegions && data->cxxBracketDepth == cxxBracketDepth)
    { // we ended up in the same state, so we are done here
        return;
    }
    data->state = state;
    data->foldingRegions = foldingRegions;
    data->cxxBracketDepth = cxxBracketDepth;

    const auto nextBlock = currentBlock().next();
    auto *const currentDocument = document();
    if (nextBlock.isValid() && currentDocument)
    {
        // QTextBlock is only a lightweight handle into QTextDocument's private
        // storage. Keeping it in a queued invocation can leave the callback with
        // a stale handle when the document is replaced or edited before the
        // event is delivered. Store stable identifiers instead and resolve the
        // block again when the callback runs.
        const QPointer<QTextDocument> guardedDocument(currentDocument);
        const QPointer<Highlighter> guardedHighlighter(this);
        const int blockNumber = nextBlock.blockNumber();
        const int documentRevision = currentDocument->revision();
        QMetaObject::invokeMethod(
            this,
            [guardedHighlighter, guardedDocument, blockNumber, documentRevision]() {
                if (!guardedHighlighter || !guardedDocument ||
                    guardedHighlighter->document() != guardedDocument.data() ||
                    guardedDocument->revision() != documentRevision)
                {
                    return;
                }

                const QTextBlock block = guardedDocument->findBlockByNumber(blockNumber);
                if (!block.isValid() || block.document() != guardedDocument.data())
                {
                    return;
                }
                guardedHighlighter->rehighlightBlock(block);
            },
            Qt::QueuedConnection);
    }
}

void Highlighter::applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format)
{
    if (length == 0)
    {
        return;
    }

    QTextCharFormat tf;
    // always set the foreground color to avoid palette issues
    tf.setForeground(format.textColor(theme()));

    if (format.hasBackgroundColor(theme()))
    {
        tf.setBackground(format.backgroundColor(theme()));
    }
    if (format.isBold(theme()))
    {
        tf.setFontWeight(QFont::Bold);
    }
    if (format.isItalic(theme()))
    {
        tf.setFontItalic(true);
    }
    if (format.isUnderline(theme()))
    {
        tf.setFontUnderline(true);
    }
    if (format.isStrikeThrough(theme()))
    {
        tf.setFontStrikeOut(true);
    }

    const auto it = m_formatsIdToIndex.find(format.id());
    //    Q_ASSERT(it != m_formatsIdToIndex.end());

    if (it != m_formatsIdToIndex.end())
    {
        m_attributes.emplace_back(offset, length, it->second);
    }

    QSyntaxHighlighter::setFormat(offset, length, tf);
}

void Highlighter::applyFolding(int offset, int length, KSH::FoldingRegion region)
{
    Q_UNUSED(offset)
    Q_UNUSED(length)

    if (region.type() == KSH::FoldingRegion::Begin)
    {
        foldingRegions.push_back(region);
    }

    if (region.type() == KSH::FoldingRegion::End)
    {
        for (int i = foldingRegions.size() - 1; i >= 0; --i)
        {
            if (foldingRegions.at(i).id() != region.id() || foldingRegions.at(i).type() != KSH::FoldingRegion::Begin)
            {
                continue;
            }
            foldingRegions.removeAt(i);
            return;
        }
        foldingRegions.push_back(region);
    }
}

void Highlighter::setSemanticHighlights(const QVector<SemanticHighlight> &highlights, int documentRevision)
{
    Q_ASSERT(thread() == QThread::currentThread());

    const auto oldHighlights = m_semanticHighlights;
    const int oldRevision = m_semanticRevision;
    m_semanticHighlights.clear();
    m_semanticRevision = documentRevision;
    for (const auto &highlight : highlights)
    {
        if (highlight.line < 0 || highlight.start < 0 || highlight.length <= 0)
            continue;
        m_semanticHighlights[highlight.line].append(highlight);
    }

    for (auto it = m_semanticHighlights.begin(); it != m_semanticHighlights.end(); ++it)
    {
        std::sort(it.value().begin(), it.value().end(),
                  [](const SemanticHighlight &left, const SemanticHighlight &right) {
                      if (left.start != right.start)
                          return left.start < right.start;
                      return left.length < right.length;
                  });
    }
    rehighlightSemanticDiff(oldHighlights, oldRevision);
}

void Highlighter::clearSemanticHighlights()
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_semanticHighlights.isEmpty() && m_semanticRevision < 0)
        return;

    const auto oldHighlights = m_semanticHighlights;
    const int oldRevision = m_semanticRevision;
    m_semanticHighlights.clear();
    m_semanticRevision = -1;
    rehighlightSemanticDiff(oldHighlights, oldRevision);
}

void Highlighter::rehighlightSemanticDiff(const QHash<int, QVector<SemanticHighlight>> &oldHighlights, int oldRevision)
{
    auto *const currentDocument = document();
    if (!currentDocument)
        return;

    // An overlay from an older revision may still be baked into QTextLayout
    // formats of blocks that the edit did not touch. Its ranges no longer map
    // safely to the current document, so clear it with one full pass. Updates
    // within one revision can stay block-local.
    if (oldRevision >= 0 && oldRevision != currentDocument->revision())
    {
        rehighlight();
        return;
    }

    QSet<int> dirtyBlocks;
    if (oldRevision == currentDocument->revision())
    {
        for (auto it = oldHighlights.cbegin(); it != oldHighlights.cend(); ++it)
            dirtyBlocks.insert(it.key());
    }
    if (m_semanticRevision == currentDocument->revision())
    {
        for (auto it = m_semanticHighlights.cbegin(); it != m_semanticHighlights.cend(); ++it)
            dirtyBlocks.insert(it.key());
    }

    for (int blockNumber : dirtyBlocks)
    {
        const QTextBlock block = currentDocument->findBlockByNumber(blockNumber);
        if (block.isValid())
            rehighlightBlock(block);
    }
}

void Highlighter::overrideForeground(int offset, int length, const QColor &color)
{
    if (length <= 0 || !color.isValid())
        return;

    QTextCharFormat format = QSyntaxHighlighter::format(offset);
    format.setForeground(color);
    QSyntaxHighlighter::setFormat(offset, length, format);
}

bool Highlighter::isProtectedCxxSyntax(int offset) const
{
    if (offset < 0)
        return true;

    // getFormat() relies on the previous completed TextBlockUserData and is
    // therefore not suitable while highlightBlock() is still building it.
    // m_attributes, however, is populated synchronously by applyFormat().
    const auto found = std::upper_bound(
        m_attributes.cbegin(), m_attributes.cend(), offset,
        [](const int pos, const Attribute &attribute) { return pos < attribute.offset + attribute.length; });
    if (found == m_attributes.cend() || found->offset > offset || offset >= found->offset + found->length ||
        found->attributeValue < 0 || size_t(found->attributeValue) >= m_formats.size())
    {
        return false;
    }

    using TextStyle = KSH::Theme::TextStyle;
    switch (m_formats[size_t(found->attributeValue)].textStyle())
    {
    case TextStyle::Comment:
    case TextStyle::Documentation:
    case TextStyle::Annotation:
    case TextStyle::CommentVar:
    case TextStyle::String:
    case TextStyle::VerbatimString:
    case TextStyle::SpecialString:
    case TextStyle::Char:
    case TextStyle::SpecialChar:
    case TextStyle::Import:
    case TextStyle::DecVal:
    case TextStyle::BaseN:
    case TextStyle::Float:
    case TextStyle::Error:
        return true;
    default:
        return false;
    }
}

QColor Highlighter::textStyleColor(KSH::Theme::TextStyle style, const char *draculaColor) const
{
    if (theme().name() == QLatin1String("Dracula"))
        return QColor(QString::fromLatin1(draculaColor));
    return QColor::fromRgba(theme().textColor(style));
}

QColor Highlighter::semanticColor(SemanticHighlightKind kind) const
{
    // Dracula's compact semantic palette. These are intentionally explicit:
    // the stock KDE Dracula theme maps Operator to normal white and cannot
    // express rainbow bracket depth.
    switch (kind)
    {
    case SemanticHighlightKind::Namespace:
    case SemanticHighlightKind::Type:
    case SemanticHighlightKind::Class:
    case SemanticHighlightKind::Enum:
    case SemanticHighlightKind::Interface:
    case SemanticHighlightKind::Struct:
    case SemanticHighlightKind::TypeParameter:
        return textStyleColor(KSH::Theme::DataType, "#8BE9FD");
    case SemanticHighlightKind::Parameter:
        return textStyleColor(KSH::Theme::Variable, "#FFB86C");
    case SemanticHighlightKind::Variable:
    case SemanticHighlightKind::Property:
    case SemanticHighlightKind::Event:
        return textStyleColor(KSH::Theme::Variable, "#F8F8F2");
    case SemanticHighlightKind::EnumMember:
    case SemanticHighlightKind::Number:
        return textStyleColor(KSH::Theme::Constant, "#BD93F9");
    case SemanticHighlightKind::Function:
    case SemanticHighlightKind::Method:
        return textStyleColor(KSH::Theme::Function, "#50FA7B");
    case SemanticHighlightKind::Macro:
    case SemanticHighlightKind::Keyword:
    case SemanticHighlightKind::Operator:
        return textStyleColor(KSH::Theme::Keyword, "#FF79C6");
    case SemanticHighlightKind::Label:
        return textStyleColor(KSH::Theme::Others, "#F1FA8C");
    case SemanticHighlightKind::Comment:
        return textStyleColor(KSH::Theme::Comment, "#6272A4");
    case SemanticHighlightKind::Bracket:
    case SemanticHighlightKind::Unknown:
        return QColor();
    }
    return QColor();
}

void Highlighter::applySemanticHighlights(int blockNumber)
{
    auto *const currentDocument = document();
    if (!currentDocument || m_semanticRevision != currentDocument->revision())
        return;

    const auto it = m_semanticHighlights.constFind(blockNumber);
    if (it == m_semanticHighlights.cend())
        return;

    const int lineLength = currentBlock().length() - 1;
    for (const auto &highlight : it.value())
    {
        if (highlight.start >= lineLength)
            continue;

        const int length = qMin(highlight.length, lineLength - highlight.start);
        const QColor color = semanticColor(highlight.kind);
        if (color.isValid())
            overrideForeground(highlight.start, length, color);
    }
}

void Highlighter::applyCxxFallback(const QString &text, int &bracketDepth)
{
    const QColor operatorColor = textStyleColor(KSH::Theme::Keyword, "#FF79C6");
    const QColor functionColor = textStyleColor(KSH::Theme::Function, "#50FA7B");
    const QColor builtinTypeColor = textStyleColor(KSH::Theme::Keyword, "#FF79C6");
    const QColor bracketColors[] = {
        textStyleColor(KSH::Theme::Normal, "#F8F8F2"),   textStyleColor(KSH::Theme::Keyword, "#FF79C6"),
        textStyleColor(KSH::Theme::DataType, "#8BE9FD"), textStyleColor(KSH::Theme::Function, "#50FA7B"),
        textStyleColor(KSH::Theme::Constant, "#BD93F9"), textStyleColor(KSH::Theme::Others, "#FFB86C"),
    };
    static const QSet<QString> excludedCallNames = {
        QStringLiteral("alignas"),       QStringLiteral("alignof"),  QStringLiteral("catch"),
        QStringLiteral("decltype"),      QStringLiteral("for"),      QStringLiteral("if"),
        QStringLiteral("noexcept"),      QStringLiteral("requires"), QStringLiteral("sizeof"),
        QStringLiteral("static_assert"), QStringLiteral("switch"),   QStringLiteral("typeid"),
        QStringLiteral("while"),
    };
    static const QRegularExpression functionPattern(QStringLiteral(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*(?=\())"));
    static const QRegularExpression builtinTypePattern(QStringLiteral(
        R"(\b(?:bool|char|char8_t|char16_t|char32_t|double|float|int|long|short|signed|unsigned|void|wchar_t)\b)"));

    auto builtinIt = builtinTypePattern.globalMatch(text);
    while (builtinIt.hasNext())
    {
        const auto match = builtinIt.next();
        const int start = match.capturedStart();
        if (!isProtectedCxxSyntax(start))
            overrideForeground(start, match.capturedLength(), builtinTypeColor);
    }

    // Color operators before brackets. Multi-character operators are grouped
    // so their existing font attributes remain uniform.
    for (int i = 0; i < text.size();)
    {
        if (isProtectedCxxSyntax(i))
        {
            ++i;
            continue;
        }

        const QChar ch = text.at(i);
        if (!QStringLiteral("+-*/%=!<>&|^~?:.").contains(ch))
        {
            ++i;
            continue;
        }

        int length = 1;
        while (i + length < text.size() && length < 3 &&
               QStringLiteral("+-*/%=!<>&|^~?:.").contains(text.at(i + length)) && !isProtectedCxxSyntax(i + length))
        {
            ++length;
        }
        overrideForeground(i, length, operatorColor);
        i += length;
    }

    auto matchIt = functionPattern.globalMatch(text);
    while (matchIt.hasNext())
    {
        const auto match = matchIt.next();
        const int start = match.capturedStart(1);
        const int length = match.capturedLength(1);
        if (length <= 0 || excludedCallNames.contains(match.captured(1)) || isProtectedCxxSyntax(start))
            continue;
        overrideForeground(start, length, functionColor);
    }

    for (int i = 0; i < text.size(); ++i)
    {
        if (isProtectedCxxSyntax(i))
            continue;

        const QChar ch = text.at(i);
        if (ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char('{'))
        {
            overrideForeground(i, 1, bracketColors[bracketDepth % 6]);
            ++bracketDepth;
        }
        else if (ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}'))
        {
            bracketDepth = qMax(0, bracketDepth - 1);
            overrideForeground(i, 1, bracketColors[bracketDepth % 6]);
        }
    }
}

KSH::Format Highlighter::getFormat(int pos)
{
    auto block = document()->findBlock(pos);
    const auto *data = dynamic_cast<TextBlockUserData *>(block.userData());
    pos -= block.position();
    const auto &attr = data->attributes;
    auto found = std::upper_bound(attr.cbegin(), attr.cend(), pos,
                                  [](const int &p, const Attribute &x) { return p < x.offset + x.length; });
    if (found != attr.cend() && found->offset <= pos && pos < (found->offset + found->length))
    {
        return m_formats[found->attributeValue];
    }
    return m_formats.front();
}

} // namespace Editor
