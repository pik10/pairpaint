// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Document.h"

#include "Filters.h"
#include "TextBox.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QTransform>
#include <QtMath>
#include <QUndoCommand>
#include <algorithm>

namespace {

class StateCommand : public QUndoCommand {
public:
    StateCommand(Document *doc, const QString &text, const DocState &before, const DocState &after, int mergeId)
        : QUndoCommand(text), m_doc(doc), m_before(before), m_after(after), m_mergeId(mergeId) {}

    void undo() override { m_doc->setState(m_before); }
    void redo() override
    {
        // The document is already in the "after" state when the command is pushed.
        if (m_firstRedo) { m_firstRedo = false; return; }
        m_doc->setState(m_after);
    }
    int id() const override { return m_mergeId; }
    bool mergeWith(const QUndoCommand *other) override
    {
        if (m_mergeId < 0 || other->id() != m_mergeId)
            return false;
        m_after = static_cast<const StateCommand *>(other)->m_after;
        return true;
    }

private:
    Document *m_doc;
    DocState m_before;
    DocState m_after;
    int m_mergeId;
    bool m_firstRedo = true;
};

QImage blankLayer(const QSize &size, const QColor &color = Qt::transparent)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

QImage emptyMask(const QSize &size)
{
    QImage m(size, QImage::Format_Alpha8);
    m.fill(0);
    return m;
}

QImage flipImage(const QImage &img, Qt::Orientation o)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return img.flipped(o);
#else
    return img.mirrored(o == Qt::Horizontal, o == Qt::Vertical);
#endif
}

// Coverage of a layer-mask pixel. Masks are premultiplied, so transparent counts as black.
inline int maskValue(QRgb p) { return (qRed(p) * 77 + qGreen(p) * 150 + qBlue(p) * 29) >> 8; }

inline QRgb scalePixel(QRgb p, int k)
{
    return qRgba(qRed(p) * k / 255, qGreen(p) * k / 255, qBlue(p) * k / 255, qAlpha(p) * k / 255);
}

// `part` holds the document region `r`; multiplies it by the mask coverage.
void multiplyByMask(QImage &part, const QImage &mask, const QRect &r)
{
    for (int y = 0; y < part.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(part.scanLine(y));
        const QRgb *m = reinterpret_cast<const QRgb *>(mask.constScanLine(r.top() + y)) + r.left();
        for (int x = 0; x < part.width(); ++x) {
            const int k = maskValue(m[x]);
            if (k != 255)
                row[x] = scalePixel(row[x], k);
        }
    }
}

// out (document region r) = lerp(out, adjusted(out), opacity * mask)
void blendAdjustment(QImage &out, const Layer &l, const QRect &r)
{
    const QImage adj = Adjustments::apply(out, l.adjustment.type, l.adjustment.params)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    Blend::mix(out, adj, l.opacity * l.fillOpacity, l.maskEnabled ? l.mask : QImage(), r);
}

QImage tinted(const QImage &alphaSource, const QColor &color)
{
    QImage t(alphaSource.size(), QImage::Format_ARGB32_Premultiplied);
    t.fill(color);
    QPainter p(&t);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, alphaSource);
    return t;
}

