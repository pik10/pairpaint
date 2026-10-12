// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "BrushTool.h"

#include "Blending.h"
#include "Canvas.h"
#include "Document.h"
#include "Filters.h"
#include "ToolHelpers.h"
#include "ToolSettings.h"

#include <QLineF>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

// One stroke of a brush tool: what the stroke engine and the tool's effect share.
struct Stroke {
    Document *doc = nullptr;
    ToolSettings *settings = nullptr;
    bool active = false;  // the button is down
    QImage base;          // the target before the stroke
    QImage buffer;        // the stroke at full opacity, for effects that use one
    QRect rect;           // the area the stroke has touched so far
};

// One round dab of the brush.
struct Dab {
    QPointF center;
    qreal radius;
    qreal alpha;  // 0..1: pen pressure, when it controls opacity
};

// What a brush tool does. The stroke engine (BrushTool, below) handles the pointer: dab spacing,
// pen pressure, smoothing, Shift-click lines and undo. An effect only decides what a dab does to
// the pixels and how the stroke so far shows on the layer. So a new brush tool is a new effect,
// and a new brush feature (tip shapes, spacing, ...) goes into the engine for all of them.
class BrushEffect {
public:
    virtual ~BrushEffect() = default;
    virtual Tool::Id id() const = 0;

    // A click that may start a stroke. False when it does something else instead (setting a clone
    // source) or the stroke can't start; the effect tells the user why.
    virtual bool press(Stroke &, const ToolEvent &, Canvas *) { return true; }
    // Whether Shift-click draws a straight line from the end of the last stroke.
    virtual bool drawsLines() const { return true; }
    // Whether the stroke collects its dabs in Stroke::buffer.
    virtual bool usesBuffer() const { return true; }
    // The stroke starts: base (and buffer) are ready.
    virtual void begin(Stroke &, const ToolEvent &) {}
    // Adds one dab to the stroke; returns the area it may have changed.
    virtual QRectF dab(Stroke &s, const Dab &d)
    {
        QPainter p(&s.buffer);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        drawRound(p, d, dabColor(s), s.settings->hardness);
        return dirtyRect(d);
    }
    // Shows the stroke within `r` (inside the image) on the target.
    virtual void show(Stroke &s, const QRect &r) { apply(s, r, strokeIn(s, r), s.settings->opacity / 100.0); }
    // The button was released; Stroke::active is already false. Undo is recorded after this.
    virtual void finish(Stroke &) {}
    // Marks over the canvas; `brush` is where the brush is, in image coordinates.
    virtual void paintOverlay(QPainter &, const QTransform &, const Stroke &, const QPointF & /*brush*/) {}

protected:
    // The color of a plain dab; most effects use only its coverage.
    virtual QColor dabColor(const Stroke &) const { return Qt::black; }

    static QRectF dirtyRect(const Dab &d)
    {
        return QRectF(d.center.x() - d.radius - 2, d.center.y() - d.radius - 2, 2 * d.radius + 4, 2 * d.radius + 4);
    }

    // A round dab of `color`, soft from `hardness` (0..100) outwards.
    static void drawRound(QPainter &p, const Dab &d, QColor color, int hardness)
    {
        const qreal hard = hardness / 100.0;
        color.setAlphaF(d.alpha);
        if (hard >= 0.999) {
            p.setBrush(color);
        } else {
            QColor clear = color;
            clear.setAlpha(0);
            QRadialGradient g(d.center, d.radius);
            g.setColorAt(0, color);
            g.setColorAt(hard, color);
            g.setColorAt(1, clear);
            p.setBrush(g);
        }
        p.drawEllipse(d.center, d.radius, d.radius);
    }

    // A round dab as coverage (Format_Alpha8) of `area`, soft from `hardness` (0..100) outwards.
    static QImage coverage(const QRect &area, const Dab &d, int hardness)
    {
        QImage coverage(area.size(), QImage::Format_Alpha8);
        coverage.fill(0);
        QPainter q(&coverage);
        q.setRenderHint(QPainter::Antialiasing);
        q.setPen(Qt::NoPen);
        const QPointF local = d.center - QPointF(area.topLeft());
        const qreal hard = hardness / 100.0;
        if (hard >= 0.999) {
            q.setBrush(Qt::black);
        } else {
            QRadialGradient g(local, d.radius);
            g.setColorAt(0, Qt::black);
            g.setColorAt(hard, Qt::black);
            g.setColorAt(1, Qt::transparent);
            q.setBrush(g);
        }
        q.drawEllipse(local, d.radius, d.radius);
        return coverage;
    }

