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
    case Adjustment::BrightnessContrast: return Filters::brightnessContrast(image, v[0], v[1]);
    case Adjustment::HueSaturation: return Filters::hueSaturation(image, v[0], v[1], v[2]);
    case Adjustment::Levels: return Filters::levels(image, v[0], v[1], v[2] / 100.0, v[3], v[4]);
    case Adjustment::Curves: return Filters::curves(image, v.size() >= 4 ? v : d);
    case Adjustment::Invert: return Filters::invert(image);
    case Adjustment::Threshold: return Filters::threshold(image, v[0]);
    case Adjustment::Posterize: return Filters::posterize(image, v[0]);
    default: return image;
    }
}

} // namespace Adjustments
