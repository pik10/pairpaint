// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Adjustments.h"

#include "Filters.h"

#include <QObject>
#include <algorithm>
#include <cmath>
#include <numeric>

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
    case Adjustment::Vibrance: return QObject::tr("Vibrance");
    case Adjustment::Exposure: return QObject::tr("Exposure");
    case Adjustment::ColorBalance: return QObject::tr("Color Balance");
    case Adjustment::WhiteBalance: return QObject::tr("White Balance");
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
    case Adjustment::Vibrance: return QStringLiteral("Vib");
    case Adjustment::Exposure: return QStringLiteral("Exp");
    case Adjustment::ColorBalance: return QStringLiteral("CB");
    case Adjustment::WhiteBalance: return QStringLiteral("WB");
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
    case Adjustment::Vibrance:
        return {{QObject::tr("Vibrance"), -100, 100, 0, {}}, {QObject::tr("Saturation"), -100, 100, 0, {}}};
    case Adjustment::Exposure:
        return {{QObject::tr("Exposure (stops ×100)"), -1000, 1000, 0, {}},
                {QObject::tr("Offset (×1000)"), -500, 500, 0, {}},
                {QObject::tr("Gamma (×100)"), 10, 999, 100, {}}};
    case Adjustment::ColorBalance: {
        QList<FilterParam> list;
        const QString ranges[3] = {QObject::tr("Shadows"), QObject::tr("Midtones"), QObject::tr("Highlights")};
        const QString axes[3] = {QObject::tr("Cyan – Red"), QObject::tr("Magenta – Green"), QObject::tr("Yellow – Blue")};
        for (const QString &range : ranges)
            for (const QString &axis : axes)
                list.append({range + QStringLiteral(": ") + axis, -100, 100, 0, {}});
        return list;
    }
    case Adjustment::WhiteBalance:
        return {{QObject::tr("Temperature"), -100, 100, 0, {}}, {QObject::tr("Tint"), -100, 100, 0, {}}};
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

// Keeps every setting in its valid range: settings can come from damaged files, and
// out-of-range values could overflow or divide by zero in the formulas.
QList<int> validated(Adjustment::Type type, QList<int> v)
{
    const QList<FilterParam> ranges = params(type);
    switch (type) {
    case Adjustment::Curves:
        for (int &x : v)
            x = std::clamp(x, -1, 255);  // points are 0..255; -1 separates the channel curves
        break;
    case Adjustment::Levels:
        for (int i = 0; i < v.size(); ++i)
            v[i] = std::clamp(v[i], ranges[i % 5].min, ranges[i % 5].max);
        break;
    case Adjustment::HueSaturation:
        for (int i = 0; i < v.size(); ++i) {
            if (i < 3) {
                v[i] = std::clamp(v[i], ranges[i].min, ranges[i].max);
            } else {
                // Color ranges, seven values each: four hues, then hue, saturation, lightness.
                static constexpr int lo[7] = {0, 0, 0, 0, -180, -100, -100};
                static constexpr int hi[7] = {360, 360, 360, 360, 180, 100, 100};
                const int k = (i - 3) % 7;
                v[i] = std::clamp(v[i], lo[k], hi[k]);
            }
        }
        break;
    case Adjustment::BrightnessContrast:
    case Adjustment::ColorBalance:  // followed by a 0/1 flag
        for (int i = 0; i < v.size(); ++i)
            v[i] = i < ranges.size() ? std::clamp(v[i], ranges[i].min, ranges[i].max) : std::clamp(v[i], 0, 1);
        break;
    default:
        for (int i = 0; i < v.size() && i < ranges.size(); ++i)
            v[i] = std::clamp(v[i], ranges[i].min, ranges[i].max);
        break;
    }
    return v;
}

