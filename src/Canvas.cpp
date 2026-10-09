// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Canvas.h"

#include "Document.h"
#include "ToolSettings.h"
#include "Tools.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

const qreal kZoomSteps[] = {0.02, 0.03, 0.05, 0.0625, 0.0833, 0.125, 0.1667, 0.25, 0.3333, 0.5, 0.6667,
                            1, 1.5, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32, 48, 64};
constexpr qreal kMinZoom = 0.02;
constexpr qreal kMaxZoom = 64;

const QPixmap &checkerPixmap()
{
    static const QPixmap pm = [] {
        QPixmap p(16, 16);
        p.fill(Qt::white);
        QPainter q(&p);
        q.fillRect(0, 0, 8, 8, QColor(204, 204, 204));
        q.fillRect(8, 8, 8, 8, QColor(204, 204, 204));
        return p;
    }();
    return pm;
}

const QPixmap &antsPixmap()
{
    static const QPixmap pm = [] {
        QImage img(8, 8, QImage::Format_RGB32);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                img.setPixel(x, y, ((x + y) % 8) < 4 ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
        return QPixmap::fromImage(img);
    }();
    return pm;
}

} // namespace

Canvas::Canvas(Document *doc, ToolManager *tools, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_tools(tools)
{
    doc->setParent(this);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_cache = doc->flattened();

    connect(doc, &Document::imageChanged, this, &Canvas::onImageChanged);
    connect(doc, &Document::sizeChanged, this, [this] {
        m_cache = m_doc->flattened();
        fitToWindow(false);
    });
    connect(doc, &Document::selectionChanged, this, &Canvas::rebuildAnts);
    connect(doc, &Document::guidesChanged, this, qOverload<>(&Canvas::update));
    connect(tools, &ToolManager::toolChanged, this, [this] {
        m_hoverZone = -1;
        updateCursor();
        update();
    });

    m_antsTimer.setInterval(120);
    connect(&m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsPhase = (m_antsPhase + 1) % 8;
        update();
    });
    rebuildAnts();
    updateCursor();
}

Tool *Canvas::tool() const
{
    Tool *t = m_tools->current();
    t->setContext(m_doc, const_cast<Canvas *>(this));
    return t;
}

void Canvas::activateTool()
{
    tool()->activated();
    syncInputMethod();
    updateCursor();
    update();
}

void Canvas::updateCursor()
{
    if (m_spaceDown || m_panning)
        setCursor(m_panning ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    else
        setCursor(tool()->cursor());
}

// ---------------------------------------------------------------------------
// View

void Canvas::setZoom(qreal zoom, const QPointF &anchor)
{
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const QPointF imgPt = mapToImage(anchor);
    m_zoom = zoom;
    m_offset = anchor - imgPt * scale();
    emit zoomChanged(m_zoom);
    update();
}

void Canvas::zoomStep(int direction, const QPointF &anchor)
{
    qreal z = m_zoom;
    if (direction > 0) {
        for (qreal s : kZoomSteps)
            if (s > m_zoom * 1.001) { z = s; break; }
    } else {
        for (auto it = std::rbegin(kZoomSteps); it != std::rend(kZoomSteps); ++it)
            if (*it < m_zoom / 1.001) { z = *it; break; }
    }
    setZoom(z, anchor);
}

void Canvas::fitToWindow(bool allowEnlarge)
{
    const QSize s = m_doc->size();
    if (width() <= 0 || height() <= 0 || s.isEmpty())
        return;
    const int r = rulerSize();
    const qreal w = width() - r, h = height() - r;
    qreal z = std::min((w - 40.0) / s.width(), (h - 40.0) / s.height()) * devicePixelRatioF();
    if (!allowEnlarge)
        z = std::min<qreal>(z, 1.0);
    m_zoom = std::clamp(z, kMinZoom, kMaxZoom);
    m_offset = QPointF(r + (w - s.width() * scale()) / 2.0, r + (h - s.height() * scale()) / 2.0);
    emit zoomChanged(m_zoom);
    update();
}

void Canvas::actualPixels()
{
    m_zoom = 1.0;
    const QSize s = m_doc->size();
    const int r = rulerSize();
    m_offset = QPointF(r + std::round((width() - r - s.width() * scale()) / 2.0),
                       r + std::round((height() - r - s.height() * scale()) / 2.0));
    emit zoomChanged(m_zoom);
    update();
}

void Canvas::panBy(const QPointF &delta)
{
    m_offset += delta;
    update();
}

void Canvas::resizeEvent(QResizeEvent *)
{
    if (!m_viewInitialized && width() > 50 && height() > 50) {
        m_viewInitialized = true;
        fitToWindow(false);
    }
}

// ---------------------------------------------------------------------------
// Rendering

void Canvas::onImageChanged(const QRect &r)
{
    // Layer effects (e.g. a drop shadow) change pixels beyond the edited area.
    const int m = m_doc->effectsMargin();
    const QRect rr = r.adjusted(-m, -m, m, m) & m_doc->rect();
    if (m_cache.size() != m_doc->size() || rr == m_doc->rect()) {
        m_cache = m_doc->flattened();
    } else if (!rr.isEmpty()) {
        const QImage part = m_doc->composite(rr);
        QPainter p(&m_cache);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(rr.topLeft(), part);
    }
    update();
}

void Canvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(38, 38, 38));

    const QRectF imgRect(m_offset, QSizeF(m_doc->size()) * scale());
    QBrush checker(checkerPixmap());
    checker.setTransform(QTransform::fromTranslate(m_offset.x(), m_offset.y()));
    p.fillRect(imgRect, checker);

    p.save();
    p.setTransform(imageToWidget());
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    const QRect src = imageToWidget().inverted().mapRect(QRectF(rect())).toAlignedRect() & m_cache.rect();
    if (!src.isEmpty())
        p.drawImage(QRectF(src), m_cache, QRectF(src));
    p.restore();

    p.setPen(QColor(0, 0, 0, 180));
    p.setBrush(Qt::NoBrush);
    p.drawRect(imgRect.adjusted(-0.5, -0.5, 0.5, 0.5));

    drawAnts(p);
    drawGuides(p);

    Tool *t = tool();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    t->paintOverlay(p, imageToWidget());
    p.restore();

    if (t->showsBrushOutline() && m_cursorInside && !m_spaceDown && !m_panning) {
        const qreal r = std::max<qreal>(1.0, m_tools->settings()->size / 2.0 * scale());
        const QPointF c = mapFromImage(m_cursorImage);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1.5));
        p.drawEllipse(c, r + 0.75, r + 0.75);
        p.setPen(QPen(QColor(255, 255, 255, 220), 1));
        p.drawEllipse(c, r, r);
    }
    drawRulers(p);
}

