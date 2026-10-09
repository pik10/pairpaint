// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Blending.h"

#include <QList>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <numeric>

namespace {

template <typename F>
void parallelFor(int count, F f)
{
    QList<int> items(count);
    std::iota(items.begin(), items.end(), 0);
    QtConcurrent::blockingMap(items, [&](const int &i) { f(i); });
}

struct Rgb {
    float r, g, b;
};

inline float lum(const Rgb &c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

inline Rgb clipColor(Rgb c)
{
    const float l = lum(c);
    const float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    auto scale = [&](float k) { c = {l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k}; };
    if (n < 0)
        scale(l / (l - n));
    if (x > 1)
        scale((1 - l) / (x - l));
    return c;
}

inline Rgb setLum(const Rgb &c, float l)
{
    const float d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}

inline float sat(const Rgb &c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

inline Rgb setSat(Rgb c, float s)
{
    float *v[3] = {&c.r, &c.g, &c.b};
    std::sort(v, v + 3, [](float *a, float *b) { return *a < *b; });  // v[0] min, v[2] max
    if (*v[2] > *v[0]) {
        *v[1] = (*v[1] - *v[0]) * s / (*v[2] - *v[0]);
        *v[2] = s;
    } else {
        *v[1] = *v[2] = 0;
    }
    *v[0] = 0;
    return c;
}

inline float colorBurn(float b, float s)
{
    if (b >= 1) return 1;
    if (s <= 0) return 0;
    return 1 - std::min(1.0f, (1 - b) / s);
}

inline float colorDodge(float b, float s)
{
    if (b <= 0) return 0;
    if (s >= 1) return 1;
    return std::min(1.0f, b / (1 - s));
}

// Separable blend function: backdrop b, source s, both 0..1.
inline float blendChannel(QPainter::CompositionMode mode, float b, float s)
{
    switch (int(mode)) {
    case int(Blend::LinearBurn): return std::max(0.0f, b + s - 1);
    case int(Blend::VividLight):
        // Photoshop decides by the top layer first: pure black gives black, pure white gives white.
        if (s <= 0.5f)
            return s <= 0 ? 0.0f : std::max(0.0f, 1 - (1 - b) / (2 * s));
        return s >= 1 ? 1.0f : std::min(1.0f, b / (2 * (1 - s)));
    case int(Blend::LinearLight): return std::clamp(b + 2 * s - 1, 0.0f, 1.0f);
    case int(Blend::PinLight): return s <= 0.5f ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    case int(Blend::HardMix):
        // Photoshop thresholds the textbook Vivid Light (which checks the bottom layer first).
        return (s <= 0.5f ? colorBurn(b, 2 * s) : colorDodge(b, 2 * s - 1)) >= 0.5f ? 1.0f : 0.0f;
    case int(Blend::Subtract): return std::max(0.0f, b - s);
    case int(Blend::Divide): return s <= 0 ? (b > 0 ? 1.0f : 0.0f) : std::min(1.0f, b / s);
    case int(QPainter::CompositionMode_Plus): return std::min(1.0f, b + s);  // Linear Dodge (Add)
    default: return s;
    }
}

inline Rgb blendColor(QPainter::CompositionMode mode, const Rgb &b, const Rgb &s)
{
    switch (int(mode)) {
    case int(Blend::Hue): return setLum(setSat(s, sat(b)), lum(b));
    case int(Blend::Saturation): return setLum(setSat(b, sat(s)), lum(b));
    case int(Blend::Color): return setLum(s, lum(b));
    case int(Blend::Luminosity): return setLum(b, lum(s));
    case int(Blend::DarkerColor): return lum(s) < lum(b) ? s : b;
    case int(Blend::LighterColor): return lum(s) > lum(b) ? s : b;
    case int(Blend::Dissolve): return s;
    default: return {blendChannel(mode, b.r, s.r), blendChannel(mode, b.g, s.g), blendChannel(mode, b.b, s.b)};
    }
}

// Stable per-pixel noise in [0, 1) for Dissolve.
inline float noise(int x, int y)
{
    quint32 h = quint32(x) * 374761393u + quint32(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffff) / float(0x1000000);
}

inline int maskValue(QRgb p) { return (qRed(p) * 77 + qGreen(p) * 150 + qBlue(p) * 29) >> 8; }

} // namespace

namespace Blend {

bool isCustom(QPainter::CompositionMode mode)
{
    // Qt's Plus also adds the alphas; Photoshop's Linear Dodge blends like the other modes.
    return int(mode) >= 1000 || mode == QPainter::CompositionMode_Plus;
}

void draw(QImage &dst, const QImage &src, QPainter::CompositionMode mode, qreal opacity, const QPoint &origin)
{
    if (opacity <= 0.0)
        return;
    if (!isCustom(mode) || mode == PassThrough) {
        QPainter p(&dst);
        p.setOpacity(opacity);
        p.setCompositionMode(mode == PassThrough ? QPainter::CompositionMode_SourceOver : mode);
        p.drawImage(0, 0, src);
        return;
    }
    const QImage s = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    uchar *bits = dst.bits();
    const qsizetype bpl = dst.bytesPerLine();
    const float op = float(opacity);
    parallelFor(dst.height(), [&](int y) {
        QRgb *d = reinterpret_cast<QRgb *>(bits + y * bpl);
        const QRgb *sp = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        for (int x = 0; x < dst.width(); ++x) {
            const QRgb sv = sp[x];
            if (qAlpha(sv) == 0)
                continue;
            float sa = qAlpha(sv) / 255.0f * op;
            if (mode == Dissolve) {
                if (noise(origin.x() + x, origin.y() + y) >= sa)
                    continue;
                sa = 1.0f;  // dissolved pixels are either fully shown or not at all
            }
            const QRgb dv = d[x];
            const float da = qAlpha(dv) / 255.0f;
            const float sk = 1.0f / qAlpha(sv);
            const Rgb cs{qRed(sv) * sk, qGreen(sv) * sk, qBlue(sv) * sk};
            const float dk = qAlpha(dv) ? 1.0f / qAlpha(dv) : 0.0f;
            const Rgb cb{qRed(dv) * dk, qGreen(dv) * dk, qBlue(dv) * dk};
            const Rgb bl = blendColor(mode, cb, cs);
            // Standard separable compositing (W3C Compositing and Blending), premultiplied result.
            const float ra = sa + da - sa * da;
            auto ch = [&](float s, float b, float m) {
                return std::clamp(int(((1 - da) * sa * s + (1 - sa) * da * b + sa * da * m) * 255.0f + 0.5f), 0, 255);
            };
            const int a = std::clamp(int(ra * 255.0f + 0.5f), 0, 255);
            d[x] = qRgba(std::min(ch(cs.r, cb.r, bl.r), a), std::min(ch(cs.g, cb.g, bl.g), a),
                         std::min(ch(cs.b, cb.b, bl.b), a), a);
        }
    });
}

void mix(QImage &dst, const QImage &src, qreal opacity, const QImage &mask, const QRect &area)
{
    const int op = qRound(opacity * 255);
    for (int y = 0; y < dst.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(dst.scanLine(y));
        const QRgb *a = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const QRgb *m = mask.isNull() ? nullptr
                                      : reinterpret_cast<const QRgb *>(mask.constScanLine(area.top() + y)) + area.left();
        for (int x = 0; x < dst.width(); ++x) {
            const int k = m ? op * maskValue(m[x]) / 255 : op;
            if (k == 0)
                continue;
            if (k == 255) {
                o[x] = a[x];
                continue;
            }
            const QRgb p = o[x], q = a[x];
            auto lerp = [k](int u, int v) { return u + (v - u) * k / 255; };
            o[x] = qRgba(lerp(qRed(p), qRed(q)), lerp(qGreen(p), qGreen(q)), lerp(qBlue(p), qBlue(q)),
                         lerp(qAlpha(p), qAlpha(q)));
        }
    }
}

void mixByAlpha(QImage &dst, const QImage &src, const QImage &coverage)
{
    for (int y = 0; y < dst.height(); ++y) {
        QRgb *o = reinterpret_cast<QRgb *>(dst.scanLine(y));
        const QRgb *a = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const QRgb *c = reinterpret_cast<const QRgb *>(coverage.constScanLine(y));
        for (int x = 0; x < dst.width(); ++x) {
            const int k = qAlpha(c[x]);
            if (k == 0)
                continue;
            const QRgb p = o[x], q = a[x];
            auto lerp = [k](int u, int v) { return u + (v - u) * k / 255; };
            o[x] = qRgba(lerp(qRed(p), qRed(q)), lerp(qGreen(p), qGreen(q)), lerp(qBlue(p), qBlue(q)),
                         lerp(qAlpha(p), qAlpha(q)));
        }
    }
}

void restoreAlpha(QImage &img, const QImage &alphaFrom)
{
    for (int y = 0; y < img.height(); ++y) {
        QRgb *p = reinterpret_cast<QRgb *>(img.scanLine(y));
        const QRgb *f = reinterpret_cast<const QRgb *>(alphaFrom.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int want = qAlpha(f[x]), have = qAlpha(p[x]);
            if (want == have)
                continue;
            if (have == 0 || want == 0) {
                p[x] = 0;
                continue;
            }
            const QRgb c = p[x];
            p[x] = qRgba(qRed(c) * want / have, qGreen(c) * want / have, qBlue(c) * want / have, want);
        }
    }
}

} // namespace Blend
