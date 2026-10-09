// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Tools.h"

#include "Canvas.h"
#include "Dialogs.h"
#include "TextBox.h"
#include "Document.h"
#include "Filters.h"
#include "ToolSettings.h"

#include <QClipboard>
#include <QCursor>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QtMath>
#include <functional>
#include <limits>

namespace {

QPoint pixelAt(const QPointF &p) { return QPoint(qFloor(p.x()), qFloor(p.y())); }

SelectionOp selectionOp(Qt::KeyboardModifiers m)
{
    const bool shift = m & Qt::ShiftModifier;
    const bool sub = m & (Qt::AltModifier | Qt::ControlModifier);
    if (shift && sub) return SelectionOp::Intersect;
    if (shift) return SelectionOp::Add;
    if (sub) return SelectionOp::Subtract;
    return SelectionOp::Replace;
}

QPointF snap45(const QPointF &from, const QPointF &to)
{
    QLineF l(from, to);
    l.setAngle(std::round(l.angle() / 45.0) * 45.0);
    return l.p2();
}

QPointF snapSquare(const QPointF &from, const QPointF &to)
{
    const QPointF d = to - from;
    const qreal s = std::max(std::abs(d.x()), std::abs(d.y()));
    return from + QPointF(d.x() < 0 ? -s : s, d.y() < 0 ? -s : s);
}

QPointF rotated(const QPointF &p, qreal degrees)
{
    const qreal a = qDegreesToRadians(degrees);
    return QPointF(p.x() * std::cos(a) - p.y() * std::sin(a), p.x() * std::sin(a) + p.y() * std::cos(a));
}

void drawOutline(QPainter &p, const QPainterPath &path)
{
    p.setBrush(Qt::NoBrush);
    QPen white(Qt::white, 1);
    white.setCosmetic(true);
    p.setPen(white);
    p.drawPath(path);
    QPen black(Qt::black, 1, Qt::DashLine);
    black.setCosmetic(true);
    p.setPen(black);
    p.drawPath(path);
}

QImage selectionMasked(QImage img, const Document *doc, const QRect &area)
{
    if (doc->hasSelection()) {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(QPoint(0, 0), doc->selection(), area);
    }
    return img;
}

void showTip(const QString &text) { QToolTip::showText(QCursor::pos(), text); }

// Shared machinery for tools that redraw a temporary result over the original
// target on every mouse move (shapes, gradient).
class PreviewEdit {
public:
    void begin(Document *doc)
    {
        doc->prepareForPixelEdit();
        m_doc = doc;
        m_before = doc->state();
        m_base = doc->targetImage();
        m_last = QRect();
        m_active = true;
    }
    bool active() const { return m_active; }

    void render(const QRect &area, qreal opacity, const std::function<void(QPainter &)> &paint)
    {
        if (!m_active)
            return;
        const QRect cur = area & m_doc->rect();
        const QRect dirty = (cur | m_last) & m_doc->rect();
        m_last = cur;
        if (dirty.isEmpty())
            return;
        QImage stroke(dirty.size(), QImage::Format_ARGB32_Premultiplied);
        stroke.fill(Qt::transparent);
        {
            QPainter p(&stroke);
            p.translate(-dirty.topLeft());
            paint(p);
        }
        stroke = selectionMasked(stroke, m_doc, dirty);
        QPainter p(&m_doc->targetImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(dirty.topLeft(), m_base, dirty);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setOpacity(opacity);
        p.drawImage(dirty.topLeft(), stroke);
        p.end();
        m_doc->notifyImageChanged(dirty);
    }

    void commit(const QString &text)
    {
        if (!m_active)
            return;
        m_active = false;
        m_doc->commit(text, m_before);
        m_doc->notifyStructureChanged();
        m_base = QImage();
        m_before = DocState();
    }

    void abort()
    {
        if (!m_active)
            return;
        m_active = false;
        m_doc->targetImage() = m_base;
        m_doc->notifyImageChanged();
        m_base = QImage();
        m_before = DocState();
    }

private:
    Document *m_doc = nullptr;
    DocState m_before;
    QImage m_base;
    QRect m_last;
    bool m_active = false;
};

// ---------------------------------------------------------------------------
// Brush, eraser, clone stamp and healing brush: all paint round dabs into a
// stroke buffer that is composited onto the target with the tool opacity.

class BrushTool : public Tool {
public:
    enum Mode { Paint, Erase, Clone, Heal, SpotHeal, SmudgeMode, BlurMode, SharpenMode, DodgeMode, BurnMode, SpongeMode };
    BrushTool(ToolSettings *s, Mode mode) : Tool(s), m_mode(mode) {}

    Id id() const override
    {
        switch (m_mode) {
        case Erase: return Eraser;
        case Clone: return CloneStamp;
        case Heal: return Healing;
        case SmudgeMode: return Smudge;
        case SpotHeal: return SpotHealing;
        case BlurMode: return Blur;
        case SharpenMode: return Sharpen;
        case SpongeMode: return Sponge;
        case DodgeMode: return Dodge;
        case BurnMode: return Burn;
        default: return Brush;
        }
    }
    bool showsBrushOutline() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        const bool sampling = m_mode == Clone || m_mode == Heal;
        if (sampling && (e.modifiers & (Qt::AltModifier | Qt::ControlModifier))) {
            m_sourcePoint = pixelAt(e.pos);
            m_hasSource = true;
            m_offsetSet = false;
            m_canvas->update();
            return;
        }
        if (sampling && !m_hasSource) {
            showTip(QObject::tr("Alt-click or Ctrl-click to set the source point first."));
            return;
        }

        m_doc->prepareForPixelEdit();
        m_active = true;
        m_before = m_doc->state();
        m_base = m_doc->targetImage();
        if (m_mode == SmudgeMode) {
            m_sample = QImage();  // picked up by the first dab
            m_smudgeSize = int(std::ceil(m_settings->size)) + 2;
        } else {
            m_buffer = QImage(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
            m_buffer.fill(Qt::transparent);
        }
        m_strokeRect = QRect();
        m_residual = 0;
        if (sampling) {
            if (!m_offsetSet) {  // "aligned": the offset is kept for later strokes
                m_offset = m_sourcePoint - pixelAt(e.pos);
                m_offsetSet = true;
            }
            m_source = m_settings->sampleMerged ? m_doc->flattened() : m_base;
        }
        if ((e.modifiers & Qt::ShiftModifier) && m_lastDoc == m_doc && !sampling)
            stroke(m_lastEnd, e.pos, true, e.pressure, e.pressure);
        else
            stroke(e.pos, e.pos, true, e.pressure, e.pressure);
        m_last = m_raw = e.pos;
        m_lastPressure = e.pressure;
    }

    void move(const ToolEvent &e) override
    {
        m_hover = e.pos;
        if (!m_active)
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
        if (m_active && stringLength() > 0 && m_last != m_raw) {  // catch up: end where the pointer is
            stroke(m_last, m_raw, false, m_lastPressure, e.pressure);
            m_last = m_raw;
        }
        finish();
    }