// The layer's pixels for region `r` with its mask applied and its effects drawn behind.
QImage renderWithEffects(const Layer &l, const QRect &r)
{
    const LayerStyle &s = l.style;
    const int m = s.margin();
    const QRect er = r.adjusted(-m, -m, m, m) & l.image.rect();
    QImage content = l.image.copy(er);
    if (!l.mask.isNull() && l.maskEnabled)
        multiplyByMask(content, l.mask, er);
    const QImage alpha = content.convertToFormat(QImage::Format_Alpha8);

    QImage out(r.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.translate(er.topLeft() - r.topLeft());
    if (s.shadow) {
        const qreal a = qDegreesToRadians(qreal(s.shadowAngle));
        const QPointF offset(-std::cos(a) * s.shadowDistance, std::sin(a) * s.shadowDistance);
        p.setOpacity(s.shadowOpacity / 100.0);
        p.drawImage(offset, Filters::gaussianBlur(tinted(alpha, s.shadowColor), s.shadowSize / 2.0));
    }
    if (s.glow) {
        const QImage spread = Filters::morphMask(alpha, s.glowSize * 0.3);
        p.setOpacity(s.glowOpacity / 100.0);
        p.drawImage(0, 0, Filters::gaussianBlur(tinted(spread, s.glowColor), s.glowSize / 2.0));
    }
    if (s.stroke) {
        p.setOpacity(s.strokeOpacity / 100.0);
        p.drawImage(0, 0, tinted(Filters::morphMask(alpha, s.strokeSize), s.strokeColor));
    }
    p.setOpacity(l.fillOpacity);  // Fill fades the pixels, not the effects
    p.drawImage(0, 0, content);
    return out;
}

// A layer's own pixels for region `r` (mask, effects and Fill applied), before its
// opacity and blend mode.
QImage layerContent(const Layer &l, const QRect &r)
{
    if (l.style.any())
        return renderWithEffects(l, r);
    QImage part = l.image.copy(r);
    if (!l.mask.isNull() && l.maskEnabled)
        multiplyByMask(part, l.mask, r);
    if (l.fillOpacity < 1.0) {
        QImage faded(part.size(), QImage::Format_ARGB32_Premultiplied);
        faded.fill(Qt::transparent);
        QPainter p(&faded);
        p.setOpacity(l.fillOpacity);
        p.drawImage(0, 0, part);
        p.end();
        part = faded;
    }
    return part;
}

// Composites one layer onto `out`, which holds the document region `r`.
void compositeLayer(QImage &out, const Layer &l, const QRect &r)
{
    if (!l.visible || l.opacity <= 0.0 || l.kind != LayerKind::Normal)
        return;
    if (l.isAdjustment()) {
        blendAdjustment(out, l, r);
        return;
    }
    const bool plain = !l.style.any() && (l.mask.isNull() || !l.maskEnabled) && l.fillOpacity >= 1.0;
    if (plain && !Blend::isCustom(l.mode)) {
        QPainter p(&out);  // fast path: no copy needed
        p.setOpacity(l.opacity);
        p.setCompositionMode(l.mode);
        p.drawImage(QPoint(0, 0), l.image, r);
        return;
    }
    Blend::draw(out, layerContent(l, r), l.mode, l.opacity, r.topLeft());
}

// Draws clipped layer `c` onto `base` (its clipping base's pixels for region r): blended
// with c's own mode and opacity, but only where the base has pixels.
void clipOnto(QImage &base, const Layer &c, const QRect &r)
{
    if (!c.visible || c.opacity <= 0.0)
        return;
    if (c.isAdjustment()) {
        blendAdjustment(base, c, r);  // adjustments keep the base's transparency
        return;
    }
    QImage blended = base.copy();
    Blend::draw(blended, layerContent(c, r), c.mode, c.opacity, r.topLeft());
    Blend::restoreAlpha(blended, base);
    base = blended;
}

// For every GroupEnd marker the index of its header, and vice versa (-1 if unmatched).
QList<int> matchGroups(const QList<Layer> &layers)
{
    QList<int> match(layers.size(), -1);
    QList<int> open;
    for (int i = 0; i < layers.size(); ++i) {
        if (layers[i].isGroupEnd()) {
            open.append(i);
        } else if (layers[i].isGroup() && !open.isEmpty()) {
            const int e = open.takeLast();
            match[e] = i;
            match[i] = e;
        }
    }
    return match;
}

// Composites layers [begin, end) onto `out`. A group's layers are composited on their
// own first, then blended in with the group's opacity, mode and mask.
void compositeRange(QImage &out, const QList<Layer> &layers, const QList<int> &match, int begin, int end,
                    const QRect &r)
{
    for (int i = begin; i < end; ++i) {
        const Layer &l = layers[i];
        if (l.isGroupEnd()) {
            const int h = match[i];
            if (h < 0)
                continue;
            const Layer &g = layers[h];
            const bool masked = !g.mask.isNull() && g.maskEnabled;
            int clipEnd = h + 1;  // layers clipped to the whole group
            while (clipEnd < end && layers[clipEnd].clipped && layers[clipEnd].kind == LayerKind::Normal)
                ++clipEnd;
            if (!g.visible || g.opacity <= 0.0) {
                // hidden group (and anything clipped to it)
            } else if (clipEnd > h + 1 && g.mode == Blend::PassThrough) {
                // Clipped to a pass-through group: the group blends into what is below it, and
                // the clipped layers blend with that result, but only within the group's shape.
                QImage shape(r.size(), QImage::Format_ARGB32_Premultiplied);
                shape.fill(Qt::transparent);
                compositeRange(shape, layers, match, i + 1, h, r);
                if (masked)
                    multiplyByMask(shape, g.mask, r);
                QImage result = out.copy();
                compositeRange(result, layers, match, i + 1, h, r);
                if (g.opacity < 1.0 || masked) {
                    QImage blended = out.copy();
                    Blend::mix(blended, result, g.opacity, masked ? g.mask : QImage(), r);
                    result = blended;
                }
                for (int c = h + 1; c < clipEnd; ++c) {
                    const Layer &cl = layers[c];
                    if (!cl.visible || cl.opacity <= 0.0 || cl.isAdjustment())
                        continue;
                    QImage withClip = result.copy();
                    Blend::draw(withClip, layerContent(cl, r), cl.mode, cl.opacity, r.topLeft());
                    Blend::mixByAlpha(result, withClip, shape);
                }
                out = result;
            } else if (clipEnd > h + 1) {
                // A group as a clipping base is rendered on its own first.
                QImage base(r.size(), QImage::Format_ARGB32_Premultiplied);
                base.fill(Qt::transparent);
                compositeRange(base, layers, match, i + 1, h, r);
                if (masked)
                    multiplyByMask(base, g.mask, r);
                for (int c = h + 1; c < clipEnd; ++c)
                    clipOnto(base, layers[c], r);
                Blend::draw(out, base, g.mode == Blend::PassThrough ? QPainter::CompositionMode_SourceOver : g.mode,
                            g.opacity, r.topLeft());
            } else if (g.mode == Blend::PassThrough) {
                // The group's layers blend straight into what is below the group.
                if (g.opacity >= 1.0 && !masked) {
                    compositeRange(out, layers, match, i + 1, h, r);
                } else {
                    QImage result = out.copy();
                    compositeRange(result, layers, match, i + 1, h, r);
                    Blend::mix(out, result, g.opacity, masked ? g.mask : QImage(), r);
                }
            } else {
                QImage group(r.size(), QImage::Format_ARGB32_Premultiplied);
                group.fill(Qt::transparent);
                compositeRange(group, layers, match, i + 1, h, r);
                if (masked)
                    multiplyByMask(group, g.mask, r);
                Blend::draw(out, group, g.mode, g.opacity, r.topLeft());
            }
            i = std::max(h, clipEnd - 1);
            continue;
        }
        // A layer followed by clipped layers forms a clipping group: they are drawn onto the
        // base layer's pixels, then the result takes the base's opacity and blend mode.
        int clipEnd = i + 1;
        while (clipEnd < end && layers[clipEnd].clipped && layers[clipEnd].kind == LayerKind::Normal)
            ++clipEnd;
        if (clipEnd > i + 1 && l.kind == LayerKind::Normal && !l.isAdjustment() && !l.clipped) {
            if (l.visible && l.opacity > 0.0) {
                QImage base = layerContent(l, r);
                for (int c = i + 1; c < clipEnd; ++c)
                    clipOnto(base, layers[c], r);
                Blend::draw(out, base, l.mode, l.opacity, r.topLeft());
            }
            i = clipEnd - 1;
            continue;
        }
        compositeLayer(out, l, r);
    }
}

void bakeMask(Layer &l)
{
    if (l.mask.isNull())
        return;
    if (l.maskEnabled)
        multiplyByMask(l.image, l.mask, l.image.rect());
    l.mask = QImage();
}

QString textLayerName(const TextData &t) { return t.text.section('\n', 0, 0).left(30); }

} // namespace

