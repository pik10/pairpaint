// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Filters.h"

#include <QColor>
#include <QList>
#include <QRandomGenerator>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

template <typename F>
void parallelFor(int count, F f)
{
    QList<int> items(count);
    std::iota(items.begin(), items.end(), 0);
    QtConcurrent::blockingMap(items, [&](const int &i) { f(i); });
}

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Applies f to every pixel in straight (non-premultiplied) ARGB.
template <typename F>
QImage mapPixels(const QImage &src, F f)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    uchar *bits = img.bits();  // detach before going parallel
    const qsizetype bpl = img.bytesPerLine();
    const int w = img.width();
    parallelFor(img.height(), [&](int y) {
        QRgb *row = reinterpret_cast<QRgb *>(bits + y * bpl);
        for (int x = 0; x < w; ++x)
            row[x] = f(row[x]);
    });
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

template <typename F>
QImage mapLut(const QImage &src, F channelFn)
{
    uchar lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = uchar(clamp255(channelFn(i)));
    return mapPixels(src, [&](QRgb p) {
        return qRgba(lut[qRed(p)], lut[qGreen(p)], lut[qBlue(p)], qAlpha(p));
    });
}

inline int luma(QRgb p) { return (qRed(p) * 299 + qGreen(p) * 587 + qBlue(p) * 114) / 1000; }

void rgbToHsl(int r, int g, int b, float &h, float &s, float &l)
{
    const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    const float mx = std::max({rf, gf, bf}), mn = std::min({rf, gf, bf});
    l = (mx + mn) / 2;
    if (mx == mn) {
        h = s = 0;
        return;
    }
    const float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == rf)
        h = (gf - bf) / d + (gf < bf ? 6 : 0);
    else if (mx == gf)
        h = (bf - rf) / d + 2;
    else
        h = (rf - gf) / d + 4;
    h /= 6;
}

float hueToRgb(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6.0f) return p + (q - p) * 6 * t;
    if (t < 1 / 2.0f) return q;
    if (t < 2 / 3.0f) return p + (q - p) * (2 / 3.0f - t) * 6;
    return p;
}

void hslToRgb(float h, float s, float l, int &r, int &g, int &b)
{
    if (s == 0) {
        r = g = b = int(std::lround(l * 255));
        return;
    }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    r = int(std::lround(hueToRgb(p, q, h + 1 / 3.0f) * 255));
    g = int(std::lround(hueToRgb(p, q, h) * 255));
    b = int(std::lround(hueToRgb(p, q, h - 1 / 3.0f) * 255));
}

// Squared Euclidean distance transform, one dimension (Felzenszwalb & Huttenlocher).
void distance1d(const double *f, double *d, int n, int *v, double *z)
{
    constexpr double inf = 1e20;
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

// Squared distance from every pixel to the nearest pixel where `feature` is true.
std::vector<double> squaredDistance(const std::vector<char> &feature, int w, int h)
{
    std::vector<double> grid(size_t(w) * h);
    for (size_t i = 0; i < grid.size(); ++i)
        grid[i] = feature[i] ? 0.0 : 1e20;
    parallelFor(w, [&](int x) {
        std::vector<double> f(h), d(h), z(h + 1);
        std::vector<int> v(h);
        for (int y = 0; y < h; ++y)
            f[y] = grid[size_t(y) * w + x];
        distance1d(f.data(), d.data(), h, v.data(), z.data());
        for (int y = 0; y < h; ++y)
            grid[size_t(y) * w + x] = d[y];
    });
    parallelFor(h, [&](int y) {
        std::vector<double> d(w), z(w + 1);
        std::vector<int> v(w);
        double *row = &grid[size_t(y) * w];
        distance1d(row, d.data(), w, v.data(), z.data());
        std::copy(d.begin(), d.end(), row);
    });
    return grid;
}

// Gaussian blur (three box passes) of an interleaved float buffer with 4 channels.
void blurFloat(std::vector<float> &buf, int w, int h, double sigma)
{
    const int n = 3;
    const double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0)
        --wl;
    const int wu = wl + 2;
    const int m = int(std::lround((12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0)));
    std::vector<float> tmp(buf.size());
    auto pass = [&](const std::vector<float> &src, std::vector<float> &dst, int r, bool horizontal) {
        const int lines = horizontal ? h : w, len = horizontal ? w : h;
        const float div = float(2 * r + 1);
        for (int line = 0; line < lines; ++line) {
            auto idx = [&](int i) {
                i = std::clamp(i, 0, len - 1);
                return 4 * (horizontal ? line * w + i : i * w + line);
            };
            float sum[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k)
                for (int c = 0; c < 4; ++c)
                    sum[c] += src[idx(k) + c];
            for (int i = 0; i < len; ++i) {
                const int o = idx(i), out = idx(i - r), in = idx(i + r + 1);
                for (int c = 0; c < 4; ++c) {
                    dst[o + c] = sum[c] / div;
                    sum[c] += src[in + c] - src[out + c];
                }
            }
        }
    };
    for (int i = 0; i < n; ++i) {
        const int r = ((i < m ? wl : wu) - 1) / 2;
        pass(buf, tmp, r, true);
        pass(tmp, buf, r, false);
    }
}

