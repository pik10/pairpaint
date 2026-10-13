// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

// Content-aware fill: texture synthesis from the area around a hole.
//
// Built only from methods published before 2005 (so free of any patents they may have had):
// pixel-by-pixel synthesis matching each pixel's neighborhood (Efros & Leung 1999), coarse to
// fine over an image pyramid (Wei & Levoy 2000), candidates from the neighbors' sources
// (Ashikhmin 2001), random candidates (Harrison 2001, as in GIMP's Resynthesizer) and a smooth
// first guess interpolated from the hole's edge (as in diffusion inpainting, Bertalmio et al. 2000).

#include "Filters.h"

#include "Blending.h"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <vector>

namespace {

// One level of the pyramid.
struct Level {
    int w = 0, h = 0;
    std::vector<QRgb> px;     // premultiplied ARGB
    std::vector<uchar> hole;  // 1 where pixels are to be filled
    std::vector<int> source;  // for hole pixels: the index of the pixel copied there
};

constexpr int kRadius = 2;               // neighborhoods of 5 x 5 pixels
constexpr int kPenalty = 2 * 255 * 255;  // a neighbor outside the image
constexpr int kExhaustive = 20000;       // try all known pixels on the coarsest level up to this many

inline int distance(QRgb a, QRgb b)
{
    const int da = qAlpha(a) - qAlpha(b), dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b);
    return da * da + dr * dr + dg * dg + db * db;
}

// Half the size: each pixel the average of the 2 x 2 below it, and part of the hole if any of them is.
Level downscale(const Level &f)
{
    Level c;
    c.w = (f.w + 1) / 2;
    c.h = (f.h + 1) / 2;
    c.px.resize(size_t(c.w) * c.h);
    c.hole.resize(c.px.size());
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            int sum[4] = {};
            bool hole = false;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const size_t k = size_t(std::min(2 * y + j, f.h - 1)) * f.w + std::min(2 * x + i, f.w - 1);
                    hole |= f.hole[k] != 0;
                    const QRgb p = f.px[k];
                    sum[0] += qAlpha(p), sum[1] += qRed(p), sum[2] += qGreen(p), sum[3] += qBlue(p);
                }
            }
            const size_t k = size_t(y) * c.w + x;
            c.hole[k] = hole;
            c.px[k] = qRgba((sum[1] + 2) / 4, (sum[2] + 2) / 4, (sum[3] + 2) / 4, (sum[0] + 2) / 4);
        }
    }
    return c;
}

class Synthesizer {
public:
    explicit Synthesizer(Level &l) : m_l(l), m_rng(12345)
    {
        for (int dy = -kRadius; dy <= kRadius; ++dy)
            for (int dx = -kRadius; dx <= kRadius; ++dx)
                if (dx || dy)
                    m_offsets.push_back({dx, dy});
        for (int i = 0; i < int(l.hole.size()); ++i)
            (l.hole[size_t(i)] ? m_holes : m_known).push_back(i);
    }

    bool hasKnown() const { return !m_known.empty(); }