QImage apply(const QImage &image, Adjustment::Type type, const QList<int> &p)
{
    const QList<int> d = defaults(type);
    const QList<int> v = validated(type, (type == Adjustment::Curves || p.size() >= d.size()) ? p : d);
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
            if (count < 0 || pos + 2 * qsizetype(count) > v.size())
                break;  // damaged data
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
    case Adjustment::Vibrance: return Filters::vibrance(image, v[0], v[1]);
    case Adjustment::Exposure: return Filters::exposure(image, v[0] / 100.0, v[1] / 1000.0, v[2] / 100.0);
    case Adjustment::ColorBalance: return Filters::colorBalance(image, v.mid(0, 9), v.value(9, 1) != 0);
    case Adjustment::WhiteBalance: return Filters::whiteBalance(image, v[0], v[1]);
    default: return image;
    }
}

QList<int> mainParams(Adjustment::Type type, const QList<int> &params)
{
    if (type == Adjustment::Levels)
        return params.mid(0, 5);
    if (type == Adjustment::HueSaturation || type == Adjustment::BrightnessContrast || type == Adjustment::ColorBalance)
        return params.mid(0, Adjustments::params(type).size());
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
    if (type == Adjustment::ColorBalance && params.size() > 9)
        return main + params.mid(9);  // preserve luminosity flag
    if (type == Adjustment::Curves) {
        const qsizetype sep = params.indexOf(-1);
        if (sep >= 0)
            return main + params.mid(sep);
    }
    return main;
}

QList<int> autoLevels(const QImage &image, Auto mode)
{
    QList<int> result = defaults(Adjustment::Levels);
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);  // once, for both passes
    const auto hist = Filters::channelHistograms(img);
    const qint64 total = std::accumulate(hist[0].begin(), hist[0].end(), qint64(0));
    if (total == 0)
        return result;
    // The darkest and lightest levels, ignoring the outermost 0.1% (stray pixels, noise).
    auto ends = [](const QList<int> &bins, qint64 count) {
        const qint64 clip = count / 1000;
        int lo = 0, hi = 255;
        for (qint64 sum = 0; lo < 255 && (sum += bins[lo]) <= clip;)
            ++lo;
        for (qint64 sum = 0; hi > 0 && (sum += bins[hi]) <= clip;)
            --hi;
        if (hi - lo < 2)  // flat: nothing to stretch
            return std::pair(0, 255);
        return std::pair(std::min(lo, 253), std::max(hi, 2));
    };
    if (mode == Auto::Contrast) {
        QList<int> all(256, 0);
        for (int i = 0; i < 256; ++i)
            all[i] = hist[0][i] + hist[1][i] + hist[2][i];
        const auto [lo, hi] = ends(all, 3 * total);
        result[0] = lo;
        result[1] = hi;
        return result;
    }
    QList<int> luts[3];
    for (int c = 0; c < 3; ++c) {
        const auto [lo, hi] = ends(hist[c], total);
        result << lo << hi << 100 << 0 << 255;
        luts[c] = Filters::levelsLut(lo, hi, 1.0, 0, 255);
    }
    if (mode == Auto::Color) {
        // Average the near-gray midtones after stretching, then bend each channel's gamma so
        // that average becomes neutral: this removes a color cast.
        const int step = std::max(1, int(std::sqrt(double(img.width()) * img.height() / 1e6)));
        double sum[3] = {};
        qint64 count = 0;
        for (int y = 0; y < img.height(); y += step) {
            const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x += step) {
                if (qAlpha(row[x]) == 0)
                    continue;
                const int r = luts[0][qRed(row[x])], g = luts[1][qGreen(row[x])], b = luts[2][qBlue(row[x])];
                const int mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (r + g + b) / 3;
                if (mx - mn < 60 && l > 40 && l < 215) {
                    sum[0] += r;
                    sum[1] += g;
                    sum[2] += b;
                    ++count;
                }
            }
        }
        if (count > 0) {
            const double gray = (sum[0] + sum[1] + sum[2]) / (3.0 * count) / 255;
            for (int c = 0; c < 3; ++c) {
                const double avg = std::clamp(sum[c] / count / 255, 0.01, 0.99);
                const double gamma = std::clamp(std::log(avg) / std::log(gray), 0.5, 2.0);
                result[5 + 5 * c + 2] = int(std::lround(gamma * 100));
            }
        }
    }
    return result;
}

} // namespace Adjustments