QList<QPair<QString, QPainter::CompositionMode>> blendModes()
{
    // Same order and grouping as Photoshop.
    return {
        {QObject::tr("Pass Through"), Blend::PassThrough},  // groups only
        {QObject::tr("Normal"), QPainter::CompositionMode_SourceOver},
        {QObject::tr("Dissolve"), Blend::Dissolve},
        {QObject::tr("Darken"), QPainter::CompositionMode_Darken},
        {QObject::tr("Multiply"), QPainter::CompositionMode_Multiply},
        {QObject::tr("Color Burn"), QPainter::CompositionMode_ColorBurn},
        {QObject::tr("Linear Burn"), Blend::LinearBurn},
        {QObject::tr("Darker Color"), Blend::DarkerColor},
        {QObject::tr("Lighten"), QPainter::CompositionMode_Lighten},
        {QObject::tr("Screen"), QPainter::CompositionMode_Screen},
        {QObject::tr("Color Dodge"), QPainter::CompositionMode_ColorDodge},
        {QObject::tr("Linear Dodge (Add)"), QPainter::CompositionMode_Plus},
        {QObject::tr("Lighter Color"), Blend::LighterColor},
        {QObject::tr("Overlay"), QPainter::CompositionMode_Overlay},
        {QObject::tr("Soft Light"), QPainter::CompositionMode_SoftLight},
        {QObject::tr("Hard Light"), QPainter::CompositionMode_HardLight},
        {QObject::tr("Vivid Light"), Blend::VividLight},
        {QObject::tr("Linear Light"), Blend::LinearLight},
        {QObject::tr("Pin Light"), Blend::PinLight},
        {QObject::tr("Hard Mix"), Blend::HardMix},
        {QObject::tr("Difference"), QPainter::CompositionMode_Difference},
        {QObject::tr("Exclusion"), QPainter::CompositionMode_Exclusion},
        {QObject::tr("Subtract"), Blend::Subtract},
        {QObject::tr("Divide"), Blend::Divide},
        {QObject::tr("Hue"), Blend::Hue},
        {QObject::tr("Saturation"), Blend::Saturation},
        {QObject::tr("Color"), Blend::Color},
        {QObject::tr("Luminosity"), Blend::Luminosity},
    };
}

int LayerStyle::margin() const
{
    int m = 0;
    if (shadow)
        m = std::max(m, shadowDistance + shadowSize * 2 + 2);
    if (glow)
        m = std::max(m, glowSize * 2 + 2);
    if (stroke)
        m = std::max(m, strokeSize + 2);
    return m;
}

bool LayerStyle::operator==(const LayerStyle &o) const
{
    return shadow == o.shadow && shadowColor == o.shadowColor && shadowOpacity == o.shadowOpacity
        && shadowAngle == o.shadowAngle && shadowDistance == o.shadowDistance && shadowSize == o.shadowSize
        && glow == o.glow && glowColor == o.glowColor && glowOpacity == o.glowOpacity && glowSize == o.glowSize
        && stroke == o.stroke && strokeColor == o.strokeColor && strokeOpacity == o.strokeOpacity
        && strokeSize == o.strokeSize;
}

QImage renderText(const TextData &t, const QSize &size)
{
    QImage img = blankLayer(size);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, t.antialias);
    p.setRenderHint(QPainter::TextAntialiasing, t.antialias);
    p.setPen(t.color);
    TextBox(t).draw(p);
    return img;
}

QRectF textBounds(const TextData &t) { return TextBox(t).bounds(); }

QRect alphaBounds(const QImage &img)
{
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x])) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = y;
            }
        }
    }
    return maxX < 0 ? QRect() : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

Document::Document(const QSize &size, const QColor &background, QObject *parent) : QObject(parent)
{
    m_state.size = size;
    Layer l;
    l.name = tr("Background");
    l.image = blankLayer(size, background);
    m_state.layers.append(l);
    init();
}

Document::Document(const QImage &image, QObject *parent) : QObject(parent)
{
    m_state.size = image.size();
    Layer l;
    l.name = tr("Background");
    l.image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_state.layers.append(l);
    init();
}

Document::Document(const DocState &state, QObject *parent) : QObject(parent), m_state(state)
{
    m_state.active = std::clamp(m_state.active, 0, layerCount() - 1);
    init();
}

Document::~Document()
{
    // Destroying the undo stack clears it, which emits cleanChanged; don't let that
    // reach (possibly half-destroyed) listeners through titleChanged.
    disconnect(&m_undo, nullptr, this, nullptr);
}

void Document::init()
{
    static int untitledCounter = 0;
    m_untitled = ++untitledCounter;
    m_layerCounter = layerCount() - 1;
    m_undo.setUndoLimit(80);
    connect(&m_undo, &QUndoStack::cleanChanged, this, &Document::titleChanged);
    updateSelectionBounds();
    syncEditTarget();
}

