// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QFont>
#include <QLineF>
#include <QList>
#include <QRectF>
#include <QTextLayout>
#include <memory>
#include <vector>

struct TextData;
class QPainter;

// Lays out a text layer's text, one QTextLayout per line. Used both to draw text layers
// and to edit them on the canvas, so the cursor always matches what is drawn.
// Positions are in image coordinates; cursor positions are indexes into the text.
class TextBox {
public:
    explicit TextBox(const TextData &text);

    void draw(QPainter &p) const;
    QRectF bounds() const;  // at least one line high, even when empty

    int hitTest(const QPointF &pos) const;  // cursor position nearest to an image point
    QLineF cursorLine(int index) const;
    QList<QRectF> selectionRects(int from, int to) const;

    int lineStart(int index) const;
    int lineEnd(int index) const;
    int moveVertically(int index, int lines) const;  // up (negative) or down, keeping the x position

private:
    struct Line {
        int start;
        int length;
        std::unique_ptr<QTextLayout> layout;
    };
    int lineOf(int index) const;
    qreal cursorX(int line, int index) const;

    QPointF m_origin;
    qreal m_lineHeight = 0;
    QFont m_font;
    std::vector<Line> m_lines;
};
