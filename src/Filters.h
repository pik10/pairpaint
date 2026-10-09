// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QList>

#include <array>

// Image filters. Inputs may be any format; outputs are ARGB32_Premultiplied
// (or Alpha8 for floodMask). Work is spread over all cores with QtConcurrent.
namespace Filters {

QImage invert(const QImage &src);
QImage desaturate(const QImage &src);
QImage brightnessContrast(const QImage &src, int brightness, int contrast);
// Photoshop's "Use Legacy" Brightness/Contrast: add brightness, then stretch around the middle.
QImage brightnessContrastLegacy(const QImage &src, int brightness, int contrast);
// `ranges`: optional color ranges, 7 values each: the range's outer start, inner start, inner
// end and outer end hue (degrees), then its own hue, saturation and lightness changes.
QImage hueSaturation(const QImage &src, int hue, int saturation, int lightness, const QList<int> &ranges = {});
// Vibrance raises the saturation of dull colors more than of saturated ones and spares skin
// tones; saturation changes all colors alike. Both -100..100.
QImage vibrance(const QImage &src, int vibrance, int saturation);
// Photographic exposure in linear light: `stops` (2^stops), then `offset` added, then gamma.
QImage exposure(const QImage &src, double stops, double offset, double gamma);
// `values`: 9 shifts in -100..100, cyan-red, magenta-green and yellow-blue for the shadows, then
// the midtones, then the highlights.
QImage colorBalance(const QImage &src, const QList<int> &values, bool preserveLuminosity);
// Temperature (-100 cool/blue .. 100 warm/yellow) and tint (-100 green .. 100 magenta), keeping
// the brightness.
QImage whiteBalance(const QImage &src, int temperature, int tint);
QImage threshold(const QImage &src, int level);
QImage posterize(const QImage &src, int levels);
QImage gaussianBlur(const QImage &src, double radius);
QImage unsharpMask(const QImage &src, int amountPercent, double radius, int threshold);
QImage addNoise(const QImage &src, int amountPercent);
QImage pixelate(const QImage &src, int cellSize);
// Lightens (dodge) or darkens (burn) the shadows, midtones or highlights (range 0..2).
QImage dodgeBurn(const QImage &src, bool burn, int range);
QImage levels(const QImage &src, int inBlack, int inWhite, double gamma, int outBlack, int outWhite);
QImage curves(const QImage &src, const QList<int> &points);  // flattened x,y pairs in 0..255

// Applies separate lookup tables (256 entries each) to the red, green and blue channels.
QImage applyLuts(const QImage &src, const QList<int> &red, const QList<int> &green, const QList<int> &blue);
// Lookup table for Levels: input black/white, gamma, output black/white.
QList<int> levelsLut(int inBlack, int inWhite, double gamma, int outBlack, int outWhite);

// Lookup table of a smooth monotone curve through the control points.
QList<int> curveLut(const QList<int> &points);
// Luminance histogram (256 bins) of the non-transparent pixels.
QList<int> histogram(const QImage &src);
// Red, green and blue histograms (256 bins each) of the non-transparent pixels.
std::array<QList<int>, 3> channelHistograms(const QImage &src);

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