void Document::setState(const DocState &state)
{
    const bool resized = state.size != m_state.size;
    m_state = state;
    m_state.active = std::clamp(m_state.active, 0, layerCount() - 1);
    updateSelectionBounds();
    syncEditTarget();
    if (resized)
        emit sizeChanged();
    emit structureChanged();
    emit imageChanged(rect());
    emit selectionChanged();
}

void Document::commit(const QString &text, const DocState &before, int mergeId)
{
    m_undo.push(new StateCommand(this, text, before, m_state, mergeId));
}

void Document::finish(const QString &text, const DocState &before, int changes, int mergeId)
{
    updateSelectionBounds();
    syncEditTarget();
    commit(text, before, mergeId);
    if (before.size != m_state.size)
        emit sizeChanged();
    if (changes & Structure)
        emit structureChanged();
    if (changes & Pixels)
        emit imageChanged(rect());
    if ((changes & Selection) || before.selection.cacheKey() != m_state.selection.cacheKey())
        emit selectionChanged();
}

void Document::setActiveIndex(int index)
{
    index = std::clamp(index, 0, layerCount() - 1);
    if (index == m_state.active)
        return;
    m_state.active = index;
    m_editMask = false;
    syncEditTarget();
    emit structureChanged();
}

void Document::setFilePath(const QString &path)
{
    m_filePath = path;
    emit titleChanged();
}

QString Document::displayName() const
{
    if (m_filePath.isEmpty())
        return tr("Untitled-%1").arg(m_untitled);
    return QFileInfo(m_filePath).fileName();
}

// ---------------------------------------------------------------------------
// Edit target

void Document::syncEditTarget()
{
    const Layer &l = activeLayer();
    if (l.mask.isNull())
        m_editMask = false;
    else if (l.isAdjustment() || l.isGroup())
        m_editMask = true;  // adjustment layers and groups have no pixels of their own
}

bool Document::editingMask() const { return m_editMask && !activeLayer().mask.isNull(); }

void Document::setEditingMask(bool mask)
{
    if (mask && activeLayer().mask.isNull())
        return;
    if (!mask && (activeLayer().isAdjustment() || activeLayer().isGroup()))
        return;
    if (m_editMask == mask)
        return;
    m_editMask = mask;
    emit structureChanged();
}

QImage &Document::targetImage() { return editingMask() ? activeLayer().mask : activeLayer().image; }

const QImage &Document::targetImage() const { return editingMask() ? activeLayer().mask : activeLayer().image; }

bool Document::canEditPixels() const
{
    return editingMask() || activeLayer().kind == LayerKind::Normal;
}

void Document::prepareForPixelEdit()
{
    if (!editingMask() && activeLayer().isText())
        rasterizeLayer(m_state.active);
}

// ---------------------------------------------------------------------------
// Compositing

QImage Document::composite(const QRect &r) const
{
    QImage out(r.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    compositeRange(out, m_state.layers, matchGroups(m_state.layers), 0, layerCount(), r);
    return out;
}

QImage Document::maskedBlend(const QImage &original, const QImage &modified) const
{
    QImage mod = modified.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!hasSelection())
        return mod;

    // out = original * (1 - selection) + modified * selection
    QImage out = original.copy();
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(0, 0, m_state.selection);
    p.end();

    QImage inner = mod.copy();
    p.begin(&inner);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, m_state.selection);
    p.end();

    p.begin(&out);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.drawImage(0, 0, inner);
    return out;
}

// ---------------------------------------------------------------------------
// Selection

void Document::updateSelectionBounds()
{
    QImage &m = m_state.selection;
    if (m.isNull()) {
        m_selectionBounds = QRect();
        return;
    }
    int minX = m.width(), minY = m.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < m.height(); ++y) {
        const uchar *row = m.constScanLine(y);
        int first = -1, last = -1;
        for (int x = 0; x < m.width(); ++x) {
            if (row[x]) { first = x; break; }
        }
        if (first < 0)
            continue;
        for (int x = m.width() - 1; x >= first; --x) {
            if (row[x]) { last = x; break; }
        }
        minX = std::min(minX, first);
        maxX = std::max(maxX, last);
        minY = std::min(minY, y);
        maxY = y;
    }
    if (maxX < 0) {
        m = QImage();  // empty mask means no selection
        m_selectionBounds = QRect();
    } else {
        m_selectionBounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
    }
}

void Document::setSelection(const QImage &mask)
{
    m_state.selection = mask;
    updateSelectionBounds();
    emit selectionChanged();
}

void Document::selectMask(const QImage &shape, SelectionOp op, const QString &text)
{
    const DocState before = m_state;
    QImage result;
    if (op == SelectionOp::Replace || (op == SelectionOp::Add && !hasSelection())) {
        result = shape;
    } else if (!hasSelection()) {
        result = QImage();  // subtracting from / intersecting with nothing
    } else {
        result = m_state.selection.copy();
        QPainter p(&result);
        if (op == SelectionOp::Subtract)
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        else if (op == SelectionOp::Intersect)
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, shape);
    }
    m_state.selection = result;
    finish(text, before, Selection);
}

void Document::selectPath(const QPainterPath &path, SelectionOp op, bool antialias, const QString &text)
{
    QImage shape = emptyMask(size());
    QPainter p(&shape);
    p.setRenderHint(QPainter::Antialiasing, antialias);
    p.fillPath(path, QColor(0, 0, 0, 255));
    p.end();
    selectMask(shape, op, text);
}

void Document::selectAll()
{
    const DocState before = m_state;
    QImage m(size(), QImage::Format_Alpha8);
    m.fill(255);
    m_state.selection = m;
    finish(tr("Select All"), before, Selection);
}

