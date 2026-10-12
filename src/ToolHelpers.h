// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

// Small helpers shared by the tool implementations (Tools.cpp, BrushTool.cpp).

#include "Document.h"
#include "ToolSettings.h"
#include "Tools.h"

#include <QCursor>
#include <QImage>
#include <QPainter>
#include <QToolTip>
#include <QtMath>

// The image pixel under an image position.
inline QPoint pixelAt(const QPointF &p) { return QPoint(qFloor(p.x()), qFloor(p.y())); }

// A pixel as stored (premultiplied for layers), without QImage::pixel's conversion.
inline QRgb rawPixel(const QImage &img, const QPoint &p)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(p.y()))[p.x()];
}

// `img` holds the document region `area`; keeps it only inside the selection, if there is one.
inline QImage selectionMasked(QImage img, const Document *doc, const QRect &area)
{
    if (doc->hasSelection()) {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(QPoint(0, 0), doc->selection(), area);
    }
    return img;
}

// The color shown at `pos` (all layers), or an invalid color where the image is transparent.
inline QColor colorAt(const Document *doc, const QPointF &pos)
{
    const QPoint pt = pixelAt(pos);
    if (!doc->rect().contains(pt))
        return {};
    QColor c = doc->composite(QRect(pt, QSize(1, 1))).pixelColor(0, 0);
    if (c.alpha() == 0)
        return {};
    c.setAlpha(255);
    return c;
}

// Alt-click or Ctrl-click with a tool that paints with the foreground color picks that color from
// the image, like the Eyedropper. True when the click was used for that.
inline bool pickForeground(const ToolEvent &e, const Document *doc, ToolSettings *settings)
{
    if (e.button != Qt::LeftButton || !(e.modifiers & (Qt::AltModifier | Qt::ControlModifier)))
        return false;
    if (const QColor c = colorAt(doc, e.pos); c.isValid())
        settings->setForeground(c);
    return true;
}

inline void showTip(const QString &text) { QToolTip::showText(QCursor::pos(), text); }