// One pass of a box blur over premultiplied pixels. `get(line, i)` addresses
// the i-th pixel of a line, so the same code handles rows and columns.
template <typename Get, typename Put>
void boxLine(int length, int r, Get get, Put put)
{
    const int div = 2 * r + 1;
    int sa = 0, sr = 0, sg = 0, sb = 0;
    auto add = [&](QRgb c, int sign) {
        sa += sign * qAlpha(c);
        sr += sign * qRed(c);
        sg += sign * qGreen(c);
        sb += sign * qBlue(c);
    };
    const int last = length - 1;
    for (int k = -r; k <= r; ++k)
        add(get(std::clamp(k, 0, last)), 1);
    for (int i = 0; i < length; ++i) {
        put(i, qRgba((sr + div / 2) / div, (sg + div / 2) / div, (sb + div / 2) / div, (sa + div / 2) / div));
        add(get(std::clamp(i - r, 0, last)), -1);
        add(get(std::clamp(i + r + 1, 0, last)), 1);
    }
}

void boxBlur(QImage &img, QImage &tmp, int r)
{
    const int w = img.width(), h = img.height();
    uchar *a = img.bits();
    uchar *b = tmp.bits();
    const qsizetype bpl = img.bytesPerLine();
    parallelFor(h, [&](int y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(a + y * bpl);
        QRgb *d = reinterpret_cast<QRgb *>(b + y * bpl);
        boxLine(w, r, [&](int i) { return s[i]; }, [&](int i, QRgb v) { d[i] = v; });
    });
    parallelFor(w, [&](int x) {
        boxLine(h, r,
                [&](int i) { return reinterpret_cast<const QRgb *>(b + i * bpl)[x]; },
                [&](int i, QRgb v) { reinterpret_cast<QRgb *>(a + i * bpl)[x] = v; });
    });
}

} // namespace

