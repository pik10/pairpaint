// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Tools.h"

#include "Canvas.h"
#include "Dialogs.h"
#include "Document.h"
#include "Filters.h"
#include "ToolSettings.h"

#include <QCursor>
#include <QKeyEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QtMath>
#include <functional>

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
    enum Mode { Paint, Erase, Clone, Heal };
    BrushTool(ToolSettings *s, Mode mode) : Tool(s), m_mode(mode) {}

    Id id() const override
    {
        switch (m_mode) {
        case Erase: return Eraser;
        case Clone: return CloneStamp;
        case Heal: return Healing;
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
        m_buffer = QImage(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
        m_buffer.fill(Qt::transparent);
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
        m_last = e.pos;
        m_lastPressure = e.pressure;
    }

    void move(const ToolEvent &e) override
    {
        m_hover = e.pos;
        if (!m_active)
            return;
        stroke(m_last, e.pos, false, m_lastPressure, e.pressure);
        m_last = e.pos;
        m_lastPressure = e.pressure;
    }

    void release(const ToolEvent &e) override
    {
        if (e.button == Qt::LeftButton)
            finish();
    }

    void cancel() override { finish(); }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
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
    void finish()
    {
        if (!m_active)
            return;
        m_active = false;
        if (m_mode == Heal)
            applyHealing();
        m_lastEnd = m_last;
        m_lastDoc = m_doc;
        m_doc->commit(name(id()), m_before);
        m_doc->notifyStructureChanged();
        m_before = DocState();
        m_base = m_buffer = m_source = QImage();
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

        QColor color = m_mode == Erase ? QColor(Qt::black) : m_settings->foreground();
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

    void composite(QRect r)
    {
        r &= m_doc->rect();
        if (r.isEmpty())
            return;
        m_strokeRect |= r;
        const QImage stroke = selectionMasked(m_buffer.copy(r), m_doc, r);
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
    QRect m_strokeRect;
    QPointF m_last, m_lastEnd, m_hover;
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
        const QRectF r = QRectF(m_start, m_end).normalized();
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
        const QImage region = Filters::floodMask(sample, pt, m_settings->tolerance, m_settings->contiguous);

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
        if (e.modifiers & Qt::ShiftModifier) {
            if (std::abs(d.x()) > std::abs(d.y())) d.setY(0); else d.setX(0);
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
        // Moving part of a text layer turns it into pixels; moving all of it keeps it editable.
        if (m_doc->hasSelection())
            m_doc->prepareForPixelEdit();
        m_before = m_doc->state();
        m_moved = false;
        m_delta = QPoint();
        m_pixels.lift(m_doc);
    }

    void apply(const QPoint &d)
    {
        m_delta = d;
        m_moved = true;
        m_pixels.place(m_doc, QTransform::fromTranslate(d.x(), d.y()));
    }

    void end()
    {
        if (m_moved) {
            if (!m_doc->editingMask() && m_doc->activeLayer().isText())
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
        if (m_active || !m_doc)
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

class CropTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Crop; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        if (m_rect.contains(e.pos))
            return;  // clicking inside keeps the rectangle (double-click applies it)
        m_drag = true;
        m_start = e.pos;
        m_rect = QRectF();
    }
    void move(const ToolEvent &e) override
    {
        if (!m_drag)
            return;
        m_rect = QRectF(m_start, e.pos).normalized() & QRectF(m_doc->rect());
        m_canvas->update();
    }
    void release(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        m_drag = false;
        if (m_rect.width() < 1 || m_rect.height() < 1)
            m_rect = QRectF();
        m_canvas->update();
    }
    void doubleClick(const ToolEvent &) override { apply(); }
    bool keyPress(QKeyEvent *e) override
    {
        if (m_rect.isEmpty())
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
        m_drag = false;
        m_rect = QRectF();
    }

    void paintOverlay(QPainter &p, const QTransform &t) override
    {
        if (m_rect.isEmpty())
            return;
        const QRectF wr = t.mapRect(m_rect);
        QPainterPath outside;
        outside.addRect(t.mapRect(QRectF(m_doc->rect())));
        outside.addRect(wr);
        p.fillPath(outside, QColor(0, 0, 0, 150));
        p.setPen(QPen(QColor(255, 255, 255, 90), 1));
        for (int i = 1; i < 3; ++i) {  // rule of thirds
            const qreal x = wr.left() + wr.width() * i / 3, y = wr.top() + wr.height() * i / 3;
            p.drawLine(QPointF(x, wr.top()), QPointF(x, wr.bottom()));
            p.drawLine(QPointF(wr.left(), y), QPointF(wr.right(), y));
        }
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(wr);
        if (!m_drag) {
            const QString text = QObject::tr("%1 × %2 — Enter to crop, Esc to cancel")
                                     .arg(qRound(m_rect.width())).arg(qRound(m_rect.height()));
            p.setPen(Qt::white);
            p.drawText(wr.bottomLeft() + QPointF(0, 16), text);
        }
    }

private:
    void apply()
    {
        if (m_rect.isEmpty())
            return;
        const QRect r(QPoint(qFloor(m_rect.left()), qFloor(m_rect.top())),
                      QPoint(qCeil(m_rect.right()) - 1, qCeil(m_rect.bottom()) - 1));
        m_rect = QRectF();
        m_doc->crop(r);
        m_canvas->update();
    }

    bool m_drag = false;
    QPointF m_start;
    QRectF m_rect;
};

// ---------------------------------------------------------------------------

class TextTool : public Tool {
public:
    using Tool::Tool;
    Id id() const override { return Text; }
    QCursor cursor() const override { return Qt::IBeamCursor; }

    void press(const ToolEvent &e) override
    {
        if (e.button != Qt::LeftButton)
            return;
        // Clicking on the active text layer edits it; anywhere else creates a new one.
        const Layer &l = m_doc->activeLayer();
        if (l.isText() && textBounds(l.text).contains(e.pos)) {
            TextDialog::editLayer(m_doc, m_doc->activeIndex(), m_canvas);
            return;
        }
        TextData t;
        t.font = m_settings->font;
        t.color = m_settings->foreground();
        t.antialias = m_settings->antialias;
        t.pos = e.pos;
        TextDialog dlg(t, m_canvas);
        if (dlg.exec() == QDialog::Accepted && dlg.data().isValid())
            m_doc->addTextLayer(dlg.data());
    }
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
    case MagicWand: return QObject::tr("Magic Wand");
    case Crop: return QObject::tr("Crop");
    case Eyedropper: return QObject::tr("Eyedropper");
    case Brush: return QObject::tr("Brush");
    case Eraser: return QObject::tr("Eraser");
    case CloneStamp: return QObject::tr("Clone Stamp");
    case Healing: return QObject::tr("Healing Brush");
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
    static const char *keys[Count] = {"V", "Ctrl+T", "M", "Shift+M", "L", "W", "C", "I", "B", "E", "S", "J",
                                      "K", "G", "N", "U", "Shift+U", "T", "H", "Z"};
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
    case Crop: return QObject::tr("Drag a rectangle, then press Enter or double-click. Esc cancels.");
    case Eyedropper: return QObject::tr("Click: foreground color · Right-click or Alt-click: background color");
    case Brush:
    case Eraser: return QObject::tr("Shift-click draws a straight line from the last stroke · [ ] change size");
    case CloneStamp: return QObject::tr("Alt-click or Ctrl-click sets the source, then paint to copy it");
    case Healing: return QObject::tr("Alt-click or Ctrl-click sets the source, then paint over blemishes");
    case Fill: return QObject::tr("Fills similar colored area with the foreground color");
    case Gradient: return QObject::tr("Drag to draw a foreground → background gradient. Shift snaps to 45°.");
    case LineShape:
    case RectShape:
    case EllipseShape: return QObject::tr("Drag to draw. Shift constrains proportions / angle.");
    case Text: return QObject::tr("Click to add a text layer · click on the active text layer to edit it");
    case Hand: return QObject::tr("Drag to pan. Hold Space with any tool to pan temporarily.");
    case Zoom: return QObject::tr("Click to zoom in · Right-click or Alt-click to zoom out · Ctrl+wheel zooms");
    case Count: break;
    }
    return {};
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