    void cancel() override { finish(); }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (m_active && stringLength() > 0 && m_raw != m_last) {  // the smoothing string
            const QPointF a = t.map(m_last), b = t.map(m_raw);
            p.setPen(QPen(QColor(0, 0, 0, 140), 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a, b);
            p.setPen(QPen(QColor(255, 120, 200), 1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a, b);
            p.setBrush(QColor(255, 120, 200));
            p.drawEllipse(b, 2.5, 2.5);
        }
        if ((m_mode != Clone && m_mode != Heal) || !m_hasSource)
            return;
        const QPointF src = m_offsetSet ? (m_active ? m_last : m_hover) + QPointF(m_offset) : QPointF(m_sourcePoint);
        const QPointF c = t.map(src);
        const qreal r = std::max<qreal>(3, m_settings->size / 2.0 * t.m11());
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
        if (!m_active)
            return;
        m_active = false;
        if (m_mode == SpotHeal && !m_strokeRect.isEmpty()) {
            // No source to pick: find a nearby patch whose surroundings look most alike.
            m_source = m_base;
            m_offset = findSpotSource();
        }
        if (m_mode == Heal || m_mode == SpotHeal)
            applyHealing();
        m_lastEnd = m_last;
        m_lastDoc = m_doc;
        m_doc->commit(name(id()), m_before);
        m_doc->notifyStructureChanged();
        m_before = DocState();
        m_base = m_buffer = m_source = m_sample = QImage();
    }

    // Spot healing: tries offsets around the stroke and returns the one whose surroundings
    // best match the stroke's surroundings, staying inside the image.
    QPoint findSpotSource() const
    {
        const int margin = std::max(8, m_settings->size);
        const QRect area = m_strokeRect.adjusted(-margin, -margin, margin, margin) & m_doc->rect();
        const QImage coverage = m_buffer.copy(area).convertToFormat(QImage::Format_Alpha8);
        const int reach = std::max(m_strokeRect.width(), m_strokeRect.height()) + 4;
        const int step = std::max(1, std::min(area.width(), area.height()) / 40);
        QPoint best(reach, 0);
        double bestScore = std::numeric_limits<double>::max();
        for (double scale : {1.15, 1.6, 2.2}) {
            for (int k = 0; k < 16; ++k) {
                const double a = qDegreesToRadians(k * 22.5);
                const QPoint o(qRound(std::cos(a) * reach * scale), qRound(std::sin(a) * reach * scale));
                if (!m_doc->rect().contains(area.translated(o)))
                    continue;
                double score = 0;
                for (int y = 0; y < area.height(); y += step) {
                    const uchar *cov = coverage.constScanLine(y);
                    const QRgb *d = reinterpret_cast<const QRgb *>(m_base.constScanLine(area.top() + y)) + area.left();
                    const QRgb *src = reinterpret_cast<const QRgb *>(m_base.constScanLine(area.top() + y + o.y())) + area.left() + o.x();
                    for (int x = 0; x < area.width(); x += step) {
                        if (cov[x])
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

    // The target's pixels in `r` with this tool's adjustment applied at full strength.
    QImage adjustedRegion(const QRect &r) const
    {
        switch (m_mode) {
        case DodgeMode:
        case BurnMode:
            return Filters::dodgeBurn(m_base.copy(r), m_mode == BurnMode, m_settings->toneRange);
        case SpongeMode:
            return Filters::hueSaturation(m_base.copy(r), 0, m_settings->spongeSaturate ? 60 : -60, 0);
        case BlurMode:
        case SharpenMode: {
            const double sigma = std::max(1.5, m_settings->size * 0.06);
            const int m = int(std::ceil(sigma * 3)) + 2;  // the filter needs pixels around the area
            const QRect er = r.adjusted(-m, -m, m, m) & m_doc->rect();
            const QImage around = m_base.copy(er);
            const QImage filtered = m_mode == BlurMode ? Filters::gaussianBlur(around, sigma)
                                                       : Filters::unsharpMask(around, 120, 1.5, 2);
            return filtered.copy(r.translated(-er.topLeft()));
        }
        default:
            return {};
        }
    }

    // Replaces the cloned pixels with ones that blend into their surroundings.
    void applyHealing()
    {
        if (m_strokeRect.isEmpty())
            return;
        const int margin = std::max(8, m_settings->size);
        const QRect r = m_strokeRect.adjusted(-margin, -margin, margin, margin) & m_doc->rect();
        const QImage src = m_source.copy(r.translated(m_offset));
        const QImage dst = m_base.copy(r);
        const QImage coverage = m_buffer.copy(r).convertToFormat(QImage::Format_Alpha8);
        QImage healed = Filters::heal(src, dst, coverage, std::max(3.0, m_settings->size / 3.0));
        QPainter hp(&healed);
        hp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        hp.drawImage(0, 0, coverage);
        hp.end();
        QPainter bp(&m_buffer);
        bp.setCompositionMode(QPainter::CompositionMode_Source);
        bp.drawImage(r.topLeft(), healed);
        bp.end();
        composite(r);
    }

    void stroke(const QPointF &a, const QPointF &b, bool first, qreal pa, qreal pb)
    {
        const qreal baseRadius = std::max<qreal>(0.5, m_settings->size / 2.0);
        const qreal spacing = std::max<qreal>(1.0, m_settings->size * 0.12);
        if (m_mode == SmudgeMode) {
            QRect changed;
            auto smudge = [&](const QPointF &c, qreal pressure) {
                const qreal radius = m_settings->pressureSize ? std::max<qreal>(0.5, baseRadius * pressure) : baseRadius;
                changed |= smudgeDab(c, radius, m_settings->pressureOpacity ? pressure : 1.0);
            };
            if (first)
                smudge(a, pa);
            const QLineF line(a, b);
            const qreal len = line.length();
            qreal t = spacing - m_residual;
            while (t <= len) {
                smudge(line.pointAt(t / len), pa + (pb - pa) * t / len);
                t += spacing;
            }
            m_residual = len - (t - spacing);
            changed &= m_doc->rect();
            if (!changed.isEmpty())
                m_doc->notifyImageChanged(changed);
            return;
        }
        QPainter p(&m_buffer);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QRectF dirty;
        auto dab = [&](const QPointF &c, qreal pressure) {
            const qreal radius = m_settings->pressureSize ? std::max<qreal>(0.5, baseRadius * pressure) : baseRadius;
            const qreal alpha = m_settings->pressureOpacity ? pressure : 1.0;
            drawDab(p, c, radius, alpha);
            dirty |= QRectF(c.x() - radius - 2, c.y() - radius - 2, 2 * radius + 4, 2 * radius + 4);
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
        p.end();
        if (!dirty.isEmpty())
            composite(dirty.toAlignedRect());
    }

    void drawDab(QPainter &p, const QPointF &c, qreal radius, qreal alpha)
    {
        const qreal hard = m_settings->hardness / 100.0;
        if (m_mode == Clone || m_mode == Heal) {
            const QRect dr = QRectF(c.x() - radius - 1, c.y() - radius - 1, 2 * radius + 2, 2 * radius + 2).toAlignedRect();
            QImage d(dr.size(), QImage::Format_ARGB32_Premultiplied);
            d.fill(Qt::transparent);
            QPainter q(&d);
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
            p.setOpacity(alpha);
            p.drawImage(dr.topLeft(), d);
            p.setOpacity(1.0);
            return;
        }

        // Only Paint uses a color; for the other modes the dabs just record coverage.
        QColor color = m_mode == Paint ? m_settings->foreground() : QColor(Qt::black);
        color.setAlphaF(alpha);
        if (hard >= 0.999) {
            p.setBrush(color);
        } else {
            QColor clear = color;
            clear.setAlpha(0);
            QRadialGradient g(c, radius);
            g.setColorAt(0, color);
            g.setColorAt(hard, color);
            g.setColorAt(1, clear);
            p.setBrush(g);
        }
        p.drawEllipse(c, radius, radius);
    }

    // Smudge: blends the paint picked up so far into the canvas under the dab, then
    // picks up some of the result. Returns the changed rectangle.
    QRect smudgeDab(const QPointF &c, qreal radius, qreal alpha)
    {
        const int size = m_smudgeSize;
        const QPoint topLeft(qRound(c.x() - size / 2.0), qRound(c.y() - size / 2.0));
        const QRect area(topLeft, QSize(size, size));
        QImage &target = m_doc->targetImage();
        const QImage current = target.copy(area);
        if (m_sample.isNull()) {
            m_sample = current;
            return QRect();
        }
        QImage coverage(size, size, QImage::Format_Alpha8);
        coverage.fill(0);
        {
            QPainter q(&coverage);
            q.setRenderHint(QPainter::Antialiasing);
            q.setPen(Qt::NoPen);
            const QPointF local = c - QPointF(topLeft);
            const qreal hard = m_settings->hardness / 100.0;
            if (hard >= 0.999) {
                q.setBrush(Qt::black);
            } else {
                QRadialGradient g(local, radius);
                g.setColorAt(0, Qt::black);
                g.setColorAt(hard, Qt::black);
                g.setColorAt(1, Qt::transparent);
                q.setBrush(g);
            }
            q.drawEllipse(local, radius, radius);
            if (m_doc->hasSelection()) {
                q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                q.drawImage(QPoint(0, 0), m_doc->selection(), area);
            }
        }
        const qreal strength = m_settings->opacity / 100.0 * alpha;
        QImage result = current;
        for (int y = 0; y < size; ++y) {
            QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
            const QRgb *smp = reinterpret_cast<const QRgb *>(m_sample.constScanLine(y));
            const uchar *cov = coverage.constScanLine(y);
            for (int x = 0; x < size; ++x) {
                const int k = int(cov[x] * strength);
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

    void composite(QRect r)
    {
        r &= m_doc->rect();
        if (r.isEmpty())
            return;
        m_strokeRect |= r;
        QImage stroke = selectionMasked(m_buffer.copy(r), m_doc, r);
        if (m_mode == SpotHeal && m_active) {
            // While painting, show where the healing will happen as a dark tint.
            QPainter p(&m_doc->targetImage());
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(r.topLeft(), m_base, r);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setOpacity(0.4);
            p.drawImage(r.topLeft(), stroke);
            p.end();
            m_doc->notifyImageChanged(r);
            return;
        }
        if (m_mode == DodgeMode || m_mode == BurnMode || m_mode == BlurMode || m_mode == SharpenMode
            || m_mode == SpongeMode) {
            // The adjusted pixels, revealed where the stroke has coverage.
            QImage adjusted = adjustedRegion(r);
            QPainter q(&adjusted);
            q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            q.drawImage(0, 0, stroke);
            q.end();
            stroke = adjusted;
        }
        QPainter p(&m_doc->targetImage());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), m_base, r);
        p.setOpacity(m_settings->opacity / 100.0);
        p.setCompositionMode(m_mode == Erase ? QPainter::CompositionMode_DestinationOut
                                             : QPainter::CompositionMode_SourceOver);
        p.drawImage(r.topLeft(), stroke);
        p.end();
        m_doc->notifyImageChanged(r);
    }

    Mode m_mode;
    bool m_active = false;
    DocState m_before;
    QImage m_base;    // the target before the stroke
    QImage m_buffer;  // the stroke at full opacity; composited with the tool opacity
    QImage m_source;  // clone/heal source pixels
    QImage m_sample;  // paint carried by the smudge tool
    int m_smudgeSize = 0;
    QRect m_strokeRect;
    QPointF m_last, m_lastEnd, m_hover;
    QPointF m_raw;  // the pointer; m_last trails it by the smoothing string
    qreal m_lastPressure = 1.0;
    const Document *m_lastDoc = nullptr;
    qreal m_residual = 0;
    QPoint m_sourcePoint, m_offset;
    bool m_hasSource = false;
    bool m_offsetSet = false;
};

// ---------------------------------------------------------------------------

class ShapeTool : public Tool {
public:
    ShapeTool(ToolSettings *s, Id id) : Tool(s), m_id(id) {}
    Id id() const override { return m_id; }
    bool snapsToGuides() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_edit.begin(m_doc);
        m_start = m_end = e.pos;
    }
    void move(const ToolEvent &e) override
    {
        if (!m_edit.active())
            return;
        m_end = e.pos;
        if (e.modifiers & Qt::ShiftModifier)
            m_end = m_id == LineShape ? snap45(m_start, e.pos) : snapSquare(m_start, e.pos);
        redraw();
    }
    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton || !m_edit.active())
            return;
        if (m_start == m_end)
            m_edit.abort();
        else
            m_edit.commit(name(m_id));
    }
    void cancel() override { m_edit.commit(name(m_id)); }

private:
    void redraw()
    {
        const qreal w = m_settings->size;
        const QColor c = m_settings->foreground();
        const bool fill = m_settings->fillShape && m_id != LineShape;
        const bool aa = m_settings->antialias;
        QRectF r = QRectF(m_start, m_end).normalized();
        if (m_id != LineShape) {
            // On the pixel grid, so straight edges come out crisp (a soft edge would leave a gap
            // when filled): corners on whole pixels, an odd-width outline centered on pixels.
            r = QRectF(QPointF(std::round(r.left()), std::round(r.top())), QPointF(std::round(r.right()), std::round(r.bottom())));
            if (int(std::round(w)) % 2 == 1)
                r.translate(0.5, 0.5);
        }
        m_edit.render(r.adjusted(-w - 2, -w - 2, w + 2, w + 2).toAlignedRect(), m_settings->opacity / 100.0,
                      [&](QPainter &p) {
            p.setRenderHint(QPainter::Antialiasing, aa);
            p.setPen(QPen(c, w, Qt::SolidLine, m_id == LineShape ? Qt::RoundCap : Qt::SquareCap, Qt::MiterJoin));
            p.setBrush(fill ? QBrush(c) : QBrush(Qt::NoBrush));
            if (m_id == LineShape)
                p.drawLine(m_start, m_end);
            else if (m_id == RectShape)
                p.drawRect(r);
            else
                p.drawEllipse(r);
        });
    }

    Id m_id;
    PreviewEdit m_edit;
    QPointF m_start, m_end;
};

// ---------------------------------------------------------------------------

class GradientTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Gradient; }
    bool snapsToGuides() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_edit.begin(m_doc);
        m_start = m_end = e.pos;
    }
    void move(const ToolEvent &e) override
    {
        if (!m_edit.active())
            return;
        m_end = (e.modifiers & Qt::ShiftModifier) ? snap45(m_start, e.pos) : e.pos;
        const QColor a = m_settings->foreground(), b = m_settings->background();
        const QRect area = m_doc->rect();
        m_edit.render(area, m_settings->opacity / 100.0, [&](QPainter &p) {
            if (m_settings->radial) {
                QRadialGradient g(m_start, std::max<qreal>(1.0, QLineF(m_start, m_end).length()));
                g.setColorAt(0, a);
                g.setColorAt(1, b);
                p.fillRect(area, g);
            } else {
                QLinearGradient g(m_start, m_end);
                g.setColorAt(0, a);
                g.setColorAt(1, b);
                p.fillRect(area, g);
            }
        });
        m_canvas->update();
    }
    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton || !m_edit.active())
            return;
        if (m_start == m_end)
            m_edit.abort();
        else
            m_edit.commit(name(Gradient));
        m_canvas->update();
    }
    void cancel() override { m_edit.commit(name(Gradient)); }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (!m_edit.active())
            return;
        const QLineF l = t.map(QLineF(m_start, m_end));
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawLine(l);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(l);
    }

private:
    PreviewEdit m_edit;
    QPointF m_start, m_end;
};

// ---------------------------------------------------------------------------

class SelectTool : public Tool {
public:
    SelectTool(ToolSettings *s, Id id) : Tool(s), m_id(id) {}
    Id id() const override { return m_id; }
    bool snapsToGuides() const override { return m_id != Lasso; }  // rectangles and ellipses

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_drag = true;
        m_op = selectionOp(e.modifiers);
        m_start = m_end = e.pos;
        m_lasso = QPolygonF{e.pos};
    }
    void move(const ToolEvent &e) override
    {
        if (!m_drag)
            return;
        m_end = e.pos;
        if (m_id == Lasso)
            m_lasso << e.pos;
        m_canvas->update();
    }
    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton || !m_drag)
            return;
        m_drag = false;
        const QPainterPath path = currentPath();
        const QRectF b = path.boundingRect();
        if (b.width() < 1 || b.height() < 1) {
            // A plain click clears the selection.
            if (m_op == SelectionOp::Replace)
                m_doc->deselect();
        } else {
            m_doc->selectPath(path, m_op, m_settings->antialias && m_id != RectSelect, name(m_id));
        }
        m_canvas->update();
    }
    void cancel() override { m_drag = false; }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (m_drag)
            drawOutline(p, t.map(currentPath()));
    }