namespace Filters {

QImage invert(const QImage &src)
{
    return mapPixels(src, [](QRgb p) { return p ^ 0x00ffffffu; });
}

QImage desaturate(const QImage &src)
{
    return mapPixels(src, [](QRgb p) {
        const int l = luma(p);
        return qRgba(l, l, l, qAlpha(p));
    });
}

QImage brightnessContrast(const QImage &src, int brightness, int contrast)
{
    const double c = contrast * 2.55;
    const double factor = (259.0 * (c + 255.0)) / (255.0 * (259.0 - c));
    return mapLut(src, [&](int v) { return int(std::lround(factor * (v + brightness - 128) + 128)); });
}

QImage brightnessContrastLegacy(const QImage &src, int brightness, int contrast)
{
    const double c = std::clamp(contrast, -100, 99) / 100.0;
    const double slope = c >= 0 ? 1.0 / (1.0 - c) : 1.0 + c;
    return mapLut(src, [&](int v) { return int(std::lround((v + brightness - 127.5) * slope + 127.5)); });
}

QImage hueSaturation(const QImage &src, int hue, int saturation, int lightness, const QList<int> &ranges)
{
    // How strongly a hue (degrees) falls in a color range: 1 between the inner limits b..c,
    // fading to 0 at the outer limits a and d. Ranges may wrap around 360.
    auto weight = [](float hd, int a, int b, int c, int d) {
        auto dist = [](float from, float to) { return std::fmod(to - from + 720.0f, 360.0f); };
        const float t = dist(a, hd), ab = dist(a, b), ac = dist(a, c), ad = dist(a, d);
        if (t <= ab)
            return ab > 0 ? t / ab : 1.0f;
        if (t <= ac)
            return 1.0f;
        if (t <= ad)
            return ad > ac ? (ad - t) / (ad - ac) : 0.0f;
        return 0.0f;
    };
    const int rangeCount = int(ranges.size() / 7);
    return mapPixels(src, [&](QRgb p) {
        if (qAlpha(p) == 0)
            return p;
        float h, s, l;
        rgbToHsl(qRed(p), qGreen(p), qBlue(p), h, s, l);
        // Color ranges add their hue and saturation changes, weighted by how much the
        // pixel's hue belongs to them (compared with Photoshop's output, this works best).
        float dh = hue, ds = saturation;
        for (int k = 0; k < rangeCount && s > 0; ++k) {
            const int *v = ranges.constData() + 7 * k;
            const float w = weight(h * 360.0f, v[0], v[1], v[2], v[3]);
            dh += w * v[4];
            ds += w * v[5];
        }
        h = std::fmod(h + dh / 360.0f + 2.0f, 1.0f);
        ds = std::clamp(ds / 100.0f, -1.0f, 1.0f);
        s = ds >= 0 ? s + (1 - s) * ds : s * (1 + ds);
        int r, g, b;
        hslToRgb(h, std::clamp(s, 0.0f, 1.0f), l, r, g, b);
        // Lightness blends each channel toward white or black, as Photoshop does.
        const float dl = std::clamp(lightness / 100.0f, -1.0f, 1.0f);
        auto light = [dl](int v) { return clamp255(int(std::lround(dl >= 0 ? v + (255 - v) * dl : v * (1 + dl)))); };
        return qRgba(light(r), light(g), light(b), qAlpha(p));
    });
}

QImage threshold(const QImage &src, int level)
{
    return mapPixels(src, [&](QRgb p) {
        const int v = luma(p) >= level ? 255 : 0;
        return qRgba(v, v, v, qAlpha(p));
    });
}

QImage posterize(const QImage &src, int levels)
{
    // Photoshop's formula: split 0..255 into `levels` equal bands.
    const int n = std::max(2, levels);
    return mapLut(src, [&](int v) { return (v * n / 256) * 255 / (n - 1); });
}

QImage gaussianBlur(const QImage &src, double radius)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (radius < 0.5 || img.isNull())
        return img;
    img.detach();
    QImage tmp(img.size(), img.format());

    // Three box blurs approximate a gaussian with sigma = radius.
    const int n = 3;
    const double sigma = radius;
    const double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0)
        --wl;
    const int wu = wl + 2;
    const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const int m = int(std::lround(mIdeal));
    for (int i = 0; i < n; ++i)
        boxBlur(img, tmp, ((i < m ? wl : wu) - 1) / 2);
    return img;
}

