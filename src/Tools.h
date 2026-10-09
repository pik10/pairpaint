// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QCursor>
#include <QObject>
#include <QPointF>
#include <QTransform>
#include <array>
#include <memory>

class Canvas;
class Document;
class ToolSettings;
class QKeyEvent;
class QPainter;

struct ToolEvent {
    QPointF pos;        // image coordinates
    QPointF widgetPos;  // canvas widget coordinates
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers modifiers;
    qreal pressure = 1.0;  // pen pressure, 1.0 for mice
};

class Tool {
public:
    enum Id {
        Move, Transform, RectSelect, EllipseSelect, Lasso, MagicWand, Crop, Eyedropper,
        Brush, Eraser, CloneStamp, Healing, Smudge, Dodge, Burn, Fill, Gradient, LineShape, RectShape, EllipseShape,
        Text, Hand, Zoom, Count
    };

    explicit Tool(ToolSettings *settings) : m_settings(settings) {}
    virtual ~Tool() = default;

    virtual Id id() const = 0;
    virtual void activated() {}  // the tool was selected while a document is shown
    virtual void press(const ToolEvent &) {}
    virtual void move(const ToolEvent &) {}  // also called while hovering
    virtual void release(const ToolEvent &) {}
    virtual void doubleClick(const ToolEvent &) {}
    virtual bool keyPress(QKeyEvent *) { return false; }
    virtual void paintOverlay(QPainter &, const QTransform &) {}  // image -> widget transform
    virtual QCursor cursor() const { return Qt::CrossCursor; }
    virtual bool showsBrushOutline() const { return false; }
    virtual void cancel() {}  // finish or abort any interaction in progress

    void setContext(Document *doc, Canvas *canvas) { m_doc = doc; m_canvas = canvas; }

    static QString name(Id id);
    static QString shortcut(Id id);
    static QString hint(Id id);
    static bool editsPixels(Id id);  // tools that need a layer with pixels (not a group)

protected:
    ToolSettings *m_settings;
    Document *m_doc = nullptr;
    Canvas *m_canvas = nullptr;
};

class ToolManager : public QObject {
    Q_OBJECT
public:
    explicit ToolManager(ToolSettings *settings, QObject *parent = nullptr);
    ~ToolManager() override;

    Tool *current() const { return m_tools[m_current].get(); }
    Tool::Id currentId() const { return m_current; }
    void setCurrent(Tool::Id id);
    ToolSettings *settings() const { return m_settings; }

signals:
    void toolChanged(int id);

private:
    ToolSettings *m_settings;
    std::array<std::unique_ptr<Tool>, Tool::Count> m_tools;
    Tool::Id m_current = Tool::Brush;
};
