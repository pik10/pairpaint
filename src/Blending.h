// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QPainter>

// Blend modes that Qt's painter doesn't provide. They are stored in Layer::mode next to
// QPainter's own modes, and PairPaint composites them itself.
namespace Blend {

constexpr QPainter::CompositionMode LinearBurn = QPainter::CompositionMode(1000);
constexpr QPainter::CompositionMode VividLight = QPainter::CompositionMode(1001);
constexpr QPainter::CompositionMode LinearLight = QPainter::CompositionMode(1002);
constexpr QPainter::CompositionMode PinLight = QPainter::CompositionMode(1003);
constexpr QPainter::CompositionMode HardMix = QPainter::CompositionMode(1004);
constexpr QPainter::CompositionMode Subtract = QPainter::CompositionMode(1005);
constexpr QPainter::CompositionMode Divide = QPainter::CompositionMode(1006);
constexpr QPainter::CompositionMode DarkerColor = QPainter::CompositionMode(1007);
constexpr QPainter::CompositionMode LighterColor = QPainter::CompositionMode(1008);
constexpr QPainter::CompositionMode Hue = QPainter::CompositionMode(1009);
constexpr QPainter::CompositionMode Saturation = QPainter::CompositionMode(1010);
constexpr QPainter::CompositionMode Color = QPainter::CompositionMode(1011);
constexpr QPainter::CompositionMode Luminosity = QPainter::CompositionMode(1012);
constexpr QPainter::CompositionMode Dissolve = QPainter::CompositionMode(1013);
// Groups only: the group's layers blend directly with what is below the group.
constexpr QPainter::CompositionMode PassThrough = QPainter::CompositionMode(1014);

// True for the modes PairPaint composites itself (its own modes, and Linear Dodge).
bool isCustom(QPainter::CompositionMode mode);

// Draws premultiplied `src` onto `dst` (same size) with any blend mode, Qt's or PairPaint's.
// `origin` is the document position of the images' top-left pixel (used by Dissolve).
void draw(QImage &dst, const QImage &src, QPainter::CompositionMode mode, qreal opacity, const QPoint &origin);

// dst = dst + (src - dst) * opacity * mask, where `mask` (may be null) is a layer mask and
// `area` the document region that dst and src cover.
void mix(QImage &dst, const QImage &src, qreal opacity, const QImage &mask, const QRect &area);

// dst = dst + (src - dst) * alpha of `coverage` (all three the same size).
void mixByAlpha(QImage &dst, const QImage &src, const QImage &coverage);

// Gives `img` the alpha channel of `alphaFrom` (same size), keeping its colors.
// Used for clipping masks: clipped layers only show where their base layer has pixels.
void restoreAlpha(QImage &img, const QImage &alphaFrom);

} // namespace Blend
