// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "TextBox.h"

#include "Document.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QTextOption>
#include <algorithm>
#include <cmath>

TextBox::TextBox(const TextData &text) : m_origin(text.pos), m_font(text.font)
{
    if (!text.antialias)
        m_font.setStyleStrategy(QFont::NoAntialias);
    m_lineHeight = QFontMetricsF(m_font).lineSpacing();
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    int start = 0;
    for (const QString &paragraph : text.text.split(QLatin1Char('\n'))) {
        auto layout = std::make_unique<QTextLayout>(paragraph, m_font);
        layout->setTextOption(option);
        layout->setCacheEnabled(true);
        layout->beginLayout();
        QTextLine line = layout->createLine();
        if (line.isValid())
            line.setPosition(QPointF(0, 0));
        layout->endLayout();
        m_lines.push_back({start, int(paragraph.size()), std::move(layout)});
        start += int(paragraph.size()) + 1;  // +1 for the line break
    }
}

void TextBox::draw(QPainter &p) const
{
    for (size_t i = 0; i < m_lines.size(); ++i)
        m_lines[i].layout->draw(&p, m_origin + QPointF(0, qreal(i) * m_lineHeight));
}

QRectF TextBox::bounds() const
{
    qreal width = 1;
    for (const Line &l : m_lines)
        if (l.layout->lineCount() > 0)
            width = std::max(width, l.layout->lineAt(0).naturalTextWidth());
    return QRectF(m_origin, QSizeF(width, m_lineHeight * qreal(m_lines.size())));
}

int TextBox::lineOf(int index) const
{
    for (size_t i = 0; i < m_lines.size(); ++i)
        if (index <= m_lines[i].start + m_lines[i].length)
            return int(i);
    return int(m_lines.size()) - 1;
}

qreal TextBox::cursorX(int line, int index) const
{
    const Line &l = m_lines[size_t(line)];
    if (l.layout->lineCount() == 0)
        return 0;
    return l.layout->lineAt(0).cursorToX(std::clamp(index - l.start, 0, l.length));
}

int TextBox::hitTest(const QPointF &pos) const
{
    const int i = std::clamp(int(std::floor((pos.y() - m_origin.y()) / m_lineHeight)), 0, int(m_lines.size()) - 1);
    const Line &l = m_lines[size_t(i)];
    if (l.layout->lineCount() == 0)
        return l.start;
    return l.start + l.layout->lineAt(0).xToCursor(pos.x() - m_origin.x(), QTextLine::CursorBetweenCharacters);
}

QLineF TextBox::cursorLine(int index) const
{
    const int i = lineOf(index);
    const qreal x = m_origin.x() + cursorX(i, index), y = m_origin.y() + i * m_lineHeight;
    return QLineF(x, y, x, y + m_lineHeight);
}

QList<QRectF> TextBox::selectionRects(int from, int to) const
{
    QList<QRectF> rects;
    if (from > to)
        std::swap(from, to);
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const Line &l = m_lines[i];
        const int a = std::max(from, l.start), b = std::min(to, l.start + l.length);
        if (a > b || (a == b && !(to > l.start + l.length && from <= l.start + l.length)))
            continue;
        qreal x1 = cursorX(int(i), a), x2 = cursorX(int(i), b);
        if (to > l.start + l.length)
            x2 += m_lineHeight * 0.3;  // show that the line break is selected too
        rects << QRectF(m_origin.x() + x1, m_origin.y() + qreal(i) * m_lineHeight, std::max<qreal>(1, x2 - x1), m_lineHeight);
    }
    return rects;
}

int TextBox::lineStart(int index) const { return m_lines[size_t(lineOf(index))].start; }

int TextBox::lineEnd(int index) const
{
    const Line &l = m_lines[size_t(lineOf(index))];
    return l.start + l.length;
}

int TextBox::moveVertically(int index, int lines) const
{
    const int from = lineOf(index);
    const int to = std::clamp(from + lines, 0, int(m_lines.size()) - 1);
    if (to == from)
        return lines < 0 ? 0 : m_lines.back().start + m_lines.back().length;
    const Line &l = m_lines[size_t(to)];
    if (l.layout->lineCount() == 0)
        return l.start;
    return l.start + l.layout->lineAt(0).xToCursor(cursorX(from, index), QTextLine::CursorBetweenCharacters);
}
