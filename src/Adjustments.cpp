// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Adjustments.h"

#include "Filters.h"

#include <QObject>

namespace Adjustments {

QString name(Adjustment::Type type)
{
    switch (type) {
    case Adjustment::BrightnessContrast: return QObject::tr("Brightness/Contrast");
    case Adjustment::HueSaturation: return QObject::tr("Hue/Saturation");
    case Adjustment::Levels: return QObject::tr("Levels");
    case Adjustment::Curves: return QObject::tr("Curves");
    case Adjustment::Invert: return QObject::tr("Invert");
    case Adjustment::Threshold: return QObject::tr("Threshold");
    case Adjustment::Posterize: return QObject::tr("Posterize");
    default: return {};
    }
}

QString shortName(Adjustment::Type type)
{
    switch (type) {
    case Adjustment::BrightnessContrast: return QStringLiteral("B/C");
    case Adjustment::HueSaturation: return QStringLiteral("H/S");
    case Adjustment::Levels: return QStringLiteral("Lvl");
    case Adjustment::Curves: return QStringLiteral("Crv");
    case Adjustment::Invert: return QStringLiteral("Inv");
    case Adjustment::Threshold: return QStringLiteral("Thr");
    case Adjustment::Posterize: return QStringLiteral("Pst");
    default: return {};
    }
}

QList<FilterParam> params(Adjustment::Type type)
{
    switch (type) {
    case Adjustment::BrightnessContrast:
        return {{QObject::tr("Brightness"), -150, 150, 0, {}}, {QObject::tr("Contrast"), -100, 100, 0, {}}};
    case Adjustment::HueSaturation:
        return {{QObject::tr("Hue"), -180, 180, 0, QStringLiteral("°")},
                {QObject::tr("Saturation"), -100, 100, 0, {}},
                {QObject::tr("Lightness"), -100, 100, 0, {}}};
    case Adjustment::Levels:
        return {{QObject::tr("Input black"), 0, 253, 0, {}},
                {QObject::tr("Input white"), 2, 255, 255, {}},
                {QObject::tr("Gamma (×100)"), 10, 999, 100, {}},
                {QObject::tr("Output black"), 0, 255, 0, {}},
                {QObject::tr("Output white"), 0, 255, 255, {}}};
    case Adjustment::Threshold:
        return {{QObject::tr("Level"), 1, 255, 128, {}}};
    case Adjustment::Posterize:
        return {{QObject::tr("Levels"), 2, 32, 4, {}}};
    default:
        return {};
    }
}

QList<int> defaults(Adjustment::Type type)
{
    if (type == Adjustment::Curves)
        return {0, 0, 255, 255};
    QList<int> v;
    for (const FilterParam &p : params(type))
        v << p.value;
    return v;
}

QImage apply(const QImage &image, Adjustment::Type type, const QList<int> &p)
{
    const QList<int> d = defaults(type);
    const QList<int> v = (type == Adjustment::Curves || p.size() >= d.size()) ? p : d;
    switch (type) {
    case Adjustment::BrightnessContrast:
        if (v.value(2) == 1)  // Photoshop's legacy formula (from PSD files)
            return Filters::brightnessContrastLegacy(image, v[0], v[1]);
        return Filters::brightnessContrast(image, v[0], v[1]);
    case Adjustment::HueSaturation: return Filters::hueSaturation(image, v[0], v[1], v[2], v.mid(3));
    case Adjustment::Levels: {
        // Each channel's own levels first, then the levels for all channels.
        const QList<int> all = Filters::levelsLut(v[0], v[1], v[2] / 100.0, v[3], v[4]);
        QList<int> luts[3] = {all, all, all};
        for (int c = 0; c < 3 && v.size() >= 10 + 5 * c; ++c) {
            const QList<int> own = Filters::levelsLut(v[5 + 5 * c], v[6 + 5 * c], v[7 + 5 * c] / 100.0,
                                                      v[8 + 5 * c], v[9 + 5 * c]);
            for (int i = 0; i < 256; ++i)
                luts[c][i] = all[own[i]];
        }
        return Filters::applyLuts(image, luts[0], luts[1], luts[2]);
    }
    case Adjustment::Curves: {
        const QList<int> main = mainParams(type, v);
        const QList<int> all = Filters::curveLut(main.size() >= 4 ? main : d);
        QList<int> luts[3] = {all, all, all};
        int pos = int(main.size()) + 1;  // after the -1 separator
        for (int c = 0; c < 3 && pos < v.size(); ++c) {
            const int count = v[pos++];
            const QList<int> own = Filters::curveLut(v.mid(pos, 2 * count));
            pos += 2 * count;
            for (int i = 0; i < 256; ++i)
                luts[c][i] = all[own[i]];
        }
        return Filters::applyLuts(image, luts[0], luts[1], luts[2]);
    }
    case Adjustment::Invert: return Filters::invert(image);
    case Adjustment::Threshold: return Filters::threshold(image, v[0]);
    case Adjustment::Posterize: return Filters::posterize(image, v[0]);
    default: return image;
    }
}

QList<int> mainParams(Adjustment::Type type, const QList<int> &params)
{
    if (type == Adjustment::Levels)
        return params.mid(0, 5);
    if (type == Adjustment::HueSaturation || type == Adjustment::BrightnessContrast)
        return params.mid(0, type == Adjustment::HueSaturation ? 3 : 2);
    if (type == Adjustment::Curves) {
        const qsizetype sep = params.indexOf(-1);
        return sep < 0 ? params : params.mid(0, sep);
    }
    return params;
}

QList<int> withMainParams(Adjustment::Type type, const QList<int> &params, const QList<int> &main)
{
    if (type == Adjustment::Levels && params.size() > 5)
        return main + params.mid(5);
    if (type == Adjustment::HueSaturation && params.size() > 3)
        return main + params.mid(3);  // color ranges
    if (type == Adjustment::BrightnessContrast && params.size() > 2)
        return main + params.mid(2);  // legacy flag
    if (type == Adjustment::Curves) {
        const qsizetype sep = params.indexOf(-1);
        if (sep >= 0)
            return main + params.mid(sep);
    }
    return main;
}

} // namespace Adjustments
