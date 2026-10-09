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
    QList<int> params;  // meaning depends on type; Curves: flattened x,y control points
};

namespace Adjustments {

QString name(Adjustment::Type type);
QString shortName(Adjustment::Type type);    // for layer thumbnails
QList<FilterParam> params(Adjustment::Type type);  // slider definitions (empty for Curves/Invert)
QList<int> defaults(Adjustment::Type type);
QImage apply(const QImage &image, Adjustment::Type type, const QList<int> &params);

} // namespace Adjustments
