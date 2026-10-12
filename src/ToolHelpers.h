// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

// Small helpers shared by the tool implementations (Tools.cpp, BrushTool.cpp).

#include "Document.h"

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

inline void showTip(const QString &text) { QToolTip::showText(QCursor::pos(), text); }