void Document::deselect()
{
    if (!hasSelection())
        return;
    const DocState before = m_state;
    m_state.selection = QImage();
    finish(tr("Deselect"), before, Selection);
}

void Document::invertSelection()
{
    if (!hasSelection())
        return;
    const DocState before = m_state;
    QImage m = m_state.selection.copy();
    for (int y = 0; y < m.height(); ++y) {
        uchar *row = m.scanLine(y);
        for (int x = 0; x < m.width(); ++x)
            row[x] = 255 - row[x];
    }
    m_state.selection = m;
    finish(tr("Inverse Selection"), before, Selection);
}

void Document::modifySelection(const QString &text, const std::function<QImage(const QImage &)> &f)
{
    if (!hasSelection())
        return;
    const DocState before = m_state;
    m_state.selection = f(m_state.selection);
    finish(text, before, Selection);
}

void Document::featherSelection(double radius)
{
    modifySelection(tr("Feather"), [radius](const QImage &m) { return Filters::featherMask(m, radius); });
}

void Document::growSelection(double pixels)
{
    modifySelection(tr("Expand Selection"), [pixels](const QImage &m) { return Filters::morphMask(m, pixels); });
}

void Document::shrinkSelection(double pixels)
{
    modifySelection(tr("Contract Selection"), [pixels](const QImage &m) { return Filters::morphMask(m, -pixels); });
}

void Document::borderSelection(double width)
{
    modifySelection(tr("Border"), [width](const QImage &m) {
        QImage outer = Filters::morphMask(m, width / 2);
        const QImage inner = Filters::morphMask(m, -width / 2);
        QPainter p(&outer);
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.drawImage(0, 0, inner);
        return outer;
    });
}

void Document::smoothSelection(double radius)
{
    // Blurring and re-thresholding rounds off corners and removes specks.
    modifySelection(tr("Smooth Selection"), [radius](const QImage &m) {
        QImage out = Filters::featherMask(m, radius);
        for (int y = 0; y < out.height(); ++y) {
            uchar *row = out.scanLine(y);
            for (int x = 0; x < out.width(); ++x)
                row[x] = uchar(std::clamp((row[x] - 128) * 4 + 128, 0, 255));
        }
        return out;
    });
}

void Document::selectLayerTransparency()
{
    selectMask(renderLayer(m_state.active).convertToFormat(QImage::Format_Alpha8), SelectionOp::Replace,
               tr("Load Selection"));
}

// ---------------------------------------------------------------------------
// Layers

void Document::addLayer(const QString &name, const QImage &content, const QString &undoText)
{
    const DocState before = m_state;
    Layer l;
    l.name = name.isEmpty() ? tr("Layer %1").arg(++m_layerCounter) : name;
    l.image = content.isNull() ? blankLayer(size())
                               : content.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_state.active = insertionIndex();
    m_state.layers.insert(m_state.active, l);
    m_editMask = false;
    finish(undoText.isEmpty() ? tr("New Layer") : undoText, before);
}

void Document::addAdjustmentLayer(const Adjustment &adjustment, bool undoable)
{
    const DocState before = m_state;
    Layer l;
    l.name = Adjustments::name(adjustment.type);
    l.image = blankLayer(size());
    l.mask = blankLayer(size(), Qt::white);
    l.adjustment = adjustment;
    m_state.active = insertionIndex();
    m_state.layers.insert(m_state.active, l);
    m_editMask = true;
    if (undoable) {
        finish(tr("New %1 Layer").arg(l.name), before);
    } else {
        emit structureChanged();
        emit imageChanged(rect());
    }
}

void Document::addTextLayer(const TextData &text)
{
    const DocState before = m_state;
    Layer l;
    l.name = textLayerName(text);
    l.text = text;
    l.image = renderText(text, size());
    m_state.active = insertionIndex();
    m_state.layers.insert(m_state.active, l);
    m_editMask = false;
    finish(tr("Text"), before);
}

void Document::setText(int i, const TextData &text)
{
    if (!layer(i).isText() || !text.isValid())
        return;
    const DocState before = m_state;
    Layer &l = layer(i);
    if (l.name == textLayerName(l.text))
        l.name = textLayerName(text);
    l.text = text;
    l.image = renderText(text, size());
    finish(tr("Edit Text"), before);
}

int Document::beginTextLayer(const TextData &text)
{
    Layer l;
    l.name = tr("Text");
    l.text = text;
    l.image = renderText(text, size());
    m_state.active = insertionIndex();
    m_state.layers.insert(m_state.active, l);
    m_editMask = false;
    emit structureChanged();
    emit imageChanged(textBounds(text).toAlignedRect() & rect());
    return m_state.active;
}

void Document::setTextLive(int i, const TextData &text)
{
    Layer &l = layer(i);
    // Repaint the old and new text area, with room for italic overhang.
    const qreal slack = std::max<qreal>(4, text.font.pixelSize() * 0.5);
    const QRectF dirty = textBounds(l.text) | textBounds(text);
    l.text = text;
    l.image = renderText(text, size());
    emit imageChanged(dirty.adjusted(-slack, -slack, slack, slack).toAlignedRect() & rect());
}

void Document::finishTextEdit(int i, const DocState &before, bool isNew)
{
    Layer &l = layer(i);
    if (l.text.text.isEmpty()) {
        if (isNew || layerCount() <= 1) {
            setState(before);  // nothing was typed: no layer, no undo step
            return;
        }
        m_state.layers.removeAt(i);  // all text deleted: the layer goes too
        m_state.active = std::clamp(i - 1, 0, layerCount() - 1);
        finish(tr("Delete Layer"), before);
        return;
    }
    if (!isNew) {
        const TextData &old = before.layers.at(i).text;
        if (old.text == l.text.text && old.font == l.text.font && old.color == l.text.color && old.pos == l.text.pos
            && old.antialias == l.text.antialias)
            return;  // no changes
        if (before.layers.at(i).name != textLayerName(old))
            return finish(tr("Edit Text"), before);  // keep a name the user chose
    }
    l.name = textLayerName(l.text);
    finish(isNew ? tr("Text") : tr("Edit Text"), before);
}