QImage unsharpMask(const QImage &src, int amountPercent, double radius, int thresholdLevel)
{
    const QImage orig = src.convertToFormat(QImage::Format_ARGB32);
    const QImage blur = gaussianBlur(src, radius).convertToFormat(QImage::Format_ARGB32);
    QImage out(orig.size(), QImage::Format_ARGB32);
    const double amount = amountPercent / 100.0;
    uchar *ob = out.bits();
    parallelFor(orig.height(), [&](int y) {
        const QRgb *o = reinterpret_cast<const QRgb *>(orig.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(blur.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(ob + y * out.bytesPerLine());
        for (int x = 0; x < orig.width(); ++x) {
            auto ch = [&](int ov, int bv) {
                const int diff = ov - bv;
                return std::abs(diff) < thresholdLevel ? ov : clamp255(int(std::lround(ov + diff * amount)));
            };
            d[x] = qRgba(ch(qRed(o[x]), qRed(b[x])), ch(qGreen(o[x]), qGreen(b[x])),
                         ch(qBlue(o[x]), qBlue(b[x])), qAlpha(o[x]));
        }
    });
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage addNoise(const QImage &src, int amountPercent)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    uchar *bits = img.bits();
    const int amp = std::max(1, int(amountPercent * 2.55));
    parallelFor(img.height(), [&](int y) {
        QRandomGenerator rng(quint32(y) * 2654435761u + 12345u);  // deterministic, so previews are stable
        QRgb *row = reinterpret_cast<QRgb *>(bits + y * img.bytesPerLine());
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = row[x];
            if (qAlpha(p) == 0)
                continue;
            row[x] = qRgba(clamp255(qRed(p) + rng.bounded(-amp, amp + 1)),
                           clamp255(qGreen(p) + rng.bounded(-amp, amp + 1)),
                           clamp255(qBlue(p) + rng.bounded(-amp, amp + 1)), qAlpha(p));
        }
    });
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage pixelate(const QImage &src, int cell)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (cell < 2)
        return img;
    img.detach();
    uchar *bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    const int w = img.width(), h = img.height();
    parallelFor((h + cell - 1) / cell, [&](int by) {
        const int y0 = by * cell, y1 = std::min(h, y0 + cell);
        for (int x0 = 0; x0 < w; x0 += cell) {
            const int x1 = std::min(w, x0 + cell);
            quint64 a = 0, r = 0, g = 0, b = 0;
            for (int y = y0; y < y1; ++y) {
                const QRgb *row = reinterpret_cast<const QRgb *>(bits + y * bpl);
                for (int x = x0; x < x1; ++x) {
                    a += qAlpha(row[x]); r += qRed(row[x]); g += qGreen(row[x]); b += qBlue(row[x]);
                }
            }
            const quint64 n = quint64(y1 - y0) * quint64(x1 - x0);
            const QRgb avg = qRgba(int(r / n), int(g / n), int(b / n), int(a / n));
            for (int y = y0; y < y1; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(bits + y * bpl);
                std::fill(row + x0, row + x1, avg);
            }
        }
    });
    return img;
}

QImage dodgeBurn(const QImage &src, bool burn, int range)
{
    return mapPixels(src, [&](QRgb p) {
        if (qAlpha(p) == 0)
            return p;
        const float l = luma(p) / 255.0f;
        const float w = range == 0 ? (1 - l) * (1 - l) : range == 2 ? l * l : 1 - (2 * l - 1) * (2 * l - 1);
        const float k = 0.6f * w;
        auto ch = [&](int c) { return clamp255(int(std::lround(burn ? c - c * k : c + (255 - c) * k))); };
        return qRgba(ch(qRed(p)), ch(qGreen(p)), ch(qBlue(p)), qAlpha(p));
    });
}

QImage levels(const QImage &src, int inBlack, int inWhite, double gamma, int outBlack, int outWhite)
{
    inWhite = std::max(inWhite, inBlack + 1);
    gamma = std::max(0.01, gamma);
    return mapLut(src, [&](int v) {
        const double t = std::clamp((v - inBlack) / double(inWhite - inBlack), 0.0, 1.0);
        return int(std::lround(outBlack + std::pow(t, 1.0 / gamma) * (outWhite - outBlack)));
    });
}

QImage applyLuts(const QImage &src, const QList<int> &red, const QList<int> &green, const QList<int> &blue)
{
    return mapPixels(src, [&](QRgb p) {
        return qRgba(red[qRed(p)], green[qGreen(p)], blue[qBlue(p)], qAlpha(p));
    });
}

QList<int> levelsLut(int inBlack, int inWhite, double gamma, int outBlack, int outWhite)
{
    inWhite = std::max(inWhite, inBlack + 1);
    gamma = std::max(0.01, gamma);
    QList<int> lut(256);
    for (int v = 0; v < 256; ++v) {
        const double t = std::clamp((v - inBlack) / double(inWhite - inBlack), 0.0, 1.0);
        lut[v] = clamp255(int(std::lround(outBlack + std::pow(t, 1.0 / gamma) * (outWhite - outBlack))));
    }
    return lut;
}

// Natural cubic spline through the points (how Photoshop draws curves); may overshoot,
// so the result is clamped.
QList<int> splineLut(std::vector<std::pair<double, double>> pts)
{
    const size_t n = pts.size();
    std::vector<double> h(n - 1), alpha(n, 0), l(n, 1), mu(n, 0), z(n, 0), c(n, 0), b(n - 1), d(n - 1);
    for (size_t i = 0; i + 1 < n; ++i)
        h[i] = pts[i + 1].first - pts[i].first;
    for (size_t i = 1; i + 1 < n; ++i)
        alpha[i] = 3 / h[i] * (pts[i + 1].second - pts[i].second) - 3 / h[i - 1] * (pts[i].second - pts[i - 1].second);
    for (size_t i = 1; i + 1 < n; ++i) {
        l[i] = 2 * (pts[i + 1].first - pts[i - 1].first) - h[i - 1] * mu[i - 1];
        mu[i] = h[i] / l[i];
        z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
    }
    for (size_t j = n - 1; j-- > 0;) {
        c[j] = z[j] - mu[j] * c[j + 1];
        b[j] = (pts[j + 1].second - pts[j].second) / h[j] - h[j] * (c[j + 1] + 2 * c[j]) / 3;
        d[j] = (c[j + 1] - c[j]) / (3 * h[j]);
    }
    QList<int> lut(256);
    size_t k = 0;
    for (int x = 0; x < 256; ++x) {
        double y;
        if (x <= pts.front().first) {
            y = pts.front().second;
        } else if (x >= pts.back().first) {
            y = pts.back().second;
        } else {
            while (x > pts[k + 1].first)
                ++k;
            const double t = x - pts[k].first;
            y = pts[k].second + b[k] * t + c[k] * t * t + d[k] * t * t * t;
        }
        lut[x] = clamp255(int(std::lround(y)));
    }
    return lut;
}

QList<int> curveLut(const QList<int> &points)
{
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i + 1 < points.size(); i += 2)
        pts.emplace_back(std::clamp(points[i], 0, 255), std::clamp(points[i + 1], 0, 255));
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(), [](auto &a, auto &b) { return a.first == b.first; }), pts.end());
    QList<int> lut(256);
    if (pts.size() < 2) {
        std::iota(lut.begin(), lut.end(), 0);
        return lut;
    }
    if (pts.size() >= 3)
        return splineLut(pts);
    // Two points: a straight line.
    const size_t n = pts.size();
    std::vector<double> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i)
        d[i] = (pts[i + 1].second - pts[i].second) / (pts[i + 1].first - pts[i].first);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    size_t k = 0;
    for (int x = 0; x < 256; ++x) {
        double y;
        if (x <= pts.front().first) {
            y = pts.front().second;
        } else if (x >= pts.back().first) {
            y = pts.back().second;
        } else {
            while (x > pts[k + 1].first)
                ++k;
            const double h = pts[k + 1].first - pts[k].first, t = (x - pts[k].first) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[k].second + (t3 - 2 * t2 + t) * h * m[k]
              + (-2 * t3 + 3 * t2) * pts[k + 1].second + (t3 - t2) * h * m[k + 1];
        }
        lut[x] = clamp255(int(std::lround(y)));
    }
    return lut;
}