    // The stroke buffer in `r`, limited to the selection.
    static QImage strokeIn(const Stroke &s, const QRect &r) { return selectionMasked(s.buffer.copy(r), s.doc, r); }

    // Draws `stroke` (pixels for the area `r`) over the target as it was before the stroke.
    static void apply(Stroke &s, const QRect &r, const QImage &stroke, qreal opacity,
                      QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver)
    {
        QPainter p(&s.doc->targetImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), s.base, r);
        p.setOpacity(opacity);
        p.setCompositionMode(mode);
        p.drawImage(r.topLeft(), stroke);
        p.end();
        s.doc->notifyImageChanged(r);
    }
};

// ---------------------------------------------------------------------------

class PaintEffect : public BrushEffect {
public:
    Tool::Id id() const override { return Tool::Brush; }

protected:
    QColor dabColor(const Stroke &s) const override { return s.settings->foreground(); }
};

class EraseEffect : public BrushEffect {
public:
    Tool::Id id() const override { return Tool::Eraser; }
    void show(Stroke &s, const QRect &r) override
    {
        apply(s, r, strokeIn(s, r), s.settings->opacity / 100.0, QPainter::CompositionMode_DestinationOut);
    }
};

// Brushes that change only the pixels matching a sampled color (the one under the brush center, or
// the one clicked first): fully within the tolerance, fading out a little beyond it, so soft edges
// are partly changed. The buffer's alpha keeps the most each pixel was matched; colors are always
// judged on the layer as it was before the stroke.
class ColorMatchEffect : public BrushEffect {
public:
    bool press(Stroke &s, const ToolEvent &, Canvas *) override
    {
        if (!s.doc->editingMask())
            return true;
        showTip(QObject::tr("The %1 works on the layer's pixels. Press Ctrl+\\ to edit them.").arg(Tool::name(id())));
        return false;
    }

    void begin(Stroke &s, const ToolEvent &e) override
    {
        // "Once": the color clicked is the one erased for the whole stroke.
        m_color = s.doc->rect().contains(pixelAt(e.pos)) ? rawPixel(s.base, pixelAt(e.pos)) : 0;
    }

    QRectF dab(Stroke &s, const Dab &d) override
    {
        const QPoint center = pixelAt(d.center);
        const QRect area = QRectF(d.center.x() - d.radius - 1, d.center.y() - d.radius - 1, 2 * d.radius + 2,
                                  2 * d.radius + 2).toAlignedRect()
                           & s.doc->rect();
        if (area.isEmpty() || !s.doc->rect().contains(center))
            return {};
        const ToolSettings *st = s.settings;
        if (!st->sampleOnce)
            m_color = rawPixel(s.base, center);
        const int tol = st->tolerance;
        const int soft = std::max(1, tol / 2);
        const QRgb protect = qPremultiply(st->foreground().rgba());
        auto distance = [](QRgb a, QRgb b) {
            return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                             std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
        };
        // How much each pixel matches the color to erase, 0..255.
        const int w = area.width(), h = area.height();
        std::vector<uchar> match(size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(s.base.constScanLine(area.top() + y)) + area.left();
            for (int x = 0; x < w; ++x) {
                const int dist = distance(row[x], m_color);
                int k = dist <= tol ? 255 : std::max(0, 255 * (tol + soft - dist) / soft);
                if (k && protectsForeground(s) && distance(row[x], protect) <= tol)
                    k = 0;
                match[size_t(y) * w + x] = uchar(k);
            }
        }
        if (st->contiguous) {
            // Only matching pixels connected to the brush center.
            std::vector<uchar> reached(match.size(), 0);
            std::vector<int> stack;
            const int start = (center.y() - area.top()) * w + (center.x() - area.left());
            if (match[size_t(start)]) {
                reached[size_t(start)] = 1;
                stack.push_back(start);
            }
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % w, y = i / w;
                for (const auto &[dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int nx = x + dx, ny = y + dy;
                    const size_t n = size_t(ny) * w + nx;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && !reached[n] && match[n]) {
                        reached[n] = 1;
                        stack.push_back(int(n));
                    }
                }
            }
            for (size_t i = 0; i < match.size(); ++i)
                if (!reached[i])
                    match[i] = 0;
        }
        const QImage cov = coverage(area, d, st->hardness);
        for (int y = 0; y < h; ++y) {
            const uchar *c = cov.constScanLine(y);
            QRgb *buf = reinterpret_cast<QRgb *>(s.buffer.scanLine(area.top() + y)) + area.left();
            for (int x = 0; x < w; ++x) {
                const int e = int(c[x] * match[size_t(y) * w + x] / 255 * d.alpha);
                if (e > qAlpha(buf[x]))
                    buf[x] = qRgba(0, 0, 0, e);
            }
        }
        return area;
    }

    void paintOverlay(QPainter &p, const QTransform &t, const Stroke &, const QPointF &brush) override
    {
        // The hot spot: its color is the one changed.
        const QPointF c = t.map(brush);
        for (const auto &[pen, w] : {std::pair{QColor(0, 0, 0, 160), 3.0}, std::pair{QColor(Qt::white), 1.0}}) {
            p.setPen(QPen(pen, w));
            p.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
            p.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
        }
    }

