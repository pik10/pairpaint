// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QLineF>
#include <QList>
#include <QTimer>
#include <QTransform>
#include <QWidget>

class Document;
class Tool;
class ToolManager;
struct ToolEvent;

// Displays one document and forwards mouse/keyboard input to the current tool.
// Owns its Document.
class Canvas : public QWidget {
    Q_OBJECT
public:
    Canvas(Document *doc, ToolManager *tools, QWidget *parent = nullptr);

    Document *document() const { return m_doc; }
    bool isToolPressed() const { return m_toolPressed; }  // mid-stroke

    qreal zoom() const { return m_zoom; }
    void setZoom(qreal zoom, const QPointF &anchor);
    void zoomStep(int direction, const QPointF &anchor);
    void zoomIn() { zoomStep(1, rect().center()); }
    void zoomOut() { zoomStep(-1, rect().center()); }
    void fitToWindow(bool allowEnlarge = true);
    void actualPixels();
    void panBy(const QPointF &delta);

    // m_zoom is relative to physical screen pixels, so 100% shows one image pixel per
    // screen pixel even when the interface is scaled up.
    qreal scale() const { return m_zoom / devicePixelRatioF(); }
    QTransform imageToWidget() const { return QTransform(scale(), 0, 0, scale(), m_offset.x(), m_offset.y()); }
    QPointF mapToImage(const QPointF &p) const { return (p - m_offset) / scale(); }
    QPointF mapFromImage(const QPointF &p) const { return p * scale() + m_offset; }

    void updateCursor();
    void activateTool();  // lets the current tool set itself up for this document

    // Rulers along the top and left edge (when shown), in logical pixels; 0 when hidden.
    int rulerSize() const;
    // Snapping to guides and the canvas edges and center, within a few screen pixels: the shift
    // along x (`xAxis`) or y that puts the closest of `positions` on a line, or 0. The line is
    // highlighted while a tool is pressed.
    qreal snapOffset(bool xAxis, const QList<qreal> &positions);
    QPointF snapPoint(const QPointF &p) { return p + QPointF(snapOffset(true, {p.x()}), snapOffset(false, {p.y()})); }
    void clearSnapHighlight();
    int guideAt(const QPointF &widgetPos) const;  // the guide under the pointer, or -1

signals:
    void cursorMoved(const QPointF &imagePos);
    void zoomChanged(qreal zoom);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void tabletEvent(QTabletEvent *e) override;
    bool event(QEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    Tool *tool() const;
    ToolEvent toolEvent(QMouseEvent *e);
    void drawGuides(QPainter &p);
    void drawRulers(QPainter &p);
    void updateGuideDrag(const QPointF &widgetPos);
    void updateHoverCursor(const QPointF &widgetPos);
    void onImageChanged(const QRect &r);
    void rebuildAnts();
    void drawAnts(QPainter &p);
    void syncInputMethod();  // keyboard text input is on only while the tool is typing

    Document *m_doc;
    ToolManager *m_tools;
    QImage m_cache;  // composite of all layers
    qreal m_zoom = 1.0;
    QPointF m_offset;
    bool m_viewInitialized = false;

    bool m_spaceDown = false;
    bool m_panning = false;
    bool m_toolPressed = false;
    QPointF m_panLast;
    QPointF m_cursorImage;
    bool m_cursorInside = false;
    qreal m_pressure = 1.0;

    struct GuideDrag {
        bool active = false;
        int index = -1;  // the guide being moved, or -1 for a new one dragged from a ruler
        Qt::Orientation orientation = Qt::Horizontal;
        qreal pos = 0;
    } m_guideDrag;
    qreal m_snapX = qQNaN(), m_snapY = qQNaN();  // highlighted snap lines (image coordinates)
    int m_hoverZone = -1;  // what the pointer is over, to change the cursor only when needed

    QList<QLineF> m_ants;  // selection outline in image coordinates
    QTimer m_antsTimer;
    int m_antsPhase = 0;
};