void Document::rasterizeLayer(int i)
{
    if (!layer(i).isText())
        return;
    const DocState before = m_state;
    layer(i).text = TextData();
    finish(tr("Rasterize Layer"), before, Structure);
}

void Document::duplicateLayer()
{
    const DocState before = m_state;
    const auto [first, last] = blockOf(m_state.active);
    QList<Layer> copy = m_state.layers.mid(first, last - first + 1);
    copy.last().name = tr("%1 copy").arg(copy.last().name);
    for (int k = 0; k < copy.size(); ++k)
        m_state.layers.insert(last + 1 + k, copy[k]);
    m_state.active = last + int(copy.size());
    finish(tr("Duplicate Layer"), before);
}

void Document::deleteLayer()
{
    const auto [first, last] = blockOf(m_state.active);
    int remaining = 0;
    for (int i = 0; i < layerCount(); ++i)
        remaining += (i < first || i > last) && layer(i).kind == LayerKind::Normal;
    if (remaining == 0)
        return;  // keep at least one layer
    const DocState before = m_state;
    m_state.layers.remove(first, last - first + 1);
    int a = std::min(first, layerCount() - 1);
    while (a > 0 && layer(a).isGroupEnd())
        --a;
    while (a < layerCount() - 1 && layer(a).isGroupEnd())
        ++a;
    m_state.active = a;
    m_editMask = false;
    finish(tr("Delete Layer"), before);
}

void Document::mergeDown()
{
    const int a = m_state.active;
    if (layer(a).isGroup()) {
        // Merge Group: the group becomes one layer that looks the same.
        const DocState before = m_state;
        const auto [first, last] = blockOf(a);
        Layer merged;
        merged.name = layer(a).name;
        merged.image = renderLayer(a);  // contents and mask, at full opacity
        merged.opacity = layer(a).opacity;
        merged.mode = layer(a).mode;
        merged.visible = layer(a).visible;
        m_state.layers.remove(first, last - first + 1);
        m_state.layers.insert(first, merged);
        m_state.active = first;
        m_editMask = false;
        finish(tr("Merge Group"), before);
        return;
    }
    if (a <= 0 || layer(a - 1).kind != LayerKind::Normal || layer(a - 1).isAdjustment())
        return;  // nothing suitable directly below (bottom of a group, a group, or an adjustment layer)
    const DocState before = m_state;
    const Layer upper = m_state.layers.at(a);
    Layer &lower = m_state.layers[a - 1];
    bakeMask(lower);
    lower.text = TextData();
    if (upper.clipped)
        clipOnto(lower.image, upper, rect());
    else
        compositeLayer(lower.image, upper, rect());
    m_state.layers.removeAt(a);
    m_state.active = a - 1;
    m_editMask = false;
    finish(tr("Merge Down"), before);
}

void Document::moveLayer(int delta)
{
    // The layer (or whole group) swaps places with the item next to it. Passing a group's
    // top or bottom edge moves it into or out of that group, as in Photoshop.
    const auto [first, last] = blockOf(m_state.active);
    if (delta > 0 && last + 1 < layerCount()) {
        const DocState before = m_state;
        m_state.layers.move(last + 1, first);
        ++m_state.active;
        finish(tr("Raise Layer"), before);
    } else if (delta < 0 && first > 0) {
        const DocState before = m_state;
        m_state.layers.move(first - 1, last);
        --m_state.active;
        finish(tr("Lower Layer"), before);
    }
}

// ---------------------------------------------------------------------------
// Groups

QPair<int, int> Document::blockOf(int i) const
{
    if (layer(i).isGroup()) {
        const int e = groupEndFor(i);
        if (e >= 0)
            return {e, i};
    }
    return {i, i};
}

int Document::groupEndFor(int header) const
{
    int depth = 0;
    for (int i = header - 1; i >= 0; --i) {
        if (layer(i).isGroup()) {
            ++depth;
        } else if (layer(i).isGroupEnd()) {
            if (depth == 0)
                return i;
            --depth;
        }
    }
    return -1;
}

int Document::parentGroup(int i) const
{
    int depth = 0;
    for (int j = i + 1; j < layerCount(); ++j) {
        if (layer(j).isGroupEnd()) {
            ++depth;
        } else if (layer(j).isGroup()) {
            if (depth == 0)
                return j;
            --depth;
        }
    }
    return -1;
}

int Document::insertionIndex() const
{
    // With a group selected, new layers go inside it, at the top.
    return activeLayer().isGroup() ? m_state.active : m_state.active + 1;
}

namespace {
Layer groupLayer(LayerKind kind, const QString &name, const QSize &size)
{
    Layer l;
    l.kind = kind;
    l.name = name;
    l.image = blankLayer(size);
    return l;
}
} // namespace

void Document::newGroup()
{
    const DocState before = m_state;
    const int at = insertionIndex();
    m_state.layers.insert(at, groupLayer(LayerKind::GroupEnd, QString(), size()));
    m_state.layers.insert(at + 1, groupLayer(LayerKind::Group, tr("Group %1").arg(++m_layerCounter), size()));
    m_state.active = at + 1;
    m_editMask = false;
    finish(tr("New Group"), before, Structure);
}

