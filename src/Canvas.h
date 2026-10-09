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
    ToolEvent toolEvent(QMouseEvent *e) const;
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

    QList<QLineF> m_ants;  // selection outline in image coordinates
    QTimer m_antsTimer;
    int m_antsPhase = 0;
};