private:
    QPainterPath currentPath() const
    {
        QPainterPath path;
        if (m_id == Lasso) {
            path.addPolygon(m_lasso);
            path.closeSubpath();
            return path;
        }
        const QRectF r = QRectF(m_start, m_end).normalized();
        const QRectF snapped(QPointF(std::floor(r.left()), std::floor(r.top())),
                             QPointF(std::ceil(r.right()), std::ceil(r.bottom())));
        if (m_id == RectSelect)
            path.addRect(snapped);
        else
            path.addEllipse(snapped);
        return path;
    }

    Id m_id;
    bool m_drag = false;
    SelectionOp m_op = SelectionOp::Replace;
    QPointF m_start, m_end;
    QPolygonF m_lasso;
};

// ---------------------------------------------------------------------------

// Click corner points; double-click, click the first point or press Enter to close.
class PolygonLassoTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return PolyLasso; }
    bool snapsToGuides() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (m_ignoreNextPress) {  // the press that follows a closing double-click
            m_ignoreNextPress = false;
            return;
        }
        if (!m_active) {
            m_active = true;
            m_op = selectionOp(e.modifiers);
            m_points = QPolygonF{e.pos};
        } else if (m_points.size() >= 3 && nearStart(e.widgetPos)) {
            close();
            return;
        } else {
            m_points << e.pos;
        }
        m_hover = e.pos;
        m_canvas->update();
    }

    void move(const ToolEvent &e) override
    {
        m_hover = e.pos;
        if (m_active)
            m_canvas->update();
    }

    void doubleClick(const ToolEvent &) override
    {
        if (!m_active)
            return;
        close();
        m_ignoreNextPress = true;
    }

    bool keyPress(QKeyEvent *e) override
    {
        if (!m_active)
            return false;
        switch (e->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            close();
            return true;
        case Qt::Key_Escape:
            cancel();
            m_canvas->update();
            return true;
        case Qt::Key_Backspace:
            m_points.removeLast();
            if (m_points.isEmpty())
                m_active = false;
            m_canvas->update();
            return true;
        default:
            return false;
        }
    }

    void cancel() override
    {
        m_active = false;
        m_points.clear();
    }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (!m_active)
            return;
        QPainterPath path;
        path.addPolygon(t.map(m_points));
        path.lineTo(t.map(m_hover));
        drawOutline(p, path);
        if (m_points.size() >= 3 && nearStart(t.map(m_hover))) {  // hint: clicking here closes
            p.setPen(QPen(Qt::white, 1.5));
            p.setBrush(QColor(42, 130, 218));
            p.drawEllipse(t.map(m_points.first()), 5, 5);
        }
    }