    // The coarsest level's first guess: the colors around the hole blended smoothly across it.
    void smoothStart()
    {
        Level &l = m_l;
        std::vector<std::array<float, 4>> v(l.px.size());
        std::array<double, 4> mean = {};
        int edge = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            const QRgb p = l.px[i];
            v[i] = {float(qAlpha(p)), float(qRed(p)), float(qGreen(p)), float(qBlue(p))};
        }
        for (int p : m_holes)
            forEachNeighbor(p % l.w, p / l.w, [&](int n) {
                if (!l.hole[size_t(n)]) {
                    for (int c = 0; c < 4; ++c)
                        mean[size_t(c)] += v[size_t(n)][size_t(c)];
                    ++edge;
                }
            });
        for (int p : m_holes)
            for (int c = 0; c < 4; ++c)
                v[size_t(p)][size_t(c)] = edge ? float(mean[size_t(c)] / edge) : 0.0f;
        // The hole is at most a few dozen pixels here, so plain relaxation converges quickly.
        for (int iteration = 0; iteration < 300; ++iteration) {
            for (int p : m_holes) {
                std::array<float, 4> sum = {};
                int n = 0;
                forEachNeighbor(p % l.w, p / l.w, [&](int q) {
                    for (int c = 0; c < 4; ++c)
                        sum[size_t(c)] += v[size_t(q)][size_t(c)];
                    ++n;
                });
                for (int c = 0; c < 4; ++c)
                    v[size_t(p)][size_t(c)] = sum[size_t(c)] / n;
            }
        }
        l.source.assign(l.px.size(), -1);
        for (int p : m_holes) {
            const auto &c = v[size_t(p)];
            l.px[size_t(p)] = qRgba(int(c[1] + 0.5f), int(c[2] + 0.5f), int(c[3] + 0.5f), int(c[0] + 0.5f));
        }
    }

    // The coarsest level's fill, after smoothStart: from the edge inwards, each pixel gets the known
    // pixel whose neighborhood best matches its own, so patterns continue from the edge; where
    // nothing is filled yet, the smooth guess keeps the matches in tune with the far side.
    void fillFromEdge()
    {
        Level &l = m_l;
        std::vector<uchar> queued(l.px.size()), done(l.px.size());
        for (int i : m_known)
            done[size_t(i)] = 1;
        m_done = &done;
        std::vector<int> ring, next;
        for (int p : m_holes) {
            bool atEdge = false;
            forEachNeighbor(p % l.w, p / l.w, [&](int n) { atEdge |= !l.hole[size_t(n)]; });
            if (atEdge) {
                queued[size_t(p)] = 1;
                ring.push_back(p);
            }
        }
        const bool exhaustive = m_known.size() <= size_t(kExhaustive);
        std::uniform_int_distribution<int> pick(0, int(m_known.size()) - 1);
        while (!ring.empty()) {
            std::shuffle(ring.begin(), ring.end(), m_rng);
            for (int p : ring) {
                const int px = p % l.w, py = p / l.w;
                int best = -1;
                long bestCost = std::numeric_limits<long>::max();
                auto consider = [&](int c) {
                    if (c < 0 || c == best || l.hole[size_t(c)])
                        return;
                    const long cost = costOf(p, c, bestCost);
                    if (cost < bestCost) {
                        bestCost = cost;
                        best = c;
                    }
                };
                forEachNeighbor(px, py, [&](int n) {
                    if (l.hole[size_t(n)])
                        consider(shifted(l.source[size_t(n)], px - n % l.w, py - n / l.w));
                });
                if (exhaustive) {
                    for (int c : m_known)
                        consider(c);
                } else {
                    for (int k = 0; k < 600; ++k)
                        consider(m_known[size_t(pick(m_rng))]);
                }
                l.source[size_t(p)] = best;
                l.px[size_t(p)] = l.px[size_t(best)];
                done[size_t(p)] = 1;
                forEachNeighbor(px, py, [&](int n) {
                    if (l.hole[size_t(n)] && !queued[size_t(n)]) {
                        queued[size_t(n)] = 1;
                        next.push_back(n);
                    }
                });
            }
            ring.swap(next);
            next.clear();
        }
        m_done = nullptr;
    }

    // Gives every hole pixel the known pixel whose neighborhood best matches its own (as filled
    // so far): among its current source, its neighbors' sources shifted (continuing the texture
    // they started), a few close by and random ones, or all known pixels when `exhaustive`.
    // Alternates the scan direction.
    void refine(int passes, bool exhaustive)
    {
        Level &l = m_l;
        exhaustive = exhaustive && m_known.size() <= size_t(kExhaustive);
        std::uniform_int_distribution<int> pick(0, int(m_known.size()) - 1);
        std::uniform_int_distribution<int> jitter(-2, 2);
        std::vector<int> order = m_holes;
        for (int pass = 0; pass < passes; ++pass) {
            if (pass % 2)
                std::reverse(order.begin(), order.end());
            for (int p : order) {
                const int px = p % l.w, py = p / l.w;
                int best = -1;
                long bestCost = std::numeric_limits<long>::max();
                auto consider = [&](int c) {
                    if (c < 0 || c == best || l.hole[size_t(c)])
                        return;
                    const long cost = costOf(p, c, bestCost);
                    if (cost < bestCost) {
                        bestCost = cost;
                        best = c;
                    }
                };
                consider(l.source[size_t(p)]);
                forEachNeighbor(px, py, [&](int n) {
                    if (l.hole[size_t(n)])
                        consider(shifted(l.source[size_t(n)], px - n % l.w, py - n / l.w));
                });
                if (exhaustive) {
                    for (int c : m_known)
                        consider(c);
                } else {
                    const int around = best;
                    for (int k = 0; k < 4 && around >= 0; ++k)
                        consider(shifted(around, jitter(m_rng), jitter(m_rng)));
                    for (int k = 0; k < 4; ++k)
                        consider(m_known[size_t(pick(m_rng))]);
                }
                if (best < 0)
                    continue;
                l.source[size_t(p)] = best;
                l.px[size_t(p)] = l.px[size_t(best)];
            }
        }
    }