protected:
    virtual bool protectsForeground(const Stroke &) const { return false; }

private:
    QRgb m_color = 0;  // the (premultiplied) color being matched
};

// Background Eraser: erases the matching pixels.
class BackgroundEraseEffect : public ColorMatchEffect {
public:
    Tool::Id id() const override { return Tool::BackgroundEraser; }
    void show(Stroke &s, const QRect &r) override
    {
        apply(s, r, strokeIn(s, r), 1.0, QPainter::CompositionMode_DestinationOut);
    }

protected:
    bool protectsForeground(const Stroke &s) const override { return s.settings->protectForeground; }
};

// Color Replacement: gives the matching pixels the foreground color's hue and saturation (or one
// of them, or its luminosity), keeping their own shading.
class ColorReplaceEffect : public ColorMatchEffect {
public:
    Tool::Id id() const override { return Tool::ColorReplacement; }
    void show(Stroke &s, const QRect &r) override
    {
        static constexpr QPainter::CompositionMode modes[] = {Blend::Color, Blend::Hue, Blend::Saturation, Blend::Luminosity};
        const QImage before = s.base.copy(r);
        QImage recolored = before;
        QImage paint(r.size(), QImage::Format_ARGB32_Premultiplied);
        paint.fill(s.settings->foreground().rgb());  // opaque: only its color counts
        Blend::draw(recolored, paint, modes[std::clamp(s.settings->replaceMode, 0, 3)], 1.0, r.topLeft());
        Blend::restoreAlpha(recolored, before);
        QImage out = before;
        Blend::mixByAlpha(out, recolored, strokeIn(s, r));
        QPainter p(&s.doc->targetImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), out);
        p.end();
        s.doc->notifyImageChanged(r);
    }
};

// Blur, Sharpen, Dodge, Burn and Sponge: the layer with an adjustment applied, revealed where
// the stroke goes.
class AdjustEffect : public BrushEffect {
public:
    explicit AdjustEffect(Tool::Id id) : m_id(id) {}
    Tool::Id id() const override { return m_id; }

    void show(Stroke &s, const QRect &r) override
    {
        QImage adjusted = adjustedRegion(s, r);
        QPainter q(&adjusted);
        q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        q.drawImage(0, 0, strokeIn(s, r));
        q.end();
        apply(s, r, adjusted, s.settings->opacity / 100.0);
    }

private:
    // The target's pixels in `r` with this tool's adjustment applied at full strength.
    QImage adjustedRegion(const Stroke &s, const QRect &r) const
    {
        const ToolSettings *st = s.settings;
        switch (m_id) {
        case Tool::Dodge:
        case Tool::Burn:
            return Filters::dodgeBurn(s.base.copy(r), m_id == Tool::Burn, st->toneRange);
        case Tool::Sponge:
            return Filters::hueSaturation(s.base.copy(r), 0, st->spongeSaturate ? 60 : -60, 0);
        case Tool::Blur:
        case Tool::Sharpen: {
            const double sigma = std::max(1.5, st->size * 0.06);
            const int m = int(std::ceil(sigma * 3)) + 2;  // the filter needs pixels around the area
            const QRect er = r.adjusted(-m, -m, m, m) & s.doc->rect();
            const QImage around = s.base.copy(er);
            const QImage filtered = m_id == Tool::Blur ? Filters::gaussianBlur(around, sigma)
                                                       : Filters::unsharpMask(around, 120, 1.5, 2);
            return filtered.copy(r.translated(-er.topLeft()));
        }
        default:
            return {};
        }
    }