private:
    bool nearStart(const QPointF &widgetPos) const
    {
        return QLineF(m_canvas->imageToWidget().map(m_points.first()), widgetPos).length() <= 8;
    }

    void close()
    {
        if (m_points.size() >= 3) {
            QPainterPath path;
            path.addPolygon(m_points);
            path.closeSubpath();
            m_doc->selectPath(path, m_op, m_settings->antialias, name(PolyLasso));
        }
        cancel();
        m_canvas->update();
    }

    bool m_active = false;
    bool m_ignoreNextPress = false;
    SelectionOp m_op = SelectionOp::Replace;
    QPolygonF m_points;
    QPointF m_hover;
};

class MagicWandTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return MagicWand; }

    void press(const ToolEvent &e) override
    {
        const QPoint pt = pixelAt(e.pos);
        if (e.button != Qt::LeftButton || !m_doc->rect().contains(pt))
            return;
        const bool merged = m_settings->sampleMerged || m_doc->activeLayer().isAdjustment();
        const QImage sample = merged ? m_doc->flattened() : m_doc->activeLayer().image;
        const QImage mask = Filters::floodMask(sample, pt, m_settings->tolerance, m_settings->contiguous);
        m_doc->selectMask(mask, selectionOp(e.modifiers), name(MagicWand));
    }
};

class FillTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Fill; }

    void press(const ToolEvent &e) override
    {
        const QPoint pt = pixelAt(e.pos);
        if (e.button != Qt::LeftButton || !m_doc->rect().contains(pt))
            return;
        if (m_doc->hasSelection() && m_doc->selection().constScanLine(pt.y())[pt.x()] == 0)
            return;  // clicked outside the selection
        m_doc->prepareForPixelEdit();
        const DocState before = m_doc->state();
        const QImage sample = m_settings->sampleMerged ? m_doc->flattened() : m_doc->targetImage();
        const QImage region = Filters::fillCoverage(
            sample, Filters::floodMask(sample, pt, m_settings->tolerance, m_settings->contiguous), pt);

        QImage paint(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
        paint.fill(m_settings->foreground());
        {
            QPainter p(&paint);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, region);
            if (m_doc->hasSelection())
                p.drawImage(0, 0, m_doc->selection());
        }
        QPainter p(&m_doc->targetImage());
        p.setOpacity(m_settings->opacity / 100.0);
        p.drawImage(0, 0, paint);
        p.end();
        m_doc->commit(name(Fill), before);
        m_doc->notifyImageChanged();
        m_doc->notifyStructureChanged();
    }
};

class EyedropperTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Eyedropper; }

    void press(const ToolEvent &e) override { pick(e, e.button); }
    void move(const ToolEvent &e) override
    {
        if (e.buttons & Qt::LeftButton)
            pick(e, Qt::LeftButton);
        else if (e.buttons & Qt::RightButton)
            pick(e, Qt::RightButton);
    }

private:
    void pick(const ToolEvent &e, Qt::MouseButton button)
    {
        const QPoint pt = pixelAt(e.pos);
        if (!m_doc->rect().contains(pt) || button == Qt::NoButton)
            return;
        QColor c = m_doc->composite(QRect(pt, QSize(1, 1))).pixelColor(0, 0);
        if (c.alpha() == 0)
            return;
        c.setAlpha(255);
        if (button == Qt::RightButton || (e.modifiers & Qt::AltModifier))
            m_settings->setBackground(c);
        else
            m_settings->setForeground(c);
    }
};

// ---------------------------------------------------------------------------
// Lifts the selected pixels (or the whole target) off the target so they can
// be moved or transformed over the remaining "hole".

struct FloatingPixels {
    QImage floating, hole, selection;

    void lift(const Document *doc)
    {
        const QImage &img = doc->targetImage();
        if (doc->hasSelection()) {
            selection = doc->selection();
            floating = img.copy();
            QPainter p(&floating);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, selection);
            p.end();
            hole = img.copy();
            p.begin(&hole);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(0, 0, selection);
        } else {
            selection = QImage();
            floating = img;
            hole = QImage(img.size(), QImage::Format_ARGB32_Premultiplied);
            hole.fill(Qt::transparent);
        }
    }

    // Draws the floating pixels over the hole with transform `t` applied.
    void place(Document *doc, const QTransform &t, const QRect &source = QRect())
    {
        QImage out = hole.copy();
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform, t.type() > QTransform::TxTranslate);
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(t);
        const QRect src = source.isNull() ? floating.rect() : source;
        p.drawImage(src.topLeft(), floating, src);
        p.end();
        doc->targetImage() = out;
        if (!selection.isNull()) {
            QImage sel(selection.size(), QImage::Format_Alpha8);
            sel.fill(0);
            QPainter sp(&sel);
            sp.setRenderHint(QPainter::SmoothPixmapTransform);
            sp.setTransform(t);
            sp.drawImage(src.topLeft(), selection, src);
            sp.end();
            doc->setSelection(sel);
        }
        doc->notifyImageChanged();
    }

    void clear() { floating = hole = selection = QImage(); }
};

class MoveTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Move; }
    QCursor cursor() const override { return Qt::SizeAllCursor; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_drag = true;
        m_start = e.pos;
        begin();
    }
    void move(const ToolEvent &e) override
    {
        if (!m_drag)
            return;
        QPointF d = e.pos - m_start;
        bool moveX = true, moveY = true;
        if (e.modifiers & Qt::ShiftModifier) {
            if (std::abs(d.x()) > std::abs(d.y())) { d.setY(0); moveY = false; } else { d.setX(0); moveX = false; }
        }
        // The content's edges and center snap to guides and to the canvas edges and center.
        m_canvas->clearSnapHighlight();
        if (!m_bounds.isEmpty()) {
            const QRectF b = QRectF(m_bounds).translated(d);
            if (moveX)
                d.rx() += m_canvas->snapOffset(true, {b.left(), b.center().x(), b.right()});
            if (moveY)
                d.ry() += m_canvas->snapOffset(false, {b.top(), b.center().y(), b.bottom()});
        }
        const QPoint di(qRound(d.x()), qRound(d.y()));
        if (di != m_delta)
            apply(di);
    }
    void release(const ToolEvent &e) override
    {
        if (e.button == Qt::LeftButton && m_drag) {
            m_drag = false;
            end();
        }
    }
    void cancel() override
    {
        if (m_drag) {
            m_drag = false;
            end();
        }
    }
    bool keyPress(QKeyEvent *e) override
    {
        if (m_drag)
            return false;
        const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
        QPoint d;
        switch (e->key()) {
        case Qt::Key_Left: d = {-step, 0}; break;
        case Qt::Key_Right: d = {step, 0}; break;
        case Qt::Key_Up: d = {0, -step}; break;
        case Qt::Key_Down: d = {0, step}; break;
        default: return false;
        }
        begin();
        apply(d);
        end();
        return true;
    }