private:
    template <typename F>
    void forEachNeighbor(int x, int y, F f) const
    {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < m_l.w && ny < m_l.h)
                    f(ny * m_l.w + nx);
            }
    }

    // Pixel index `i` moved by (dx, dy), or -1 outside the image.
    int shifted(int i, int dx, int dy) const
    {
        if (i < 0)
            return -1;
        const int x = i % m_l.w + dx, y = i / m_l.w + dy;
        return x >= 0 && y >= 0 && x < m_l.w && y < m_l.h ? y * m_l.w + x : -1;
    }

    // How different the neighborhood of pixel `c` is from that of pixel `p`. Stops early once
    // it can't beat `limit`. While filling from the edge, pixels not filled yet (the smooth guess)
    // count a quarter, so the known pixels decide the pattern.
    long costOf(int p, int c, long limit) const
    {
        const Level &l = m_l;
        const int px = p % l.w, py = p / l.w, cx = c % l.w, cy = c / l.w;
        long cost = 0;
        for (const auto &[dx, dy] : m_offsets) {
            const int nx = px + dx, ny = py + dy;
            if (nx < 0 || ny < 0 || nx >= l.w || ny >= l.h)
                continue;
            const int sx = cx + dx, sy = cy + dy;
            if (sx < 0 || sy < 0 || sx >= l.w || sy >= l.h)
                cost += kPenalty;
            else if (m_done && !(*m_done)[size_t(ny) * l.w + nx])
                cost += distance(l.px[size_t(ny) * l.w + nx], l.px[size_t(sy) * l.w + sx]) / 4;
            else
                cost += distance(l.px[size_t(ny) * l.w + nx], l.px[size_t(sy) * l.w + sx]);
            if (cost >= limit)
                break;
        }
        return cost;
    }

    Level &m_l;
    std::mt19937 m_rng;
    std::vector<std::pair<int, int>> m_offsets;
    std::vector<int> m_holes, m_known;
    const std::vector<uchar> *m_done = nullptr;  // while filling from the edge: the pixels filled
};

} // namespace

namespace Filters {

QImage fillFromSurroundings(const QImage &src, const QImage &hole)
{
    QImage out = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage mask = hole.convertToFormat(QImage::Format_Alpha8);
    if (out.isNull() || mask.size() != out.size())
        return out;

    std::vector<Level> levels(1);
    {
        Level &base = levels[0];
        base.w = out.width();
        base.h = out.height();
        base.px.resize(size_t(base.w) * base.h);
        base.hole.resize(base.px.size());
    }
    int left = out.width(), top = out.height(), right = -1, bottom = -1;
    for (int y = 0; y < out.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(out.constScanLine(y));
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const size_t k = size_t(y) * out.width() + x;
            levels[0].px[k] = row[x];
            if (m[x]) {
                levels[0].hole[k] = 1;
                left = std::min(left, x), right = std::max(right, x);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0)
        return out;  // nothing to fill

    // Halve until the hole is small: there, the best matches are found among all known pixels,
    // which settles the large structures; finer levels then only refine them.
    int extent = std::max(right - left, bottom - top) + 1;
    while (extent > 24 && levels.back().w > 16 && levels.back().h > 16 && levels.size() < 8) {
        levels.push_back(downscale(levels.back()));
        extent = (extent + 1) / 2;
    }
    for (int i = int(levels.size()) - 1; i >= 0; --i) {
        Level &l = levels[size_t(i)];
        Synthesizer synth(l);
        if (!synth.hasKnown())
            return out;  // nothing to take the fill from
        const bool coarsest = i == int(levels.size()) - 1;
        if (coarsest) {
            synth.smoothStart();
            synth.fillFromEdge();
        } else {
            // Start from the coarser level's choice, scaled up.
            const Level &c = levels[size_t(i) + 1];
            l.source.assign(l.px.size(), -1);
            for (int y = 0; y < l.h; ++y) {
                for (int x = 0; x < l.w; ++x) {
                    const size_t k = size_t(y) * l.w + x;
                    if (!l.hole[k])
                        continue;
                    const int cs = c.source[size_t(y / 2) * c.w + x / 2];
                    const int sx = std::min(2 * (cs % c.w) + x % 2, l.w - 1);
                    const int sy = std::min(2 * (cs / c.w) + y % 2, l.h - 1);
                    const int s = sy * l.w + sx;
                    if (!l.hole[size_t(s)]) {  // otherwise the refinement finds one
                        l.source[k] = s;
                        l.px[k] = l.px[size_t(s)];
                    }
                }
            }
        }
        synth.refine(coarsest ? 2 : (i == 0 ? 2 : 3), coarsest);
    }

    const Level &result = levels[0];
    for (int y = 0; y < result.h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < result.w; ++x)
            row[x] = result.px[size_t(y) * result.w + x];
    }
    return out;
}

QImage contentAwareFill(const QImage &image, const QImage &coverage, QRect *area)
{
    const QImage cov = coverage.convertToFormat(QImage::Format_Alpha8);
    int left = cov.width(), top = cov.height(), right = -1, bottom = -1;
    for (int y = 0; y < cov.height(); ++y) {
        const uchar *row = cov.constScanLine(y);
        for (int x = 0; x < cov.width(); ++x) {
            if (row[x]) {
                left = std::min(left, x), right = std::max(right, x);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
        }
    }
    *area = right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
    if (area->isEmpty())
        return {};
    // The fill comes from around the hole, as far again as the hole is big.
    const int margin = std::max(32, std::max(area->width(), area->height()));
    const QRect region = area->adjusted(-margin, -margin, margin, margin) & image.rect();
    const QImage hole = cov.copy(region);
    const QImage filled = fillFromSurroundings(image.copy(region), hole);
    const QRect inside = area->translated(-region.topLeft());
    QImage out = image.copy(*area).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    Blend::mixByAlpha(out, filled.copy(inside), hole.copy(inside).convertToFormat(QImage::Format_ARGB32_Premultiplied));
    return out;
}

} // namespace Filters