    Tool::Id m_id;
};

// Shared by the Clone Stamp and the healing brushes: pixels taken from a source at an offset.
class SourceEffect : public BrushEffect {
protected:
    // Replaces the cloned pixels with ones that blend into their surroundings.
    void applyHealing(Stroke &s)
    {
        if (s.rect.isEmpty())
            return;
        const int margin = std::max(8, s.settings->size);
        const QRect r = s.rect.adjusted(-margin, -margin, margin, margin) & s.doc->rect();
        const QImage src = m_source.copy(r.translated(m_offset));
        const QImage dst = s.base.copy(r);
        const QImage cov = s.buffer.copy(r).convertToFormat(QImage::Format_Alpha8);
        QImage healed = Filters::heal(src, dst, cov, std::max(3.0, s.settings->size / 3.0));
        QPainter hp(&healed);
        hp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        hp.drawImage(0, 0, cov);
        hp.end();
        QPainter bp(&s.buffer);
        bp.setCompositionMode(QPainter::CompositionMode_Source);
        bp.drawImage(r.topLeft(), healed);
        bp.end();
        s.rect |= r;
        show(s, r);
    }

    QImage m_source;  // the pixels copied from
    QPoint m_offset;  // source position minus brush position
};

// Clone Stamp, and the Healing Brush, which blends the clone in when the stroke ends.
class CloneEffect : public SourceEffect {
public:
    explicit CloneEffect(bool heal) : m_heal(heal) {}
    Tool::Id id() const override { return m_heal ? Tool::Healing : Tool::CloneStamp; }
    bool drawsLines() const override { return false; }

    bool press(Stroke &, const ToolEvent &e, Canvas *canvas) override
    {
        if (e.modifiers & (Qt::AltModifier | Qt::ControlModifier)) {
            m_sourcePoint = pixelAt(e.pos);
            m_hasSource = true;
            m_offsetSet = false;
            canvas->update();
            return false;
        }
        if (!m_hasSource) {
            showTip(QObject::tr("Alt-click or Ctrl-click to set the source point first."));
            return false;
        }
        return true;
    }

    void begin(Stroke &s, const ToolEvent &e) override
    {
        if (!m_offsetSet) {  // "aligned": the offset is kept for later strokes
            m_offset = m_sourcePoint - pixelAt(e.pos);
            m_offsetSet = true;
        }
        m_source = s.settings->sampleMerged ? s.doc->flattened() : s.base;
    }

    QRectF dab(Stroke &s, const Dab &d) override
    {
        const qreal hard = s.settings->hardness / 100.0;
        const QPointF c = d.center;
        const qreal radius = d.radius;
        const QRect dr = QRectF(c.x() - radius - 1, c.y() - radius - 1, 2 * radius + 2, 2 * radius + 2).toAlignedRect();
        QImage img(dr.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter q(&img);
        q.setRenderHint(QPainter::Antialiasing);
        q.translate(-dr.topLeft());
        q.setPen(Qt::NoPen);
        QBrush texture(m_source);
        texture.setTransform(QTransform::fromTranslate(-m_offset.x(), -m_offset.y()));
        q.setBrush(texture);
        q.drawEllipse(c, radius, radius);
        if (hard < 0.999) {
            QRadialGradient g(c, radius);
            g.setColorAt(0, Qt::black);
            g.setColorAt(hard, Qt::black);
            g.setColorAt(1, Qt::transparent);
            q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            q.setBrush(g);
            q.drawEllipse(c, radius, radius);
        }
        q.end();
        QPainter p(&s.buffer);
        p.setOpacity(d.alpha);
        p.drawImage(dr.topLeft(), img);
        return dirtyRect(d);
    }

    void finish(Stroke &s) override
    {
        if (m_heal)
            applyHealing(s);
        m_source = QImage();
    }

    void paintOverlay(QPainter &p, const QTransform &t, const Stroke &s, const QPointF &brush) override
    {
        if (!m_hasSource)
            return;
        const QPointF src = m_offsetSet ? brush + QPointF(m_offset) : QPointF(m_sourcePoint);
        const QPointF c = t.map(src);
        const qreal r = std::max<qreal>(3, s.settings->size / 2.0 * t.m11());
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawEllipse(c, r, r);
        p.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        p.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
        p.setPen(QPen(Qt::white, 1));
        p.drawEllipse(c, r, r);
        p.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        p.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
    }

private:
    bool m_heal;
    QPoint m_sourcePoint;
    bool m_hasSource = false;
    bool m_offsetSet = false;
};

// Spot Healing Brush: no source to pick; when the stroke ends, a nearby patch whose surroundings
// look most alike is healed in.
class SpotHealEffect : public SourceEffect {
public:
    Tool::Id id() const override { return Tool::SpotHealing; }