private:
    void begin()
    {
        m_before = m_doc->state();
        m_moved = false;
        m_delta = QPoint();
        m_groupMembers.clear();
        m_bounds = QRect();
        if (m_doc->activeLayer().isGroup() && !m_doc->editingMask()) {
            // Moving a group moves every layer inside it.
            const int header = m_doc->activeIndex();
            for (int i = m_doc->groupEndFor(header) + 1; i < header; ++i) {
                if (m_doc->layer(i).kind == LayerKind::Normal) {
                    m_groupMembers << i;
                    if (m_settings->snap)
                        m_bounds |= alphaBounds(m_doc->layer(i).image);
                }
            }
            return;
        }
        if (m_settings->snap && !m_doc->editingMask()) {  // what is being moved, for snapping
            m_bounds = alphaBounds(m_doc->targetImage());
            if (m_doc->hasSelection())
                m_bounds &= m_doc->selectionBounds();
        }
        // Moving part of a text layer turns it into pixels; moving all of it keeps it editable.
        if (m_doc->hasSelection()) {
            m_doc->prepareForPixelEdit();
            m_before = m_doc->state();
        }
        m_pixels.lift(m_doc);
    }

    void apply(const QPoint &d)
    {
        m_delta = d;
        m_moved = true;
        if (!m_groupMembers.isEmpty()) {
            auto shift = [&](const QImage &img, const QColor &fill) {
                QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
                out.fill(fill);
                QPainter p(&out);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(d, img);
                return out;
            };
            for (int i : std::as_const(m_groupMembers)) {
                const Layer &orig = m_before.layers.at(i);
                Layer &l = m_doc->layer(i);
                l.image = shift(orig.image, Qt::transparent);
                if (!orig.mask.isNull())
                    l.mask = shift(orig.mask, Qt::white);
            }
            m_doc->notifyImageChanged();
            return;
        }
        m_pixels.place(m_doc, QTransform::fromTranslate(d.x(), d.y()));
    }

    void end()
    {
        if (m_moved) {
            for (int i : std::as_const(m_groupMembers))
                if (m_doc->layer(i).isText())
                    m_doc->layer(i).text.pos = m_before.layers.at(i).text.pos + m_delta;
            if (m_groupMembers.isEmpty() && !m_doc->editingMask() && m_doc->activeLayer().isText())
                m_doc->activeLayer().text.pos += m_delta;
            m_doc->commit(name(Move), m_before);
            m_doc->notifyStructureChanged();
        }
        m_before = DocState();
        m_pixels.clear();
    }

    bool m_drag = false;
    bool m_moved = false;
    QPointF m_start;
    QPoint m_delta;
    DocState m_before;
    FloatingPixels m_pixels;
    QList<int> m_groupMembers;  // layers moved together when a group is active
    QRect m_bounds;  // the content being moved, for snapping
};

// ---------------------------------------------------------------------------
// Free transform: scale (handles), rotate (drag outside the box) and move
// (drag inside) the layer or the selected pixels. Enter applies, Esc cancels.

class TransformTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Transform; }
    QCursor cursor() const override { return Qt::ArrowCursor; }

    void activated() override { begin(); }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (!m_active)
            begin();
        if (!m_active)
            return;
        m_press = e.pos;
        m_c0 = m_center;
        m_w0 = m_w;
        m_h0 = m_h;
        m_a0 = m_angle;
        m_handle = handleAt(e.widgetPos);
        if (m_handle >= 0)
            m_drag = DragHandle;
        else if (insideBox(e.pos))
            m_drag = DragMove;
        else
            m_drag = DragRotate;
    }

    void move(const ToolEvent &e) override
    {
        if (!m_active || m_drag == DragNone)
            return;
        const bool shift = e.modifiers & Qt::ShiftModifier;
        if (m_drag == DragMove) {
            m_center = m_c0 + (e.pos - m_press);
        } else if (m_drag == DragRotate) {
            const QPointF a = m_press - m_c0, b = e.pos - m_c0;
            qreal angle = m_a0 + qRadiansToDegrees(std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x()));
            if (shift)
                angle = std::round(angle / 15.0) * 15.0;
            m_angle = angle;
        } else {
            static const QPointF dirs[8] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
            const QPointF dir = dirs[m_handle];
            const QPointF local = rotated(e.pos - m_c0, -m_a0);
            const QPointF anchor(-dir.x() * m_w0 / 2, -dir.y() * m_h0 / 2);
            qreal w = dir.x() != 0 ? std::max<qreal>(1, (local.x() - anchor.x()) * dir.x()) : m_w0;
            qreal h = dir.y() != 0 ? std::max<qreal>(1, (local.y() - anchor.y()) * dir.y()) : m_h0;
            if (shift && dir.x() != 0 && dir.y() != 0) {
                const qreal s = std::max(w / m_w0, h / m_h0);
                w = m_w0 * s;
                h = m_h0 * s;
            }
            const QPointF centerLocal(dir.x() != 0 ? anchor.x() + dir.x() * w / 2 : 0,
                                      dir.y() != 0 ? anchor.y() + dir.y() * h / 2 : 0);
            m_center = m_c0 + rotated(centerLocal, m_a0);
            m_w = w;
            m_h = h;
        }
        m_modified = true;
        m_pixels.place(m_doc, transform(), m_source.toAlignedRect());
    }

    void release(const ToolEvent &) override { m_drag = DragNone; }
    void doubleClick(const ToolEvent &) override { commit(); }

    bool keyPress(QKeyEvent *e) override
    {
        if (!m_active)
            return false;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            commit();
            return true;
        }
        if (e->key() == Qt::Key_Escape) {
            revert();
            return true;
        }
        return false;
    }

    void cancel() override { commit(); }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (!m_active)
            return;
        QPolygonF box;
        for (int i : {0, 2, 4, 6})
            box << t.map(handlePos(i));
        QPainterPath path;
        path.addPolygon(box);
        path.closeSubpath();
        drawOutline(p, path);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        for (int i = 0; i < 8; ++i) {
            const QPointF c = t.map(handlePos(i));
            p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
        }
        const QPointF c = t.map(m_center);
        p.drawEllipse(c, 3, 3);
    }

private:
    enum Drag { DragNone, DragMove, DragRotate, DragHandle };

    void begin()
    {
        if (m_active || !m_doc || !m_doc->canEditPixels())
            return;
        m_doc->prepareForPixelEdit();
        const QImage &img = m_doc->targetImage();
        QRect bounds = m_doc->editingMask() ? m_doc->rect() : alphaBounds(img);
        if (m_doc->hasSelection())
            bounds = m_doc->selectionBounds();
        if (bounds.isEmpty()) {
            showTip(QObject::tr("Nothing to transform on this layer."));
            return;
        }
        m_before = m_doc->state();
        m_pixels.lift(m_doc);
        m_source = QRectF(bounds);
        m_center = m_source.center();
        m_w = m_source.width();
        m_h = m_source.height();
        m_angle = 0;
        m_active = true;
        m_modified = false;
        m_drag = DragNone;
        if (m_canvas)
            m_canvas->update();
    }

    void commit()
    {
        if (!m_active)
            return;
        m_active = false;
        m_drag = DragNone;
        if (m_modified) {
            m_doc->commit(name(Transform), m_before);
            m_doc->notifyStructureChanged();
        }
        m_pixels.clear();
        m_before = DocState();
        m_canvas->update();
    }

    void revert()
    {
        if (!m_active)
            return;
        m_active = false;
        m_drag = DragNone;
        if (m_modified)
            m_doc->setState(m_before);
        m_pixels.clear();
        m_before = DocState();
        m_canvas->update();
    }

    QTransform transform() const
    {
        QTransform t;
        t.translate(m_center.x(), m_center.y());
        t.rotate(m_angle);
        t.scale(m_w / m_source.width(), m_h / m_source.height());
        t.translate(-m_source.center().x(), -m_source.center().y());
        return t;
    }

    QPointF handlePos(int i) const
    {
        static const QPointF dirs[8] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
        return m_center + rotated(QPointF(dirs[i].x() * m_w / 2, dirs[i].y() * m_h / 2), m_angle);
    }

    int handleAt(const QPointF &widgetPos) const
    {
        for (int i = 0; i < 8; ++i)
            if (QLineF(m_canvas->imageToWidget().map(handlePos(i)), widgetPos).length() <= 7)
                return i;
        return -1;
    }

    bool insideBox(const QPointF &imagePos) const
    {
        const QPointF local = rotated(imagePos - m_center, -m_angle);
        return std::abs(local.x()) <= m_w / 2 && std::abs(local.y()) <= m_h / 2;
    }

    bool m_active = false;
    bool m_modified = false;
    DocState m_before;
    FloatingPixels m_pixels;
    QRectF m_source;
    QPointF m_center;
    qreal m_w = 0, m_h = 0, m_angle = 0;
    Drag m_drag = DragNone;
    int m_handle = -1;
    QPointF m_press, m_c0;
    qreal m_w0 = 0, m_h0 = 0, m_a0 = 0;
};