// ---------------------------------------------------------------------------
// Rulers, guides and snapping

int Canvas::rulerSize() const { return m_tools->settings()->showRulers ? 20 : 0; }

void Canvas::drawGuides(QPainter &p)
{
    const bool show = m_tools->settings()->showGuides;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    auto line = [&](Qt::Orientation o, qreal pos, const QColor &color) {
        p.setPen(QPen(color, 1));
        if (o == Qt::Horizontal) {
            const qreal y = std::floor(mapFromImage(QPointF(0, pos)).y()) + 0.5;
            p.drawLine(QPointF(0, y), QPointF(width(), y));
        } else {
            const qreal x = std::floor(mapFromImage(QPointF(pos, 0)).x()) + 0.5;
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
    };
    const QColor guide(0, 210, 255);
    if (show) {
        const QList<Guide> &guides = m_doc->guides();
        for (int i = 0; i < guides.size(); ++i)
            if (!(m_guideDrag.active && m_guideDrag.index == i))
                line(guides[i].orientation, guides[i].pos, guide);
    }
    if (m_guideDrag.active)
        line(m_guideDrag.orientation, m_guideDrag.pos, guide);
    // What the tool snapped to, while it is pressed.
    if (m_toolPressed) {
        const QColor snap(255, 60, 200);
        if (!qIsNaN(m_snapX))
            line(Qt::Vertical, m_snapX, snap);
        if (!qIsNaN(m_snapY))
            line(Qt::Horizontal, m_snapY, snap);
    }
    p.restore();
}

void Canvas::drawRulers(QPainter &p)
{
    const int r = rulerSize();
    if (r == 0)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont f = font();
    f.setPixelSize(9);
    p.setFont(f);
    const QColor background(48, 48, 48), ink(170, 170, 170), border(25, 25, 25);
    p.fillRect(QRect(0, 0, width(), r), background);
    p.fillRect(QRect(0, 0, r, height()), background);

    // Numbered ticks at the smallest "round" step (1, 2 or 5 times a power of ten image pixels)
    // that is at least 50 screen pixels apart, with smaller ticks in between.
    const qreal s = scale();
    qreal step = 0;
    for (qreal base = 1; step == 0 && base < 1e9; base *= 10)
        for (qreal n : {1.0, 2.0, 5.0})
            if (step == 0 && n * base * s >= 50)
                step = n * base;
    if (step == 0)
        step = 1e9;
    const int minorCount = step * s / 10 >= 5 ? 10 : step * s / 5 >= 5 ? 5 : step * s / 2 >= 5 ? 2 : 1;
    auto axis = [&](bool horizontal) {
        const qreal origin = horizontal ? m_offset.x() : m_offset.y();
        const qreal length = horizontal ? width() : height();
        const qint64 first = qint64(std::floor((r - origin) / s / step));
        const qint64 last = qint64(std::ceil((length - origin) / s / step));
        p.setPen(ink);
        for (qint64 k = first; k <= last; ++k) {
            const qreal v = k * step;
            const qreal at = std::floor(origin + v * s) + 0.5;
            for (int m = 1; m < minorCount; ++m) {
                const qreal mat = std::floor(origin + (v + step * m / minorCount) * s) + 0.5;
                const qreal tick = (minorCount == 10 && m == 5) ? r * 0.45 : r * 0.7;
                if (mat > r)
                    horizontal ? p.drawLine(QPointF(mat, tick), QPointF(mat, r)) : p.drawLine(QPointF(tick, mat), QPointF(r, mat));
            }
            if (at <= r)
                continue;
            const QString label = QString::number(v, 'g', 10);
            if (horizontal) {
                p.drawLine(QPointF(at, 0), QPointF(at, r));
                p.drawText(QPointF(at + 3, 10), label);
            } else {
                p.drawLine(QPointF(0, at), QPointF(r, at));
                p.save();
                p.translate(11, at + 3);
                p.rotate(-90);
                p.drawText(QPointF(-p.fontMetrics().horizontalAdvance(label), 0), label);
                p.restore();
            }
        }
    };
    axis(true);
    axis(false);
    // Where the pointer is.
    if (m_cursorInside) {
        const QPointF c = mapFromImage(m_cursorImage);
        p.setPen(QColor(255, 255, 255, 200));
        if (c.x() > r)
            p.drawLine(QPointF(std::floor(c.x()) + 0.5, 0), QPointF(std::floor(c.x()) + 0.5, r));
        if (c.y() > r)
            p.drawLine(QPointF(0, std::floor(c.y()) + 0.5), QPointF(r, std::floor(c.y()) + 0.5));
    }
    p.setPen(border);
    p.drawLine(QPointF(r - 0.5, r - 0.5), QPointF(width(), r - 0.5));
    p.drawLine(QPointF(r - 0.5, r - 0.5), QPointF(r - 0.5, height()));
    p.fillRect(QRect(0, 0, r - 1, r - 1), background);
    p.restore();
}

int Canvas::guideAt(const QPointF &w) const
{
    if (!m_tools->settings()->showGuides)
        return -1;
    const QList<Guide> &guides = m_doc->guides();
    for (int i = int(guides.size()) - 1; i >= 0; --i) {  // the newest on top
        const QPointF at = mapFromImage(QPointF(guides[i].pos, guides[i].pos));
        const qreal d = guides[i].orientation == Qt::Horizontal ? std::abs(at.y() - w.y()) : std::abs(at.x() - w.x());
        if (d <= 4)
            return i;
    }
    return -1;
}

qreal Canvas::snapOffset(bool xAxis, const QList<qreal> &positions)
{
    qreal &highlight = xAxis ? m_snapX : m_snapY;
    highlight = qQNaN();
    const ToolSettings *settings = m_tools->settings();
    if (!settings->snap || positions.isEmpty())
        return 0;
    const qreal extent = xAxis ? m_doc->size().width() : m_doc->size().height();
    QList<qreal> targets{0, extent / 2, extent};  // the edges and the center
    if (settings->showGuides)
        for (const Guide &g : m_doc->guides())
            if ((g.orientation == Qt::Vertical) == xAxis)
                targets << g.pos;
    qreal best = 8 / scale();  // 8 screen pixels
    qreal shift = 0;
    for (qreal p : positions) {
        for (qreal t : std::as_const(targets)) {
            if (std::abs(t - p) < std::abs(best)) {
                best = t - p;
                shift = t - p;
                highlight = t;
            }
        }
    }
    return shift;
}

void Canvas::clearSnapHighlight()
{
    m_snapX = m_snapY = qQNaN();
}

void Canvas::updateGuideDrag(const QPointF &w)
{
    const QPointF img = mapToImage(w);
    m_guideDrag.pos = std::round(m_guideDrag.orientation == Qt::Horizontal ? img.y() : img.x());  // on pixel edges
    update();
}

// Arrow over the rulers, a resize cursor over guides the Move tool can drag, else the tool's.
void Canvas::updateHoverCursor(const QPointF &w)
{
    if (m_spaceDown || m_panning || m_toolPressed)
        return;
    const int r = rulerSize();
    int zone = 0;
    if (r > 0 && (w.x() < r || w.y() < r)) {
        zone = 1;
    } else if (m_tools->currentId() == Tool::Move) {
        const int g = guideAt(w);
        if (g >= 0)
            zone = m_doc->guides().at(g).orientation == Qt::Horizontal ? 2 : 3;
    }
    if (zone == m_hoverZone)
        return;
    m_hoverZone = zone;
    if (zone == 1)
        setCursor(Qt::ArrowCursor);
    else if (zone == 2)
        setCursor(Qt::SplitVCursor);
    else if (zone == 3)
        setCursor(Qt::SplitHCursor);
    else
        updateCursor();
}

void Canvas::rebuildAnts()
{
    m_ants.clear();
    if (!m_doc->hasSelection()) {
        m_antsTimer.stop();
        update();
        return;
    }
    const QImage &m = m_doc->selection();
    const int w = m.width(), h = m.height();
    const QRect b = m_doc->selectionBounds().adjusted(-1, -1, 1, 1) & m.rect();
    auto in = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && m.constScanLine(y)[x] >= 128;
    };

    // Horizontal edges lie between rows y-1 and y; vertical edges between columns x-1 and x.
    for (int y = b.top(); y <= b.bottom() + 1; ++y) {
        int start = -1;
        for (int x = b.left(); x <= b.right() + 1; ++x) {
            const bool edge = x <= b.right() && in(x, y - 1) != in(x, y);
            if (edge && start < 0) {
                start = x;
            } else if (!edge && start >= 0) {
                m_ants.append(QLineF(start, y, x, y));
                start = -1;
            }
        }
    }
    for (int x = b.left(); x <= b.right() + 1; ++x) {
        int start = -1;
        for (int y = b.top(); y <= b.bottom() + 1; ++y) {
            const bool edge = y <= b.bottom() && in(x - 1, y) != in(x, y);
            if (edge && start < 0) {
                start = y;
            } else if (!edge && start >= 0) {
                m_ants.append(QLineF(x, start, x, y));
                start = -1;
            }
        }
    }
    if (!m_antsTimer.isActive())
        m_antsTimer.start();
    update();
}