    void show(Stroke &s, const QRect &r) override
    {
        if (!s.active) {
            SourceEffect::show(s, r);
            return;
        }
        // While painting, show where the healing will happen as a dark tint.
        apply(s, r, strokeIn(s, r), 0.4);
    }

    void finish(Stroke &s) override
    {
        if (!s.rect.isEmpty()) {
            m_source = s.base;
            m_offset = findSource(s);
        }
        applyHealing(s);
        m_source = QImage();
    }

private:
    // Tries offsets around the stroke and returns the one whose surroundings best match the
    // stroke's surroundings, staying inside the image.
    static QPoint findSource(const Stroke &s)
    {
        const int margin = std::max(8, s.settings->size);
        const QRect area = s.rect.adjusted(-margin, -margin, margin, margin) & s.doc->rect();
        const QImage cov = s.buffer.copy(area).convertToFormat(QImage::Format_Alpha8);
        const int reach = std::max(s.rect.width(), s.rect.height()) + 4;
        const int step = std::max(1, std::min(area.width(), area.height()) / 40);
        QPoint best(reach, 0);
        double bestScore = std::numeric_limits<double>::max();
        for (double scale : {1.15, 1.6, 2.2}) {
            for (int k = 0; k < 16; ++k) {
                const double a = qDegreesToRadians(k * 22.5);
                const QPoint o(qRound(std::cos(a) * reach * scale), qRound(std::sin(a) * reach * scale));
                if (!s.doc->rect().contains(area.translated(o)))
                    continue;
                double score = 0;
                for (int y = 0; y < area.height(); y += step) {
                    const uchar *c = cov.constScanLine(y);
                    const QRgb *d = reinterpret_cast<const QRgb *>(s.base.constScanLine(area.top() + y)) + area.left();
                    const QRgb *src = reinterpret_cast<const QRgb *>(s.base.constScanLine(area.top() + y + o.y())) + area.left() + o.x();
                    for (int x = 0; x < area.width(); x += step) {
                        if (c[x])
                            continue;  // compare only the surroundings, not the blemish
                        const int dr = qRed(d[x]) - qRed(src[x]), dg = qGreen(d[x]) - qGreen(src[x]), db = qBlue(d[x]) - qBlue(src[x]);
                        score += dr * dr + dg * dg + db * db;
                    }
                }
                if (score < bestScore) {
                    bestScore = score;
                    best = o;
                }
            }
        }
        return best;
    }
};

// Smudge: blends the paint picked up so far into the layer under the dab, then picks up some of
// the result. Works on the layer directly, without a stroke buffer.
class SmudgeEffect : public BrushEffect {
public:
    Tool::Id id() const override { return Tool::Smudge; }
    bool usesBuffer() const override { return false; }

    void begin(Stroke &s, const ToolEvent &) override
    {
        m_sample = QImage();  // picked up by the first dab
        m_size = int(std::ceil(s.settings->size)) + 2;
    }

