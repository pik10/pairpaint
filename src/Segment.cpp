// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

// Quick Selection: seeded region growing (Adams & Bischof, 1994) from the pixels painted over,
// judged against the painted colors (summarized by k-means clustering, Lloyd 1957/1982) and
// stopped by edges.

#include "Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <random>
#include <vector>

namespace {

using Color = std::array<float, 3>;

inline Color colorOf(QRgb p) { return {float(qRed(p)), float(qGreen(p)), float(qBlue(p))}; }

inline float distance2(const Color &a, const Color &b)
{
    const float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// A few representative colors of `samples` (k-means).
std::vector<Color> clusters(std::vector<Color> samples, int k)
{
    std::mt19937 rng(7);
    if (samples.size() > 3000) {
        std::shuffle(samples.begin(), samples.end(), rng);
        samples.resize(3000);
    }
    k = std::min<int>(k, int(samples.size()));
    std::vector<Color> centers;
    // Spread-out starting points: each next one the sample farthest from those chosen.
    centers.push_back(samples[0]);
    std::vector<float> nearest(samples.size(), std::numeric_limits<float>::max());
    while (int(centers.size()) < k) {
        size_t far = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            nearest[i] = std::min(nearest[i], distance2(samples[i], centers.back()));
            if (nearest[i] > nearest[far])
                far = i;
        }
        if (nearest[far] < 1)
            break;  // fewer distinct colors than k
        centers.push_back(samples[far]);
    }
    std::vector<std::array<double, 4>> sum;
    for (int iteration = 0; iteration < 8; ++iteration) {
        sum.assign(centers.size(), {});
        for (const Color &s : samples) {
            size_t best = 0;
            for (size_t c = 1; c < centers.size(); ++c)
                if (distance2(s, centers[c]) < distance2(s, centers[best]))
                    best = c;
            for (int i = 0; i < 3; ++i)
                sum[best][size_t(i)] += s[size_t(i)];
            sum[best][3] += 1;
        }
        for (size_t c = 0; c < centers.size(); ++c)
            if (sum[c][3] > 0)
                centers[c] = {float(sum[c][0] / sum[c][3]), float(sum[c][1] / sum[c][3]), float(sum[c][2] / sum[c][3])};
    }
    // Colors only grazed by the brush (the background at an object's edge, a gap between leaves)
    // aren't the object's.
    std::vector<Color> kept;
    for (size_t c = 0; c < centers.size(); ++c)
        if (sum[c][3] >= 0.08 * samples.size())
            kept.push_back(centers[c]);
    return kept.empty() ? centers : kept;
}

} // namespace

namespace Filters {

QImage quickSelect(const QImage &image, const QImage &painted, double radius)
{
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const QImage seeds = painted.convertToFormat(QImage::Format_Alpha8);
    QImage result(img.size(), QImage::Format_Alpha8);
    result.fill(0);

    // The painted pixels and their colors; the growth stays within a reach of them.
    QRect painted_;
    std::vector<Color> samples;
    for (int y = 0; y < seeds.height(); ++y) {
        const uchar *s = seeds.constScanLine(y);
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < seeds.width(); ++x) {
            if (s[x]) {
                painted_ |= QRect(x, y, 1, 1);
                samples.push_back(colorOf(row[x]));
            }
        }
    }
    if (samples.empty())
        return result;
    const int reach = int(std::max({400.0, radius * 12, std::max(img.width(), img.height()) / 3.0}));
    const QRect area = painted_.adjusted(-reach, -reach, reach, reach) & img.rect();

    // How far the painted colors spread from their representatives sets how alike a pixel must be.
    const std::vector<Color> centers = clusters(samples, 6);
    auto nearest = [&](const Color &c) {
        float best = std::numeric_limits<float>::max();
        for (const Color &k : centers)
            best = std::min(best, distance2(c, k));
        return best;
    };
    std::vector<float> spread;
    spread.reserve(std::min<size_t>(samples.size(), 3000));
    for (size_t i = 0; i < samples.size(); i += std::max<size_t>(1, samples.size() / 3000))
        spread.push_back(std::sqrt(nearest(samples[i])));
    std::nth_element(spread.begin(), spread.begin() + spread.size() * 9 / 10, spread.end());
    const float like = std::clamp(spread[spread.size() * 9 / 10] * 1.5f + 12.0f, 16.0f, 70.0f);
    const float like2 = like * like;
    const float loose2 = 2.5f * 2.5f * like2;   // along smooth gradients, a pixel may drift this far
    const float step2 = 0.35f * 0.35f * like2;  // a smooth step between neighbors; more is an edge

    // Grow from the painted pixels: a neighbor joins if its color is like the painted ones, or
    // if it is a smooth continuation (no edge) of a pixel already in and still roughly alike.
    const int w = area.width(), h = area.height();
    std::vector<uchar> in(size_t(w) * h);
    std::deque<int> queue;
    auto colorAt = [&](int x, int y) {
        return colorOf(reinterpret_cast<const QRgb *>(img.constScanLine(area.top() + y))[area.left() + x]);
    };
    for (int y = 0; y < h; ++y) {
        const uchar *s = seeds.constScanLine(area.top() + y) + area.left();
        for (int x = 0; x < w; ++x)
            if (s[x]) {
                in[size_t(y) * w + x] = 1;
                queue.push_back(y * w + x);
            }
    }
    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        const int x = i % w, y = i / w;
        const Color c = colorAt(x, y);
        for (const auto &[dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || in[size_t(ny) * w + nx])
                continue;
            const Color n = colorAt(nx, ny);
            const float d = nearest(n);
            if (d <= like2 || (d <= loose2 && distance2(c, n) <= step2)) {
                in[size_t(ny) * w + nx] = 1;
                queue.push_back(ny * w + nx);
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        uchar *r = result.scanLine(area.top() + y) + area.left();
        for (int x = 0; x < w; ++x)
            r[x] = in[size_t(y) * w + x] ? 255 : 0;
    }
    // Close small gaps and specks (noise, highlights), with smooth edges.
    return morphMask(morphMask(morphMask(result, 2), -3), 1);
}

} // namespace Filters
