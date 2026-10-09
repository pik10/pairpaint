// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Adjustments.h"
#include "Document.h"

#include <QDialog>
#include <QTimer>
#include <functional>

class QCheckBox;
class QSpinBox;

// Edits a list of integer parameters.
class ParamEditor : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual QList<int> values() const = 0;

signals:
    void changed();
};

class SliderEditor : public ParamEditor {
    Q_OBJECT
public:
    explicit SliderEditor(const QList<FilterParam> &params, const QList<int> &initial = {}, QWidget *parent = nullptr);
    QList<int> values() const override;

private:
    QList<QSpinBox *> m_spins;
};

// Interactive tone curve over a luminance histogram. Click to add a point,
// drag to move, right-click or drag off the graph to remove.
class CurvesEditor : public ParamEditor {
    Q_OBJECT
public:
    CurvesEditor(const QList<int> &points, const QList<int> &histogram, QWidget *parent = nullptr);
    QList<int> values() const override;
    QSize sizeHint() const override { return {300, 300}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    QRectF graphRect() const;
    QPointF toWidget(const QPoint &v) const;
    QPoint toValue(const QPointF &w) const;
    int pointAt(const QPointF &w) const;

    QList<QPoint> m_points;  // sorted by x
    QList<int> m_histogram;
    int m_drag = -1;
};

// What the dialog previews into and how it finishes.
struct PreviewTarget {
    std::function<void(const QList<int> &)> preview;
    std::function<void()> commit;
    std::function<void()> cancel;  // also used to switch the preview off
    bool canTogglePreview = true;
};

// Destructive filter on the document's edit target, limited to the selection.
PreviewTarget filterTarget(Document *doc, const QString &title,
                           const std::function<QImage(const QImage &, const QList<int> &)> &func);
// Edits the parameters of adjustment layer `index`. If `creating`, the layer was
// just added without an undo step and cancelling removes it again.
PreviewTarget adjustmentLayerTarget(Document *doc, int index, const QString &title, const DocState *beforeCreation);

class FilterDialog : public QDialog {
    Q_OBJECT
public:
    FilterDialog(const QString &title, ParamEditor *editor, PreviewTarget target, QWidget *parent = nullptr);
    void done(int result) override;

private:
    void schedulePreview();
    void runPreview();

    ParamEditor *m_editor;
    PreviewTarget m_target;
    QCheckBox *m_previewBox = nullptr;
    QTimer m_timer;
    bool m_previewCurrent = false;
};