void Canvas::drawAnts(QPainter &p)
{
    if (m_ants.isEmpty())
        return;
    const QTransform t = imageToWidget();
    QList<QLineF> lines;
    lines.reserve(m_ants.size());
    for (const QLineF &l : m_ants)
        lines.append(t.map(l));

    // A diagonal stripe texture gives continuous "marching ants" regardless of
    // how the outline is split into segments.
    QBrush stripes(antsPixmap());
    stripes.setTransform(QTransform::fromTranslate(m_antsPhase, 0));
    QPen pen(stripes, 1);
    pen.setCosmetic(true);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(pen);
    p.drawLines(lines);
    p.restore();
}

// ---------------------------------------------------------------------------
// Input

ToolEvent Canvas::toolEvent(QMouseEvent *e)
{
    ToolEvent t;
    t.widgetPos = e->position();
    t.pos = mapToImage(e->position());
    if (tool()->snapsToGuides())
        t.pos = snapPoint(t.pos);
    t.button = e->button();
    t.buttons = e->buttons();
    t.modifiers = e->modifiers();
    // Qt turns unaccepted tablet events into mouse events from the stylus device.
    const QPointingDevice *dev = e->pointingDevice();
    const bool pen = dev && (dev->type() == QInputDevice::DeviceType::Stylus
                             || dev->type() == QInputDevice::DeviceType::Airbrush);
    t.pressure = pen ? std::clamp(m_pressure, 0.05, 1.0) : 1.0;
    return t;
}

