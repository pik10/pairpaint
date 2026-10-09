// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Adjustments.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

class Document;
class QComboBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QSlider;
class QToolButton;

class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(QWidget *parent = nullptr);
    void setDocument(Document *doc);

signals:
    void newAdjustmentRequested(Adjustment::Type type);
    void editAdjustmentRequested(int layer);
    void editTextRequested(int layer);
    void layerStyleRequested();

private:
    void scheduleRebuild();
    void rebuild();
    void refreshThumbnails();
    void updateTargetButtons();
    QIcon thumbnailFor(int layer) const;
    void onDoubleClicked(QTreeWidgetItem *item);

    QPointer<Document> m_doc;
    QTreeWidget *m_list;  // top of the tree = top layer; groups are expandable
    QComboBox *m_mode;
    QSlider *m_opacity;
    QLabel *m_opacityLabel;
    QToolButton *m_editLayer;
    QToolButton *m_editMask;
    QList<QWidget *> m_docWidgets;
    QTimer m_rebuildTimer;
    QTimer m_thumbTimer;
    bool m_updating = false;
};
