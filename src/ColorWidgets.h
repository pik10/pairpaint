// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QColor>
#include <QList>
#include <QWidget>

class ToolSettings;

// Foreground / background color squares, with swap and reset buttons.
class ColorSwatch : public QWidget {
    Q_OBJECT
public:
    explicit ColorSwatch(ToolSettings *settings, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {60, 60}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    static QRect fgRect() { return {4, 4, 32, 32}; }
    static QRect bgRect() { return {22, 22, 32, 32}; }
    static QRect swapRect() { return {38, 2, 18, 16}; }
    static QRect resetRect() { return {2, 40, 16, 16}; }
    ToolSettings *m_settings;
};

// A grid of preset colors. Left click sets the foreground, right click the background.
class SwatchPalette : public QWidget {
    Q_OBJECT
public:
    explicit SwatchPalette(ToolSettings *settings, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    int columns() const;
    QRect cellRect(int i) const;
    ToolSettings *m_settings;
    QList<QColor> m_colors;
};