void Canvas::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (e->button() == Qt::MiddleButton || (m_spaceDown && e->button() == Qt::LeftButton)) {
        m_panning = true;
        m_panLast = e->position();
        updateCursor();
        return;
    }
    if (m_toolPressed && (e->buttons() & ~e->button()))
        return;  // ignore a second button while a tool drag is in progress
    if (e->button() == Qt::LeftButton && !m_toolPressed) {
        const QPointF w = e->position();
        const int r = rulerSize();
        if (r > 0 && (w.x() < r || w.y() < r)) {  // dragging a new guide out of a ruler
            if (w.x() >= r || w.y() >= r) {
                m_guideDrag = {true, -1, w.y() < r ? Qt::Horizontal : Qt::Vertical, 0};
                updateGuideDrag(w);
            }
            return;
        }
        if (m_tools->currentId() == Tool::Move) {  // the Move tool also moves guides
            const int g = guideAt(w);
            if (g >= 0) {
                const Guide &guide = m_doc->guides().at(g);
                m_guideDrag = {true, g, guide.orientation, guide.pos};
                return;
            }
        }
    }
    if (e->button() == Qt::LeftButton && Tool::editsPixels(m_tools->currentId()) && !m_doc->canEditPixels()) {
        QToolTip::showText(e->globalPosition().toPoint(),
                           tr("A group is selected. Select a layer inside it to paint, or add a mask to the group."));
        return;
    }
    m_toolPressed = true;
    tool()->press(toolEvent(e));
    syncInputMethod();
    update();
}

