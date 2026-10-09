// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <utility>

// Options shared by all tools (brush size, colors, ...), edited from the
// tool options bar and the color swatch.
class ToolSettings : public QObject {
    Q_OBJECT
public:
    explicit ToolSettings(QObject *parent = nullptr) : QObject(parent) { font.setPixelSize(48); }

    int size = 20;          // brush / stroke width in pixels
    int hardness = 100;     // 0..100
    int opacity = 100;      // 0..100
    int smoothing = 0;      // 0..100: how much brush strokes are steadied
    int tolerance = 32;     // 0..255
    bool contiguous = true;
    bool sampleMerged = false;
    bool fillShape = false;
    bool antialias = true;
    bool radial = false;
    int toneRange = 1;             // Dodge/Burn: 0 shadows, 1 midtones, 2 highlights
    bool spongeSaturate = false;   // Sponge: add saturation (true) or remove it
    double cropRatio = 0;          // Crop: width / height, 0 = free, -1 = the image's own ratio
    bool pressureSize = true;      // pen pressure controls brush size
    bool pressureOpacity = false;  // pen pressure controls brush opacity
    // View options, shared by every canvas.
    bool showRulers = false;
    bool showGuides = true;
    bool snap = true;              // to guides and the canvas edges and center
    QFont font{QStringLiteral("Sans Serif")};

    QColor foreground() const { return m_fg; }
    QColor background() const { return m_bg; }
    void setForeground(const QColor &c)
    {
        if (c != m_fg) { m_fg = c; emit colorsChanged(); }
    }
    void setBackground(const QColor &c)
    {
        if (c != m_bg) { m_bg = c; emit colorsChanged(); }
    }
    void swapColors() { std::swap(m_fg, m_bg); emit colorsChanged(); }
    // Called by the Text tool when editing existing text, so the options bar shows its style.
    void setTextStyle(const QFont &f, bool aa)
    {
        font = f;
        antialias = aa;
        emit textStyleChanged();
    }
    void resetColors() { m_fg = Qt::black; m_bg = Qt::white; emit colorsChanged(); }

signals:
    void colorsChanged();
    void textStyleChanged();

private:
    QColor m_fg = Qt::black;
    QColor m_bg = Qt::white;
};