    QRectF dab(Stroke &s, const Dab &d) override
    {
        const int size = m_size;
        const QPoint topLeft(qRound(d.center.x() - size / 2.0), qRound(d.center.y() - size / 2.0));
        const QRect area(topLeft, QSize(size, size));
        QImage &target = s.doc->targetImage();
        const QImage current = target.copy(area);
        if (m_sample.isNull()) {
            m_sample = current;
            return {};
        }
        QImage cov = coverage(area, d, s.settings->hardness);
        if (s.doc->hasSelection()) {
            QPainter q(&cov);
            q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            q.drawImage(QPoint(0, 0), s.doc->selection(), area);
        }
        const qreal strength = s.settings->opacity / 100.0 * d.alpha;
        QImage result = current;
        for (int y = 0; y < size; ++y) {
            QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
            const QRgb *smp = reinterpret_cast<const QRgb *>(m_sample.constScanLine(y));
            const uchar *c = cov.constScanLine(y);
            for (int x = 0; x < size; ++x) {
                const int k = int(c[x] * strength);
                if (k == 0)
                    continue;
                const QRgb a = out[x], b = smp[x];
                auto mix = [k](int u, int v) { return u + (v - u) * k / 255; };
                out[x] = qRgba(mix(qRed(a), qRed(b)), mix(qGreen(a), qGreen(b)), mix(qBlue(a), qBlue(b)), mix(qAlpha(a), qAlpha(b)));
            }
        }
        QPainter tp(&target);
        tp.setCompositionMode(QPainter::CompositionMode_Source);
        tp.drawImage(topLeft, result);
        tp.end();
        // Carry the paint along: the sample drifts towards what is now under the brush.
        const int keep = int(strength * 255);
        for (int y = 0; y < size; ++y) {
            QRgb *smp = reinterpret_cast<QRgb *>(m_sample.scanLine(y));
            const QRgb *now = reinterpret_cast<const QRgb *>(result.constScanLine(y));
            for (int x = 0; x < size; ++x) {
                const QRgb a = now[x], b = smp[x];
                auto mix = [keep](int u, int v) { return u + (v - u) * keep / 255; };
                smp[x] = qRgba(mix(qRed(a), qRed(b)), mix(qGreen(a), qGreen(b)), mix(qBlue(a), qBlue(b)), mix(qAlpha(a), qAlpha(b)));
            }
        }
        return area;
    }

    void show(Stroke &s, const QRect &r) override { s.doc->notifyImageChanged(r); }  // already on the layer
    void finish(Stroke &) override { m_sample = QImage(); }

private:
    QImage m_sample;  // the paint being carried
    int m_size = 0;
};

// ---------------------------------------------------------------------------
// The stroke engine: turns pointer movement into evenly spaced dabs for the tool's effect.

class BrushTool : public Tool {
public:
    BrushTool(ToolSettings *s, std::unique_ptr<BrushEffect> effect) : Tool(s), m_effect(std::move(effect))
    {
        m_stroke.settings = s;
    }

    Id id() const override { return m_effect->id(); }
    bool showsBrushOutline() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_stroke.doc = m_doc;
        if (!m_effect->press(m_stroke, e, m_canvas))
            return;
        m_doc->prepareForPixelEdit();
        m_stroke.active = true;
        m_before = m_doc->state();
        m_stroke.base = m_doc->targetImage();
        if (m_effect->usesBuffer()) {
            m_stroke.buffer = QImage(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
            m_stroke.buffer.fill(Qt::transparent);
        }
        m_stroke.rect = QRect();
        m_residual = 0;
        m_effect->begin(m_stroke, e);
        if ((e.modifiers & Qt::ShiftModifier) && m_lastDoc == m_doc && m_effect->drawsLines())
            stroke(m_lastEnd, e.pos, true, e.pressure, e.pressure);
        else
            stroke(e.pos, e.pos, true, e.pressure, e.pressure);
        m_last = m_raw = e.pos;
        m_lastPressure = e.pressure;
    }

    void move(const ToolEvent &e) override
    {
        m_hover = e.pos;
        if (!m_stroke.active)
            return;
        m_raw = e.pos;
        // Smoothing ("pulled string"): the brush follows the pointer on a string and only moves
        // when the string is taut, so small wobbles of the hand don't reach the canvas.
        const qreal string = stringLength();
        QPointF to = e.pos;
        if (string > 0) {
            const QLineF pull(m_last, e.pos);
            if (pull.length() <= string) {
                m_canvas->update();  // the string moved
                return;
            }
            to = pull.pointAt((pull.length() - string) / pull.length());
            m_canvas->update();
        }
        stroke(m_last, to, false, m_lastPressure, e.pressure);
        m_last = to;
        m_lastPressure = e.pressure;
    }

    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (m_stroke.active && stringLength() > 0 && m_last != m_raw) {  // catch up: end where the pointer is
            stroke(m_last, m_raw, false, m_lastPressure, e.pressure);
            m_last = m_raw;
        }
        finish();
    }