// ---------------------------------------------------------------------------

// Crop frame that can be moved (drag inside), resized (handles, keeping the chosen
// aspect ratio) and rotated to straighten the image (drag outside).
// Enter or double-click applies, Esc cancels.
class CropTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Crop; }
    bool snapsToGuides() const override { return true; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_press = e.pos;
        m_c0 = m_center;
        m_w0 = m_w;
        m_h0 = m_h;
        m_a0 = m_angle;
        if (!m_framed) {
            m_drag = Create;
            m_framed = true;
            m_center = e.pos;
            m_w = m_h = 0;
            m_angle = 0;
        } else if ((m_handle = handleAt(e.widgetPos)) >= 0) {
            m_drag = Resize;
        } else if (inside(e.pos)) {
            m_drag = MoveFrame;
        } else {
            m_drag = Rotate;
        }
    }

    void move(const ToolEvent &e) override
    {
        if (m_drag == None)
            return;
        const double ratio = aspect();
        if (m_drag == Create) {
            QPointF d = e.pos - m_press;
            if (ratio > 0) {  // keep the chosen shape
                const double w = std::max(std::abs(d.x()), std::abs(d.y()) * ratio);
                d = QPointF(d.x() < 0 ? -w : w, (d.y() < 0 ? -w : w) / ratio);
            }
            m_center = m_press + d / 2;
            m_w = std::abs(d.x());
            m_h = std::abs(d.y());
        } else if (m_drag == MoveFrame) {
            m_center = m_c0 + (e.pos - m_press);
        } else if (m_drag == Rotate) {
            const QPointF a = m_press - m_c0, b = e.pos - m_c0;
            m_angle = m_a0 + qRadiansToDegrees(std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x()));
        } else {
            static const QPointF dirs[8] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
            const QPointF dir = dirs[m_handle];
            const QPointF local = rotated(e.pos - m_c0, -m_a0);
            const QPointF anchor(-dir.x() * m_w0 / 2, -dir.y() * m_h0 / 2);
            double w = dir.x() != 0 ? std::max(1.0, (local.x() - anchor.x()) * dir.x()) : m_w0;
            double h = dir.y() != 0 ? std::max(1.0, (local.y() - anchor.y()) * dir.y()) : m_h0;
            if (ratio > 0) {
                if (dir.x() != 0 && dir.y() != 0)
                    w = std::max(w, h * ratio);
                else if (dir.y() != 0)
                    w = h * ratio;
                h = w / ratio;
            }
            const QPointF centerLocal(dir.x() != 0 ? anchor.x() + dir.x() * w / 2 : 0,
                                      dir.y() != 0 ? anchor.y() + dir.y() * h / 2 : 0);
            m_center = m_c0 + rotated(centerLocal, m_a0);
            m_w = w;
            m_h = h;
        }
        m_canvas->update();
    }

    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (m_drag == Create && (m_w < 2 || m_h < 2))
            m_framed = false;
        m_drag = None;
        m_canvas->update();
    }

    void doubleClick(const ToolEvent &) override { apply(); }

    bool keyPress(QKeyEvent *e) override
    {
        if (!m_framed)
            return false;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            apply();
            return true;
        }
        if (e->key() == Qt::Key_Escape) {
            cancel();
            m_canvas->update();
            return true;
        }
        return false;
    }

    void cancel() override
    {
        m_framed = false;
        m_drag = None;
    }

    void settingsChanged() override
    {
        // A newly chosen aspect ratio reshapes the current frame around its center.
        const double ratio = aspect();
        if (m_framed && ratio > 0 && m_w > 0) {
            m_h = m_w / ratio;
            m_canvas->update();
        }
    }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (!m_framed || m_w < 1 || m_h < 1)
            return;
        QPolygonF frame;
        for (int i : {0, 2, 4, 6})
            frame << t.map(handlePos(i));
        QPainterPath outside;
        outside.addRect(t.mapRect(QRectF(m_doc->rect())));
        outside.addPolygon(frame);
        outside.closeSubpath();
        p.fillPath(outside, QColor(0, 0, 0, 150));
        p.setPen(QPen(QColor(255, 255, 255, 90), 1));
        for (int i = 1; i < 3; ++i) {  // rule of thirds
            const double f = i / 3.0;
            p.drawLine(t.map(point(f, 0)), t.map(point(f, 1)));
            p.drawLine(t.map(point(0, f)), t.map(point(1, f)));
        }
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(frame);
        p.setBrush(Qt::white);
        p.setPen(QPen(Qt::black, 1));
        for (int i = 0; i < 8; ++i) {
            const QPointF c = t.map(handlePos(i));
            p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
        }
        if (m_drag == None) {
            QString text = QObject::tr("%1 × %2").arg(qRound(m_w)).arg(qRound(m_h));
            if (std::abs(m_angle) >= 0.05)
                text += QObject::tr(" · %1°").arg(m_angle, 0, 'f', 1);
            text += QObject::tr(" — Enter to crop, Esc to cancel");
            p.setPen(Qt::white);
            p.drawText(frame.boundingRect().bottomLeft() + QPointF(0, 18), text);
        }
    }

private:
    enum Drag { None, Create, MoveFrame, Resize, Rotate };

    double aspect() const
    {
        return m_settings->cropRatio < 0 ? double(m_doc->size().width()) / m_doc->size().height() : m_settings->cropRatio;
    }

    // A point of the frame: fx, fy from 0 (left/top) to 1 (right/bottom).
    QPointF point(double fx, double fy) const
    {
        return m_center + rotated(QPointF((fx - 0.5) * m_w, (fy - 0.5) * m_h), m_angle);
    }

    QPointF handlePos(int i) const
    {
        static const QPointF f[8] = {{0, 0}, {0.5, 0}, {1, 0}, {1, 0.5}, {1, 1}, {0.5, 1}, {0, 1}, {0, 0.5}};
        return point(f[i].x(), f[i].y());
    }

    int handleAt(const QPointF &widgetPos) const
    {
        for (int i = 0; i < 8; ++i)
            if (QLineF(m_canvas->imageToWidget().map(handlePos(i)), widgetPos).length() <= 8)
                return i;
        return -1;
    }

    bool inside(const QPointF &pos) const
    {
        const QPointF local = rotated(pos - m_center, -m_angle);
        return std::abs(local.x()) <= m_w / 2 && std::abs(local.y()) <= m_h / 2;
    }

    void apply()
    {
        if (!m_framed || m_w < 1 || m_h < 1)
            return;
        m_framed = false;
        m_doc->cropRotated(m_center, QSize(qRound(m_w), qRound(m_h)), m_angle);
        m_canvas->update();
    }

    bool m_framed = false;
    Drag m_drag = None;
    int m_handle = -1;
    QPointF m_center, m_press, m_c0;
    double m_w = 0, m_h = 0, m_angle = 0;
    double m_w0 = 0, m_h0 = 0, m_a0 = 0;
};

// ---------------------------------------------------------------------------