QImage curves(const QImage &src, const QList<int> &points)
{
    const QList<int> lut = curveLut(points);
    return mapLut(src, [&](int v) { return lut[v]; });
}

QList<int> histogram(const QImage &src)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    QList<int> bins(256, 0);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(row[x]))
                ++bins[luma(row[x])];
    }
    return bins;
}

QImage heal(const QImage &source, const QImage &dest, const QImage &mask, double sigma)
{
    const QImage S = source.convertToFormat(QImage::Format_ARGB32);
    const QImage D = dest.convertToFormat(QImage::Format_ARGB32);
    const int w = S.width(), h = S.height();

    // Normalized convolution: blur color weighted by "outside the stroke", so the
    // low frequencies inside the stroke are interpolated from its surroundings.
    auto lowFrequency = [&](const QImage &img) {
        std::vector<float> buf(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            const uchar *m = mask.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                const float wt = (255 - m[x]) / 255.0f * (qAlpha(row[x]) / 255.0f);
                float *o = &buf[(size_t(y) * w + x) * 4];
                o[0] = qRed(row[x]) * wt;
                o[1] = qGreen(row[x]) * wt;
                o[2] = qBlue(row[x]) * wt;
                o[3] = wt;
            }
        }
        blurFloat(buf, w, h, sigma);
        return buf;
    };
    const std::vector<float> lowS = lowFrequency(S), lowD = lowFrequency(D);

    QImage out(S.size(), QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(S.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float *ls = &lowS[(size_t(y) * w + x) * 4];
            const float *ld = &lowD[(size_t(y) * w + x) * 4];
            if (ls[3] < 1e-4f || ld[3] < 1e-4f) {
                o[x] = s[x];
                continue;
            }
            auto ch = [&](int v, int c) { return clamp255(int(std::lround(v + ld[c] / ld[3] - ls[c] / ls[3]))); };
            o[x] = qRgba(ch(qRed(s[x]), 0), ch(qGreen(s[x]), 1), ch(qBlue(s[x]), 2), qAlpha(s[x]));
        }
    }
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage morphMask(const QImage &src, double radius)
{
    const QImage m = src.convertToFormat(QImage::Format_Alpha8);
    const int w = m.width(), h = m.height();
    std::vector<char> inside(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar *row = m.constScanLine(y);
        for (int x = 0; x < w; ++x)
            inside[size_t(y) * w + x] = row[x] >= 128;
    }
    const bool grow = radius >= 0;
    const double r = std::abs(radius);
    std::vector<char> feature = inside;
    if (!grow)
        for (char &c : feature)
            c = !c;
    const std::vector<double> dist = squaredDistance(feature, w, h);

    QImage out(m.size(), QImage::Format_Alpha8);
    for (int y = 0; y < h; ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const double d = std::sqrt(dist[i]);
            double coverage;
            // d is measured between pixel centres; the selection edge lies half a pixel
            // from the outermost centre, so a pixel is fully covered when it is at
            // least one pixel inside the new edge.
            if (grow)
                coverage = inside[i] ? 1.0 : r - d + 1.0;   // d = distance to the selection
            else
                coverage = inside[i] ? d - r : 0.0;         // d = distance to the outside
            row[x] = uchar(std::lround(std::clamp(coverage, 0.0, 1.0) * 255));
        }
    }
    return out;
}

