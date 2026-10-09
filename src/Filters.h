// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QList>

// Image filters. Inputs may be any format; outputs are ARGB32_Premultiplied
// (or Alpha8 for floodMask). Work is spread over all cores with QtConcurrent.
namespace Filters {

QImage invert(const QImage &src);
QImage desaturate(const QImage &src);
QImage brightnessContrast(const QImage &src, int brightness, int contrast);
QImage hueSaturation(const QImage &src, int hue, int saturation, int lightness);
QImage threshold(const QImage &src, int level);
QImage posterize(const QImage &src, int levels);
QImage gaussianBlur(const QImage &src, double radius);
QImage unsharpMask(const QImage &src, int amountPercent, double radius, int threshold);
QImage addNoise(const QImage &src, int amountPercent);
QImage pixelate(const QImage &src, int cellSize);
QImage levels(const QImage &src, int inBlack, int inWhite, double gamma, int outBlack, int outWhite);
QImage curves(const QImage &src, const QList<int> &points);  // flattened x,y pairs in 0..255

// Lookup table of a smooth monotone curve through the control points.
QList<int> curveLut(const QList<int> &points);
// Luminance histogram (256 bins) of the non-transparent pixels.
QList<int> histogram(const QImage &src);

// Healing: keeps the texture of `source` but takes the low-frequency color and
// lighting of `dest` around the stroke. `mask` (Alpha8) is the stroke coverage.
QImage heal(const QImage &source, const QImage &dest, const QImage &mask, double sigma);

// Pixels connected to (or, if !contiguous, anywhere matching) the seed color
// within `tolerance` (0..255 per channel). Returns a Format_Alpha8 mask.
QImage floodMask(const QImage &src, const QPoint &seed, int tolerance, bool contiguous);

// Mask operations on Format_Alpha8 masks (selections).
// Grows (radius > 0) or shrinks (radius < 0) a mask with round, anti-aliased edges.
QImage morphMask(const QImage &mask, double radius);
QImage featherMask(const QImage &mask, double radius);
// Soft selection of pixels close to `color`; `fuzziness` 0..255.
QImage colorRangeMask(const QImage &src, const QColor &color, int fuzziness);

} // namespace Filters