void Document::groupActiveLayer()
{
    const DocState before = m_state;
    const auto [first, last] = blockOf(m_state.active);
    m_state.layers.insert(last + 1, groupLayer(LayerKind::Group, tr("Group %1").arg(++m_layerCounter), size()));
    m_state.layers.insert(first, groupLayer(LayerKind::GroupEnd, QString(), size()));
    m_state.active = last + 2;
    m_editMask = false;
    finish(tr("Group Layers"), before, Structure);
}

void Document::ungroup()
{
    const int h = m_state.active;
    const int e = layer(h).isGroup() ? groupEndFor(h) : -1;
    if (e < 0)
        return;
    const DocState before = m_state;
    m_state.layers.removeAt(h);
    m_state.layers.removeAt(e);
    int a = std::clamp(h - 2, 0, layerCount() - 1);  // the group's top layer
    while (a > 0 && layer(a).isGroupEnd())
        --a;
    m_state.active = a;
    m_editMask = false;
    finish(tr("Ungroup Layers"), before);
}

void Document::flatten()
{
    const DocState before = m_state;
    Layer l;
    l.name = tr("Background");
    l.image = flattened();
    m_state.layers = {l};
    m_state.active = 0;
    finish(tr("Flatten Image"), before);
}

void Document::setLayerVisible(int i, bool visible)
{
    if (layer(i).visible == visible)
        return;
    const DocState before = m_state;
    layer(i).visible = visible;
    finish(visible ? tr("Show Layer") : tr("Hide Layer"), before, Pixels | Structure);
}

void Document::setLayerOpacity(int i, qreal opacity)
{
    if (qFuzzyCompare(layer(i).opacity, opacity))
        return;
    const DocState before = m_state;
    layer(i).opacity = opacity;
    finish(tr("Layer Opacity"), before, Pixels | Structure, 1000 + i);
}

void Document::setLayerMode(int i, QPainter::CompositionMode mode)
{
    if (layer(i).mode == mode)
        return;
    const DocState before = m_state;
    layer(i).mode = mode;
    finish(tr("Blend Mode"), before, Pixels | Structure);
}

void Document::renameLayer(int i, const QString &name)
{
    if (name.isEmpty() || layer(i).name == name)
        return;
    const DocState before = m_state;
    layer(i).name = name;
    finish(tr("Rename Layer"), before, Structure);
}

void Document::setLayerStyle(int i, const LayerStyle &style)
{
    if (layer(i).style == style)
        return;
    const DocState before = m_state;
    layer(i).style = style;
    finish(style.any() ? tr("Layer Style") : tr("Clear Layer Style"), before, Pixels | Structure);
}

void Document::setLayerClipped(int i, bool clipped)
{
    if (layer(i).clipped == clipped || layer(i).kind != LayerKind::Normal)
        return;
    const DocState before = m_state;
    layer(i).clipped = clipped;
    finish(clipped ? tr("Create Clipping Mask") : tr("Release Clipping Mask"), before, Pixels | Structure);
}

void Document::setLayerFillOpacity(int i, qreal fill)
{
    if (qFuzzyCompare(layer(i).fillOpacity, fill))
        return;
    const DocState before = m_state;
    layer(i).fillOpacity = fill;
    finish(tr("Fill Opacity"), before, Pixels | Structure, 2000 + i);
}

QImage Document::renderLayer(int i) const
{
    if (layer(i).isGroup()) {
        const auto [first, last] = blockOf(i);
        QList<Layer> block = m_state.layers.mid(first, last - first + 1);
        Layer &g = block.last();
        g.visible = true;
        g.opacity = 1.0;
        g.mode = QPainter::CompositionMode_SourceOver;
        QImage out = blankLayer(size());
        compositeRange(out, block, matchGroups(block), 0, int(block.size()), rect());
        return out;
    }
    Layer l = layer(i);
    l.visible = true;
    l.opacity = 1.0;
    l.mode = QPainter::CompositionMode_SourceOver;
    QImage out = blankLayer(size());
    compositeLayer(out, l, rect());
    return out;
}

int Document::effectsMargin() const
{
    int m = 0;
    for (const Layer &l : m_state.layers)
        if (l.visible && !l.isAdjustment())
            m = std::max(m, l.style.margin());
    return m;
}

// ---------------------------------------------------------------------------
// Layer masks

void Document::addMask(bool fromSelection)
{
    Layer &l = activeLayer();
    if (!l.mask.isNull())
        return;
    const DocState before = m_state;
    QImage mask;
    if (fromSelection && hasSelection()) {
        mask = blankLayer(size(), Qt::black);
        QImage white = blankLayer(size(), Qt::white);
        QPainter p(&white);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, m_state.selection);
        p.end();
        p.begin(&mask);
        p.drawImage(0, 0, white);
    } else {
        mask = blankLayer(size(), Qt::white);
    }
    activeLayer().mask = mask;
    activeLayer().maskEnabled = true;
    m_editMask = true;
    finish(tr("Add Layer Mask"), before);
}

void Document::deleteMask()
{
    if (activeLayer().mask.isNull() || activeLayer().isAdjustment())
        return;
    const DocState before = m_state;
    activeLayer().mask = QImage();
    m_editMask = false;
    finish(tr("Delete Layer Mask"), before);
}

void Document::applyMask()
{
    if (activeLayer().mask.isNull() || activeLayer().isAdjustment())
        return;
    const DocState before = m_state;
    Layer &l = activeLayer();
    bakeMask(l);
    l.text = TextData();
    m_editMask = false;
    finish(tr("Apply Layer Mask"), before);
}