QImage featherMask(const QImage &mask, double radius)
{
    // Alpha8 converts to black pixels carrying the mask as alpha, which blurs correctly.
    return gaussianBlur(mask.convertToFormat(QImage::Format_ARGB32_Premultiplied), radius)
        .convertToFormat(QImage::Format_Alpha8);
}

QImage colorRangeMask(const QImage &src, const QColor &color, int fuzziness)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    QImage out(img.size(), QImage::Format_Alpha8);
    const int cr = color.red(), cg = color.green(), cb = color.blue();
    const double full = fuzziness / 2.0, falloff = std::max(1.0, fuzziness / 2.0);
    uchar *bits = out.bits();
    const qsizetype bpl = out.bytesPerLine();
    parallelFor(img.height(), [&](int y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        uchar *o = bits + y * bpl;
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = row[x];
            const int dist = std::max({std::abs(qRed(p) - cr), std::abs(qGreen(p) - cg), std::abs(qBlue(p) - cb)});
            const double c = std::clamp(1.0 - (dist - full) / falloff, 0.0, 1.0);  // fully selected up to fuzziness/2
            o[x] = uchar(std::lround(c * qAlpha(p)));
        }
    });
    return out;
}

QImage floodMask(const QImage &src, const QPoint &seed, int tolerance, bool contiguous)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = img.width(), h = img.height();
    QImage mask(img.size(), QImage::Format_Alpha8);
    mask.fill(0);
    if (!img.rect().contains(seed))
        return mask;

    const QRgb target = reinterpret_cast<const QRgb *>(img.constScanLine(seed.y()))[seed.x()];
    auto match = [&](QRgb c) {
        return std::abs(qRed(c) - qRed(target)) <= tolerance
            && std::abs(qGreen(c) - qGreen(target)) <= tolerance
            && std::abs(qBlue(c) - qBlue(target)) <= tolerance
            && std::abs(qAlpha(c) - qAlpha(target)) <= tolerance;
    };
    auto pixels = [&](int y) { return reinterpret_cast<const QRgb *>(img.constScanLine(y)); };

    if (!contiguous) {
        for (int y = 0; y < h; ++y) {
            const QRgb *row = pixels(y);
            uchar *m = mask.scanLine(y);
            for (int x = 0; x < w; ++x)
                if (match(row[x]))
                    m[x] = 255;
        }
        return mask;
    }

    // Scanline flood fill.
    std::vector<QPoint> stack{seed};
    while (!stack.empty()) {
        const QPoint p = stack.back();
        stack.pop_back();
        const int y = p.y();
        const QRgb *row = pixels(y);
        uchar *m = mask.scanLine(y);
        if (m[p.x()] || !match(row[p.x()]))
            continue;
        int x0 = p.x(), x1 = p.x();
        while (x0 > 0 && !m[x0 - 1] && match(row[x0 - 1]))
            --x0;
        while (x1 < w - 1 && !m[x1 + 1] && match(row[x1 + 1]))
            ++x1;
        std::fill(m + x0, m + x1 + 1, uchar(255));
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            const QRgb *nrow = pixels(ny);
            const uchar *nm = mask.constScanLine(ny);
            bool inSpan = false;
            for (int x = x0; x <= x1; ++x) {
                const bool ok = !nm[x] && match(nrow[x]);
                if (ok && !inSpan)
                    stack.emplace_back(x, ny);
                inSpan = ok;
            }
        }
    }
    return mask;
}

} // namespace Filters