    void cancel() override { finish(); }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (m_stroke.active && stringLength() > 0 && m_raw != m_last) {  // the smoothing string
            const QPointF a = t.map(m_last), b = t.map(m_raw);
            p.setPen(QPen(QColor(0, 0, 0, 140), 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a, b);
            p.setPen(QPen(QColor(255, 120, 200), 1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a, b);
            p.setBrush(QColor(255, 120, 200));
            p.drawEllipse(b, 2.5, 2.5);
        }
        m_effect->paintOverlay(p, t, m_stroke, m_stroke.active ? m_last : m_hover);
    }

private:
    // The smoothing string in image pixels: up to 60 screen pixels at 100% smoothing, so it
    // feels the same at any zoom.
    qreal stringLength() const
    {
        if (m_settings->smoothing <= 0 || !m_canvas)
            return 0;
        return m_settings->smoothing / 100.0 * 60.0 / std::max<qreal>(0.01, m_canvas->zoom());
    }

    void finish()
    {
        if (!m_stroke.active)
            return;
        m_stroke.active = false;
        m_effect->finish(m_stroke);
        m_lastEnd = m_last;
        m_lastDoc = m_doc;
        m_doc->commit(name(id()), m_before);
        m_doc->notifyStructureChanged();
        m_before = DocState();
        m_stroke.base = m_stroke.buffer = QImage();
    }

    // Dabs from a to b, spaced evenly along the whole stroke; pressure goes from pa to pb.
    void stroke(const QPointF &a, const QPointF &b, bool first, qreal pa, qreal pb)
    {
        const qreal baseRadius = std::max<qreal>(0.5, m_settings->size / 2.0);
        const qreal spacing = std::max<qreal>(1.0, m_settings->size * 0.12);
        QRectF dirty;
        auto dab = [&](const QPointF &c, qreal pressure) {
            const qreal radius = m_settings->pressureSize ? std::max<qreal>(0.5, baseRadius * pressure) : baseRadius;
            const qreal alpha = m_settings->pressureOpacity ? pressure : 1.0;
            dirty |= m_effect->dab(m_stroke, {c, radius, alpha});
        };
        if (first)
            dab(a, pa);
        const QLineF line(a, b);
        const qreal len = line.length();
        qreal t = spacing - m_residual;  // distance along the line to the next dab
        while (t <= len) {
            dab(line.pointAt(t / len), pa + (pb - pa) * t / len);
            t += spacing;
        }
        m_residual = len - (t - spacing);
        const QRect r = dirty.toAlignedRect() & m_doc->rect();
        if (r.isEmpty())
            return;
        m_stroke.rect |= r;
        m_effect->show(m_stroke, r);
    }

    std::unique_ptr<BrushEffect> m_effect;
    Stroke m_stroke;
    DocState m_before;
    QPointF m_last, m_lastEnd, m_hover;
    QPointF m_raw;  // the pointer; m_last trails it by the smoothing string
    qreal m_lastPressure = 1.0;
    const Document *m_lastDoc = nullptr;
    qreal m_residual = 0;
};

} // namespace

std::unique_ptr<Tool> createBrushTool(Tool::Id id, ToolSettings *settings)
{
    std::unique_ptr<BrushEffect> effect;
    switch (id) {
    case Tool::Brush: effect = std::make_unique<PaintEffect>(); break;
    case Tool::ColorReplacement: effect = std::make_unique<ColorReplaceEffect>(); break;
    case Tool::Eraser: effect = std::make_unique<EraseEffect>(); break;
    case Tool::BackgroundEraser: effect = std::make_unique<BackgroundEraseEffect>(); break;
    case Tool::CloneStamp: effect = std::make_unique<CloneEffect>(false); break;
    case Tool::Healing: effect = std::make_unique<CloneEffect>(true); break;
    case Tool::SpotHealing: effect = std::make_unique<SpotHealEffect>(); break;
    case Tool::Smudge: effect = std::make_unique<SmudgeEffect>(); break;
    case Tool::Blur:
    case Tool::Sharpen:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Sponge: effect = std::make_unique<AdjustEffect>(id); break;
    default: return nullptr;
    }
    return std::make_unique<BrushTool>(settings, std::move(effect));
}