void Canvas::mouseMoveEvent(QMouseEvent *e)
{
    m_cursorImage = mapToImage(e->position());
    m_cursorInside = true;
    emit cursorMoved(m_cursorImage);
    if (m_panning) {
        panBy(e->position() - m_panLast);
        m_panLast = e->position();
        return;
    }
    if (m_guideDrag.active) {
        updateGuideDrag(e->position());
        return;
    }
    updateHoverCursor(e->position());
    tool()->move(toolEvent(e));
    update();
}

void Canvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panning && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        m_panning = false;
        updateCursor();
        return;
    }
    if (m_guideDrag.active && e->button() == Qt::LeftButton) {
        // Dropped on the canvas: placed. Dropped back on a ruler or outside: removed.
        m_guideDrag.active = false;
        const int r = rulerSize();
        const bool onCanvas = QRectF(rect()).adjusted(r, r, 0, 0).contains(e->position());
        const GuideDrag d = m_guideDrag;
        if (d.index < 0 && onCanvas)
            m_doc->addGuide({d.orientation, d.pos});
        else if (d.index >= 0 && onCanvas)
            m_doc->moveGuide(d.index, d.pos);
        else if (d.index >= 0)
            m_doc->removeGuide(d.index);
        m_hoverZone = -1;
        updateHoverCursor(e->position());
        update();
        return;
    }
    if (m_toolPressed && e->buttons() == Qt::NoButton) {
        m_toolPressed = false;
        tool()->release(toolEvent(e));
        clearSnapHighlight();
        update();
    }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e)
{
    tool()->doubleClick(toolEvent(e));
    mousePressEvent(e);
}

void Canvas::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const qreal factor = std::pow(1.0015, e->angleDelta().y());
        setZoom(m_zoom * factor, e->position());
    } else {
        QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2.0 : QPointF(e->pixelDelta());
        if (e->modifiers() & Qt::ShiftModifier)
            d = QPointF(d.y(), d.x());
        panBy(d);
    }
    e->accept();
}

void Canvas::keyPressEvent(QKeyEvent *e)
{
    if (tool()->capturesKeyboard() && tool()->keyPress(e)) {  // typing text: every key goes to the tool
        syncInputMethod();
        update();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        if (!e->isAutoRepeat()) {
            m_spaceDown = true;
            updateCursor();
            update();
        }
        return;
    }
    if (tool()->keyPress(e)) {
        update();
        return;
    }
    QWidget::keyPressEvent(e);
}

void Canvas::keyReleaseEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceDown = false;
        updateCursor();
        update();
        return;
    }
    QWidget::keyReleaseEvent(e);
}

bool Canvas::event(QEvent *e)
{
    // While the Text tool is typing, letters must reach it instead of triggering
    // single-key shortcuts (B for Brush, ...).
    if (e->type() == QEvent::ShortcutOverride && tool()->capturesKeyboard()
        && tool()->wantsKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void Canvas::syncInputMethod()
{
    const bool on = tool()->capturesKeyboard();
    if (testAttribute(Qt::WA_InputMethodEnabled) != on) {
        setAttribute(Qt::WA_InputMethodEnabled, on);
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
    }
    if (on)
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}

void Canvas::inputMethodEvent(QInputMethodEvent *e)
{
    // Accented and other composed characters arrive here from the input method.
    if (tool()->capturesKeyboard()) {
        tool()->inputText(e->commitString());
        update();
    }
    e->accept();
}

QVariant Canvas::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return tool()->capturesKeyboard();
    case Qt::ImCursorRectangle:
        return imageToWidget().mapRect(tool()->caretRect());
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void Canvas::tabletEvent(QTabletEvent *e)
{
    m_pressure = e->pressure();
    e->ignore();  // let Qt synthesize the mouse event that drives the tools
}

void Canvas::leaveEvent(QEvent *)
{
    m_cursorInside = false;
    update();
}

void Canvas::focusOutEvent(QFocusEvent *)
{
    if (m_spaceDown) {
        m_spaceDown = false;
        updateCursor();
    }
}
