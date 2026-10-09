// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QList>
#include <QString>

struct FilterParam {
    QString label;
    int min;
    int max;
    int value;
    QString suffix;
};

// A color adjustment: used destructively from the Image > Adjustments menu
// and non-destructively by adjustment layers.
struct Adjustment {
    enum Type { None, BrightnessContrast, HueSaturation, Levels, Curves, Invert, Threshold, Posterize, TypeCount };
    Type type = None;
    // Meaning depends on type. Levels: 5 values for all channels, optionally followed by
    // 5 each for red, green and blue. Curves: flattened x,y points of the main curve,
    // optionally followed by -1 and, for red, green and blue, a point count and the points.
    // Hue/Saturation: hue, saturation, lightness, optionally followed by color ranges (7 values
    // each). Brightness/Contrast: brightness, contrast, optionally 1 for Photoshop's legacy formula.
    QList<int> params;
};

namespace Adjustments {

constexpr int kMaxParams = 1000;  // longest settings list accepted from files

QString name(Adjustment::Type type);
QString shortName(Adjustment::Type type);    // for layer thumbnails
QList<FilterParam> params(Adjustment::Type type);  // slider definitions (empty for Curves/Invert)
QList<int> defaults(Adjustment::Type type);
QImage apply(const QImage &image, Adjustment::Type type, const QList<int> &params);
// `params` with every setting clamped to its valid range (settings can come from damaged files).
QList<int> validated(Adjustment::Type type, QList<int> params);
// The part of the parameters the dialogs edit (Levels: first 5; Curves: the main curve).
QList<int> mainParams(Adjustment::Type type, const QList<int> &params);
// `params` with the main part replaced by `main`, keeping any per-channel settings.
QList<int> withMainParams(Adjustment::Type type, const QList<int> &params, const QList<int> &main);

} // namespace Adjustments