void Document::setMaskEnabled(bool enabled)
{
    if (activeLayer().mask.isNull() || activeLayer().maskEnabled == enabled)
        return;
    const DocState before = m_state;
    activeLayer().maskEnabled = enabled;
    finish(enabled ? tr("Enable Layer Mask") : tr("Disable Layer Mask"), before, Pixels | Structure);
}

// ---------------------------------------------------------------------------
// Whole image

void Document::resizeImage(const QSize &s)
{
    if (s == size() || s.isEmpty())
        return;
    const DocState before = m_state;
    for (Layer &l : m_state.layers) {
        l.image = l.image.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (!l.mask.isNull())
            l.mask = l.mask.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        l.text = TextData();
    }
    m_state.size = s;
    m_state.selection = QImage();
    finish(tr("Image Size"), before);
}

void Document::resizeCanvas(const QSize &s, const QPoint &offset)
{
    if (s.isEmpty() || (s == size() && offset.isNull()))
        return;
    const DocState before = m_state;
    auto place = [&](const QImage &img, const QColor &fill) {
        QImage n = blankLayer(s, fill);
        QPainter p(&n);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(offset, img);
        return n;
    };
    for (Layer &l : m_state.layers) {
        l.image = place(l.image, Qt::transparent);
        if (!l.mask.isNull())
            l.mask = place(l.mask, Qt::white);
        l.text.pos += offset;
    }
    m_state.size = s;
    m_state.selection = QImage();
    finish(tr("Canvas Size"), before);
}

void Document::rotate(int degrees)
{
    const DocState before = m_state;
    QTransform t;
    t.rotate(degrees);
    for (Layer &l : m_state.layers) {
        l.image = l.image.transformed(t);
        if (!l.mask.isNull())
            l.mask = l.mask.transformed(t);
        l.text = TextData();
    }
    m_state.size = m_state.layers.first().image.size();
    m_state.selection = QImage();
    finish(tr("Rotate Canvas"), before);
}

void Document::flip(Qt::Orientation o)
{
    const DocState before = m_state;
    for (Layer &l : m_state.layers) {
        l.image = flipImage(l.image, o);
        if (!l.mask.isNull())
            l.mask = flipImage(l.mask, o);
        l.text = TextData();
    }
    if (hasSelection())
        m_state.selection = flipImage(m_state.selection, o);
    finish(o == Qt::Horizontal ? tr("Flip Horizontal") : tr("Flip Vertical"), before);
}

void Document::crop(const QRect &r)
{
    const QRect c = r & rect();
    if (c.isEmpty() || c == rect())
        return;
    const DocState before = m_state;
    for (Layer &l : m_state.layers) {
        l.image = l.image.copy(c);
        if (!l.mask.isNull())
            l.mask = l.mask.copy(c);
        l.text.pos -= c.topLeft();
    }
    if (hasSelection())
        m_state.selection = m_state.selection.copy(c);
    m_state.size = c.size();
    finish(tr("Crop"), before);
}

void Document::cropRotated(const QPointF &center, const QSize &s, qreal angle)
{
    if (s.isEmpty())
        return;
    if (std::abs(angle) < 0.01) {
        crop(QRect((center - QPointF(s.width(), s.height()) / 2).toPoint(), s));
        return;
    }
    const DocState before = m_state;
    // Maps the old image into the new one: frame center to the middle, rotated back upright.
    QTransform t;
    t.translate(s.width() / 2.0, s.height() / 2.0);
    t.rotate(-angle);
    t.translate(-center.x(), -center.y());
    auto render = [&](const QImage &img, const QColor &fill) {
        QImage out = blankLayer(s, fill);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(t);
        p.drawImage(0, 0, img);
        return out;
    };
    for (Layer &l : m_state.layers) {
        l.image = render(l.image, Qt::transparent);
        if (!l.mask.isNull())
            l.mask = render(l.mask, Qt::white);
        l.text = TextData();  // rotated text becomes pixels
    }
    m_state.size = s;
    m_state.selection = QImage();
    finish(tr("Crop"), before);
}

// ---------------------------------------------------------------------------
// Pixel operations

void Document::applyToActive(const QString &text, const std::function<QImage(const QImage &)> &f)
{
    prepareForPixelEdit();
    const DocState before = m_state;
    QImage &target = targetImage();
    const QImage original = target;
    target = maskedBlend(original, f(original));
    finish(text, before, Pixels | Structure);
}

void Document::clearSelected()
{
    prepareForPixelEdit();
    const DocState before = m_state;
    QImage &target = targetImage();
    if (!hasSelection()) {
        target = blankLayer(size());
    } else {
        QPainter p(&target);
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.drawImage(0, 0, m_state.selection);
    }
    finish(tr("Clear"), before, Pixels | Structure);
}

void Document::fillSelected(const QColor &color)
{
    prepareForPixelEdit();
    const DocState before = m_state;
    QImage fill = blankLayer(size(), color);
    if (hasSelection()) {
        QPainter p(&fill);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, m_state.selection);
    }
    QPainter p(&targetImage());
    p.drawImage(0, 0, fill);
    p.end();
    finish(tr("Fill"), before, Pixels | Structure);
}

QImage Document::copySelected(bool merged) const
{
    QImage src = merged ? flattened() : targetImage();
    if (!hasSelection())
        return src;
    QImage m = src.copy();
    QPainter p(&m);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, m_state.selection);
    p.end();
    return m.copy(m_selectionBounds);
}

void Document::pasteImage(const QImage &image)
{
    QImage content = blankLayer(size());
    const QPoint pos = hasSelection()
        ? m_selectionBounds.topLeft()
        : QPoint((size().width() - image.width()) / 2, (size().height() - image.height()) / 2);
    QPainter p(&content);
    p.drawImage(pos, image);
    p.end();
    addLayer(tr("Pasted Layer"), content, tr("Paste"));
}
