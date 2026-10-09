// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Adjustments.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QUndoStack>
#include <functional>

// Editable text of a text layer. The layer image is rendered from it.
struct TextData {
    QString text;
    QFont font;
    QColor color = Qt::black;
    QPointF pos;
    bool antialias = true;
    bool isValid() const { return !text.isEmpty(); }
};

struct Layer {
    QString name;
    QImage image;  // always Format_ARGB32_Premultiplied, same size as the document
    bool visible = true;
    qreal opacity = 1.0;
    QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;

    // Layer mask: ARGB32_Premultiplied grayscale, luminance = coverage
    // (white reveals, black hides). Null when the layer has no mask.
    QImage mask;
    bool maskEnabled = true;

    Adjustment adjustment;  // type != None makes this an adjustment layer
    TextData text;          // valid makes this an editable text layer

    bool isAdjustment() const { return adjustment.type != Adjustment::None; }
    bool isText() const { return text.isValid(); }
};

// Everything that takes part in undo/redo. QImage is implicitly shared, so a
// copy of DocState is cheap: only layers that are modified later get detached.
struct DocState {
    QSize size;
    QList<Layer> layers;  // index 0 is the bottom layer
    int active = 0;
    QImage selection;     // Format_Alpha8 mask; null when nothing is selected
};

enum class SelectionOp { Replace, Add, Subtract, Intersect };

QList<QPair<QString, QPainter::CompositionMode>> blendModes();
QImage renderText(const TextData &text, const QSize &size);
QRectF textBounds(const TextData &text);
QRect alphaBounds(const QImage &image);  // bounding rect of non-transparent pixels

class Document : public QObject {
    Q_OBJECT
public:
    Document(const QSize &size, const QColor &background, QObject *parent = nullptr);
    explicit Document(const QImage &image, QObject *parent = nullptr);
    explicit Document(const DocState &state, QObject *parent = nullptr);

    const DocState &state() const { return m_state; }
    void setState(const DocState &state);

    QSize size() const { return m_state.size; }
    QRect rect() const { return QRect(QPoint(0, 0), m_state.size); }

    int layerCount() const { return int(m_state.layers.size()); }
    const Layer &layer(int i) const { return m_state.layers.at(i); }
    Layer &layer(int i) { return m_state.layers[i]; }
    int activeIndex() const { return m_state.active; }
    const Layer &activeLayer() const { return m_state.layers.at(m_state.active); }
    Layer &activeLayer() { return m_state.layers[m_state.active]; }
    void setActiveIndex(int index);

    // What painting tools and filters modify: the active layer's pixels or its mask.
    bool editingMask() const;
    void setEditingMask(bool mask);
    QImage &targetImage();
    const QImage &targetImage() const;
    // Text layers become plain pixels as soon as their pixels are edited.
    void prepareForPixelEdit();

    // Selection
    bool hasSelection() const { return !m_state.selection.isNull(); }
    const QImage &selection() const { return m_state.selection; }
    QRect selectionBounds() const { return m_selectionBounds; }
    void setSelection(const QImage &mask);  // not undoable; used for live previews
    void selectMask(const QImage &shape, SelectionOp op, const QString &text);
    void selectPath(const QPainterPath &path, SelectionOp op, bool antialias, const QString &text);
    void selectAll();
    void deselect();
    void invertSelection();
    void featherSelection(double radius);
    void growSelection(double pixels);
    void shrinkSelection(double pixels);
    void borderSelection(double width);
    void smoothSelection(double radius);
    void selectLayerTransparency();

    // `modified` inside the selection, `original` outside it.
    QImage maskedBlend(const QImage &original, const QImage &modified) const;

    QImage composite(const QRect &r) const;
    QImage flattened() const { return composite(rect()); }

    // Layer operations (undoable)
    void addLayer(const QString &name = QString(), const QImage &content = QImage(),
                  const QString &undoText = QString());
    void addAdjustmentLayer(const Adjustment &adjustment, bool undoable = true);
    void addTextLayer(const TextData &text);
    void setText(int i, const TextData &text);
    void rasterizeLayer(int i);
    void duplicateLayer();
    void deleteLayer();
    void mergeDown();
    void moveLayer(int delta);
    void flatten();
    void setLayerVisible(int i, bool visible);
    void setLayerOpacity(int i, qreal opacity);
    void setLayerMode(int i, QPainter::CompositionMode mode);
    void renameLayer(int i, const QString &name);

    // Layer masks (undoable)
    void addMask(bool fromSelection);
    void deleteMask();
    void applyMask();
    void setMaskEnabled(bool enabled);

    // Whole-image operations (undoable)
    void resizeImage(const QSize &size);
    void resizeCanvas(const QSize &size, const QPoint &offset);
    void rotate(int degrees);
    void flip(Qt::Orientation orientation);
    void crop(const QRect &r);

    // Pixel operations on the edit target, limited to the selection (undoable)
    void applyToActive(const QString &text, const std::function<QImage(const QImage &)> &f);
    void clearSelected();
    void fillSelected(const QColor &color);
    QImage copySelected(bool merged) const;
    void pasteImage(const QImage &image);

    QUndoStack *undoStack() { return &m_undo; }
    void commit(const QString &text, const DocState &before, int mergeId = -1);
    void notifyImageChanged(const QRect &r = QRect()) { emit imageChanged(r.isNull() ? rect() : r); }
    void notifyStructureChanged() { emit structureChanged(); }

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString &path);
    QString displayName() const;
    bool isModified() const { return !m_undo.isClean(); }

signals:
    void imageChanged(const QRect &rect);
    void structureChanged();  // layers, layer properties, active layer or edit target
    void selectionChanged();
    void sizeChanged();
    void titleChanged();

private:
    enum Change { Pixels = 1, Structure = 2, Selection = 4, All = 7 };
    void init();
    void finish(const QString &text, const DocState &before, int changes = All, int mergeId = -1);
    void updateSelectionBounds();
    void syncEditTarget();
    void modifySelection(const QString &text, const std::function<QImage(const QImage &)> &f);

    DocState m_state;
    QUndoStack m_undo;
    QRect m_selectionBounds;
    QString m_filePath;
    bool m_editMask = false;
    int m_untitled = 0;
    int m_layerCounter = 0;
};