// Types text directly on the canvas. Clicking empty space starts a new text layer;
// clicking existing text edits it. Esc, clicking elsewhere or switching tools finishes,
// and the whole edit is one undo step.
class TextTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Text; }
    QCursor cursor() const override { return Qt::IBeamCursor; }
    bool capturesKeyboard() const override { return m_editing; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (m_editing && valid()) {
            const QRectF area = TextBox(m_text).bounds().adjusted(-6, -6, 6, 6);
            if (area.contains(e.pos)) {
                m_cursor = TextBox(m_text).hitTest(e.pos);
                if (!(e.modifiers & Qt::ShiftModifier))
                    m_anchor = m_cursor;
                m_selecting = true;
                m_canvas->update();
                return;
            }
        }
        finish();
        // Edit the topmost visible text layer under the click, or start a new one.
        for (int i = m_doc->layerCount() - 1; i >= 0; --i) {
            const Layer &l = m_doc->layer(i);
            if (l.visible && l.isText() && textBounds(l.text).adjusted(-4, -4, 4, 4).contains(e.pos)) {
                m_doc->setActiveIndex(i);
                m_before = m_doc->state();
                m_layer = i;
                m_text = l.text;
                m_isNew = false;
                m_editing = true;
                m_cursor = m_anchor = TextBox(m_text).hitTest(e.pos);
                m_selecting = true;
                m_settings->setTextStyle(m_text.font, m_text.antialias);
                m_canvas->update();
                return;
            }
        }
        m_before = m_doc->state();
        m_text = TextData();
        m_text.font = m_settings->font;
        m_text.color = m_settings->foreground();
        m_text.antialias = m_settings->antialias;
        // The click marks where the first line sits (its baseline), as in Photoshop.
        m_text.pos = e.pos - QPointF(0, QFontMetricsF(m_text.font).ascent());
        m_layer = m_doc->beginTextLayer(m_text);
        m_isNew = true;
        m_editing = true;
        m_cursor = m_anchor = 0;
        m_canvas->update();
    }

    void move(const ToolEvent &e) override
    {
        if (m_editing && m_selecting && (e.buttons & Qt::LeftButton)) {
            m_cursor = TextBox(m_text).hitTest(e.pos);
            m_canvas->update();
        }
    }

    void release(const ToolEvent &) override { m_selecting = false; }

    void doubleClick(const ToolEvent &e) override
    {
        if (!m_editing)
            return;
        // Select the word under the pointer.
        const QString &t = m_text.text;
        int a = TextBox(m_text).hitTest(e.pos), b = a;
        auto isWord = [&](int i) { return i >= 0 && i < t.size() && t[i].isLetterOrNumber(); };
        while (isWord(a - 1))
            --a;
        while (isWord(b))
            ++b;
        m_anchor = a;
        m_cursor = b;
        m_canvas->update();
    }

    void cancel() override { finish(); }

    bool wantsKey(QKeyEvent *e) const override
    {
        if (!m_editing)
            return false;
        const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (mods == Qt::ControlModifier) {
            switch (e->key()) {
            case Qt::Key_A: case Qt::Key_C: case Qt::Key_X: case Qt::Key_V: case Qt::Key_Left: case Qt::Key_Right:
            case Qt::Key_Home: case Qt::Key_End: case Qt::Key_Backspace: case Qt::Key_Delete: case Qt::Key_Return:
            case Qt::Key_Enter:
                return true;
            default:
                return false;  // other shortcuts (Ctrl+S, Ctrl+Z...) still work and finish the edit first
            }
        }
        // Plain keys, and AltGr combinations that type a character.
        return mods == Qt::NoModifier || !e->text().isEmpty();
    }

    bool keyPress(QKeyEvent *e) override
    {
        if (!m_editing || !valid())
            return false;
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        const bool ctrl = e->modifiers() & Qt::ControlModifier;
        const TextBox box(m_text);
        const int length = int(m_text.text.size());
        auto moveTo = [&](int pos) {
            m_cursor = std::clamp(pos, 0, length);
            if (!shift)
                m_anchor = m_cursor;
            m_canvas->update();
            return true;
        };
        switch (e->key()) {
        case Qt::Key_Escape:
            finish();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (ctrl) {
                finish();
                return true;
            }
            insert(QStringLiteral("\n"));
            return true;
        case Qt::Key_Backspace:
            if (hasSelection())
                insert(QString());
            else if (m_cursor > 0) {
                m_anchor = ctrl ? wordBoundary(m_cursor, -1) : m_cursor - 1;
                insert(QString());
            }
            return true;
        case Qt::Key_Delete:
            if (hasSelection())
                insert(QString());
            else if (m_cursor < length) {
                m_anchor = ctrl ? wordBoundary(m_cursor, 1) : m_cursor + 1;
                insert(QString());
            }
            return true;
        case Qt::Key_Left:
            if (!shift && hasSelection())
                return moveTo(std::min(m_cursor, m_anchor));
            return moveTo(ctrl ? wordBoundary(m_cursor, -1) : m_cursor - 1);
        case Qt::Key_Right:
            if (!shift && hasSelection())
                return moveTo(std::max(m_cursor, m_anchor));
            return moveTo(ctrl ? wordBoundary(m_cursor, 1) : m_cursor + 1);
        case Qt::Key_Up: return moveTo(box.moveVertically(m_cursor, -1));
        case Qt::Key_Down: return moveTo(box.moveVertically(m_cursor, 1));
        case Qt::Key_Home: return moveTo(ctrl ? 0 : box.lineStart(m_cursor));
        case Qt::Key_End: return moveTo(ctrl ? length : box.lineEnd(m_cursor));
        default:
            break;
        }
        if (ctrl && !(e->modifiers() & Qt::AltModifier)) {
            switch (e->key()) {
            case Qt::Key_A:
                m_anchor = 0;
                m_cursor = length;
                m_canvas->update();
                return true;
            case Qt::Key_C:
            case Qt::Key_X:
                if (hasSelection()) {
                    QGuiApplication::clipboard()->setText(selectedText());
                    if (e->key() == Qt::Key_X)
                        insert(QString());
                }
                return true;
            case Qt::Key_V:
                insert(QGuiApplication::clipboard()->text().remove(QLatin1Char('\r')));
                return true;
            default:
                return false;
            }
        }
        QString typed = e->text();
        typed.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
        if (typed.isEmpty())
            return false;
        insert(typed);
        return true;
    }

    void inputText(const QString &text) override
    {
        if (m_editing && valid() && !text.isEmpty())
            insert(text);
    }

    QRectF caretRect() const override
    {
        if (!m_editing)
            return {};
        const QLineF l = TextBox(m_text).cursorLine(m_cursor);
        return QRectF(l.p1(), l.p2()).normalized().adjusted(-1, 0, 1, 0);
    }

    void settingsChanged() override
    {
        // The options bar's font, color and anti-aliasing apply to the text being edited.
        if (!m_editing || !valid())
            return;
        m_text.font = m_settings->font;
        m_text.color = m_settings->foreground();
        m_text.antialias = m_settings->antialias;
        m_doc->setTextLive(m_layer, m_text);
        m_canvas->update();
    }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (!m_editing || !valid())
            return;
        const TextBox box(m_text);
        // Frame around the text being edited
        QPen frame(QColor(255, 255, 255, 160), 1, Qt::DashLine);
        frame.setCosmetic(true);
        p.setPen(frame);
        p.setBrush(Qt::NoBrush);
        p.drawRect(t.mapRect(box.bounds().adjusted(-3, -3, 3, 3)));
        // Selection
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(42, 130, 218, 110));
        for (const QRectF &r : box.selectionRects(m_anchor, m_cursor))
            p.drawRect(t.mapRect(r));
        // Cursor: dark outline with a light core, visible on any background
        const QLineF caret = t.map(box.cursorLine(m_cursor));
        p.setPen(QPen(QColor(0, 0, 0, 200), 3));
        p.drawLine(caret);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(caret);
    }

private:
    bool hasSelection() const { return m_anchor != m_cursor; }

    QString selectedText() const
    {
        const int a = std::min(m_anchor, m_cursor), b = std::max(m_anchor, m_cursor);
        return m_text.text.mid(a, b - a);
    }

    int wordBoundary(int from, int dir) const
    {
        const QString &t = m_text.text;
        int i = from;
        auto at = [&](int k) { return t[k].isLetterOrNumber(); };
        if (dir < 0) {
            while (i > 0 && !at(i - 1)) --i;
            while (i > 0 && at(i - 1)) --i;
        } else {
            while (i < t.size() && !at(i)) ++i;
            while (i < t.size() && at(i)) ++i;
        }
        return i;
    }

    // Replaces the selection (or inserts at the cursor) and re-renders the layer.
    void insert(const QString &s)
    {
        const int a = std::min(m_anchor, m_cursor), b = std::max(m_anchor, m_cursor);
        m_text.text.replace(a, b - a, s);
        m_cursor = m_anchor = a + int(s.size());
        m_doc->setTextLive(m_layer, m_text);
        m_canvas->update();
    }

    // The layer may have been removed or moved by something else (e.g. the Layers panel).
    bool valid() const
    {
        if (m_layer >= 0 && m_layer < m_doc->layerCount() && m_doc->layer(m_layer).text.pos == m_text.pos
            && m_doc->layer(m_layer).kind == LayerKind::Normal)
            return true;
        const_cast<TextTool *>(this)->m_editing = false;
        return false;
    }

    void finish()
    {
        if (!m_editing)
            return;
        m_editing = false;
        m_selecting = false;
        if (valid_noReset())
            m_doc->finishTextEdit(m_layer, m_before, m_isNew);
        m_before = DocState();
        if (m_canvas)
            m_canvas->update();
    }

    bool valid_noReset() const
    {
        return m_layer >= 0 && m_layer < m_doc->layerCount() && m_doc->layer(m_layer).text.pos == m_text.pos;
    }

    bool m_editing = false;
    bool m_isNew = false;
    bool m_selecting = false;
    int m_layer = -1;
    int m_cursor = 0;
    int m_anchor = 0;
    TextData m_text;
    DocState m_before;
};

class HandTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Hand; }
    QCursor cursor() const override { return Qt::OpenHandCursor; }
    void press(const ToolEvent &e) override { m_last = e.widgetPos; }
    void move(const ToolEvent &e) override
    {
        if (!(e.buttons & Qt::LeftButton))
            return;
        m_canvas->panBy(e.widgetPos - m_last);
        m_last = e.widgetPos;
    }

private:
    QPointF m_last;
};

class ZoomTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Zoom; }
    void press(const ToolEvent &e) override
    {
        const bool out = e.button == Qt::RightButton || (e.modifiers & Qt::AltModifier);
        m_canvas->zoomStep(out ? -1 : 1, e.widgetPos);
    }
};

std::unique_ptr<Tool> createTool(Tool::Id id, ToolSettings *s)
{
    switch (id) {
    case Tool::Move: return std::make_unique<MoveTool>(s);
    case Tool::Transform: return std::make_unique<TransformTool>(s);
    case Tool::RectSelect:
    case Tool::EllipseSelect:
    case Tool::Lasso: return std::make_unique<SelectTool>(s, id);
    case Tool::MagicWand: return std::make_unique<MagicWandTool>(s);
    case Tool::Crop: return std::make_unique<CropTool>(s);
    case Tool::Eyedropper: return std::make_unique<EyedropperTool>(s);
    case Tool::Brush: return std::make_unique<BrushTool>(s, BrushTool::Paint);
    case Tool::Eraser: return std::make_unique<BrushTool>(s, BrushTool::Erase);
    case Tool::CloneStamp: return std::make_unique<BrushTool>(s, BrushTool::Clone);
    case Tool::Healing: return std::make_unique<BrushTool>(s, BrushTool::Heal);
    case Tool::SpotHealing: return std::make_unique<BrushTool>(s, BrushTool::SpotHeal);
    case Tool::Blur: return std::make_unique<BrushTool>(s, BrushTool::BlurMode);
    case Tool::Sharpen: return std::make_unique<BrushTool>(s, BrushTool::SharpenMode);
    case Tool::Sponge: return std::make_unique<BrushTool>(s, BrushTool::SpongeMode);
    case Tool::PolyLasso: return std::make_unique<PolygonLassoTool>(s);
    case Tool::Smudge: return std::make_unique<BrushTool>(s, BrushTool::SmudgeMode);
    case Tool::Dodge: return std::make_unique<BrushTool>(s, BrushTool::DodgeMode);
    case Tool::Burn: return std::make_unique<BrushTool>(s, BrushTool::BurnMode);
    case Tool::Fill: return std::make_unique<FillTool>(s);
    case Tool::Gradient: return std::make_unique<GradientTool>(s);
    case Tool::LineShape:
    case Tool::RectShape:
    case Tool::EllipseShape: return std::make_unique<ShapeTool>(s, id);
    case Tool::Text: return std::make_unique<TextTool>(s);
    case Tool::Hand: return std::make_unique<HandTool>(s);
    case Tool::Zoom:
    case Tool::Count: break;
    }
    return std::make_unique<ZoomTool>(s);
}

} // namespace

QString Tool::name(Id id)
{
    switch (id) {
    case Move: return QObject::tr("Move");
    case Transform: return QObject::tr("Free Transform");
    case RectSelect: return QObject::tr("Rectangular Marquee");
    case EllipseSelect: return QObject::tr("Elliptical Marquee");
    case Lasso: return QObject::tr("Lasso");
    case PolyLasso: return QObject::tr("Polygonal Lasso");
    case MagicWand: return QObject::tr("Magic Wand");
    case Crop: return QObject::tr("Crop");
    case Eyedropper: return QObject::tr("Eyedropper");
    case Brush: return QObject::tr("Brush");
    case Eraser: return QObject::tr("Eraser");
    case CloneStamp: return QObject::tr("Clone Stamp");
    case SpotHealing: return QObject::tr("Spot Healing Brush");
    case Healing: return QObject::tr("Healing Brush");
    case Smudge: return QObject::tr("Smudge");
    case Blur: return QObject::tr("Blur");
    case Sharpen: return QObject::tr("Sharpen");
    case Sponge: return QObject::tr("Sponge");
    case Dodge: return QObject::tr("Dodge");
    case Burn: return QObject::tr("Burn");
    case Fill: return QObject::tr("Paint Bucket");
    case Gradient: return QObject::tr("Gradient");
    case LineShape: return QObject::tr("Line");
    case RectShape: return QObject::tr("Rectangle");
    case EllipseShape: return QObject::tr("Ellipse");
    case Text: return QObject::tr("Text");
    case Hand: return QObject::tr("Hand");
    case Zoom: return QObject::tr("Zoom");
    case Count: break;
    }
    return {};
}

QString Tool::shortcut(Id id)
{
    static const char *keys[Count] = {"V", "Ctrl+T", "M", "Shift+M", "L", "Shift+L", "W", "C", "I", "B", "E", "S",
                                      "J", "Shift+J", "R", "Shift+R", "", "O", "Shift+O", "", "K", "G", "N", "U",
                                      "Shift+U", "T", "H", "Z"};
    return QString::fromLatin1(keys[id]);
}

QString Tool::hint(Id id)
{
    switch (id) {
    case Move: return QObject::tr("Drag to move the layer or the selected pixels. Arrow keys nudge (Shift = 10 px).");
    case Transform: return QObject::tr("Drag handles to scale (Shift keeps proportions), outside to rotate, inside to move. Enter applies, Esc cancels.");
    case RectSelect:
    case EllipseSelect:
    case Lasso:
    case MagicWand: return QObject::tr("Shift: add · Alt/Ctrl: subtract · Shift+Alt: intersect · click to deselect");
    case Crop: return QObject::tr("Drag a frame · drag inside to move, outside to straighten · Enter crops, Esc cancels");
    case PolyLasso: return QObject::tr("Click corner points · double-click or click the first point to close · Backspace undoes a point");
    case SpotHealing: return QObject::tr("Paint over spots and blemishes; a matching patch nearby replaces them");
    case Blur: return QObject::tr("Paint to soften details");
    case Sharpen: return QObject::tr("Paint to sharpen details");
    case Sponge: return QObject::tr("Paint to remove (or add) color saturation");
    case Eyedropper: return QObject::tr("Click: foreground color · Right-click or Alt-click: background color");
    case Brush:
    case Eraser: return QObject::tr("Shift-click draws a straight line from the last stroke · [ ] change size");
    case CloneStamp: return QObject::tr("Alt-click or Ctrl-click sets the source, then paint to copy it");
    case Healing: return QObject::tr("Alt-click or Ctrl-click sets the source, then paint over blemishes");
    case Smudge: return QObject::tr("Drag to smear colors, like a finger through wet paint");
    case Dodge: return QObject::tr("Paint to lighten the chosen tonal range");
    case Burn: return QObject::tr("Paint to darken the chosen tonal range");
    case Fill: return QObject::tr("Fills similar colored area with the foreground color");
    case Gradient: return QObject::tr("Drag to draw a foreground → background gradient. Shift snaps to 45°.");
    case LineShape:
    case RectShape:
    case EllipseShape: return QObject::tr("Drag to draw. Shift constrains proportions / angle.");
    case Text: return QObject::tr("Click to type on the image · click text to edit it · Esc or click elsewhere to finish");
    case Hand: return QObject::tr("Drag to pan. Hold Space with any tool to pan temporarily.");
    case Zoom: return QObject::tr("Click to zoom in · Right-click or Alt-click to zoom out · Ctrl+wheel zooms");
    case Count: break;
    }
    return {};
}

bool Tool::editsPixels(Id id)
{
    switch (id) {
    case Transform: case Brush: case Eraser: case CloneStamp: case SpotHealing: case Healing: case Smudge:
    case Blur: case Sharpen: case Dodge: case Burn: case Sponge:
    case Fill: case Gradient: case LineShape: case RectShape: case EllipseShape:
        return true;
    default:
        return false;
    }
}

ToolManager::ToolManager(ToolSettings *settings, QObject *parent) : QObject(parent), m_settings(settings)
{
    for (int i = 0; i < Tool::Count; ++i)
        m_tools[i] = createTool(Tool::Id(i), settings);
}

ToolManager::~ToolManager() = default;

void ToolManager::setCurrent(Tool::Id id)
{
    if (id == m_current)
        return;
    current()->cancel();
    m_current = id;
    emit toolChanged(id);
}
