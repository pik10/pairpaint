// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "FilterDialog.h"

#include "Filters.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <memory>

// ---------------------------------------------------------------------------

SliderEditor::SliderEditor(const QList<FilterParam> &params, const QList<int> &initial, QWidget *parent)
    : ParamEditor(parent)
{
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < params.size(); ++i) {
        const FilterParam &fp = params[i];
        const int value = i < initial.size() ? initial[i] : fp.value;
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(fp.min, fp.max);
        slider->setValue(value);
        slider->setMinimumWidth(240);
        auto *spin = new QSpinBox;
        spin->setRange(fp.min, fp.max);
        spin->setValue(value);
        spin->setSuffix(fp.suffix);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        connect(spin, &QSpinBox::valueChanged, this, &ParamEditor::changed);
        grid->addWidget(new QLabel(fp.label + QLatin1Char(':')), i, 0);
        grid->addWidget(slider, i, 1);
        grid->addWidget(spin, i, 2);
        m_spins << spin;
    }
}

QList<int> SliderEditor::values() const
{
    QList<int> v;
    for (const QSpinBox *s : m_spins)
        v << s->value();
    return v;
}

// ---------------------------------------------------------------------------

CurvesEditor::CurvesEditor(const QList<int> &points, const QList<int> &histogram, QWidget *parent)
    : ParamEditor(parent), m_histogram(histogram)
{
    for (int i = 0; i + 1 < points.size(); i += 2)
        m_points << QPoint(points[i], points[i + 1]);
    if (m_points.size() < 2)
        m_points = {QPoint(0, 0), QPoint(255, 255)};
    std::sort(m_points.begin(), m_points.end(), [](const QPoint &a, const QPoint &b) { return a.x() < b.x(); });
    setMinimumSize(280, 280);
    setMouseTracking(true);
}

QList<int> CurvesEditor::values() const
{
    QList<int> v;
    for (const QPoint &p : m_points)
        v << p.x() << p.y();
    return v;
}

QRectF CurvesEditor::graphRect() const
{
    const qreal s = std::min(width(), height()) - 16;
    return QRectF((width() - s) / 2, (height() - s) / 2, s, s);
}

QPointF CurvesEditor::toWidget(const QPoint &v) const
{
    const QRectF g = graphRect();
    return QPointF(g.left() + v.x() / 255.0 * g.width(), g.bottom() - v.y() / 255.0 * g.height());
}

QPoint CurvesEditor::toValue(const QPointF &w) const
{
    const QRectF g = graphRect();
    return QPoint(std::clamp(qRound((w.x() - g.left()) / g.width() * 255), 0, 255),
                  std::clamp(qRound((g.bottom() - w.y()) / g.height() * 255), 0, 255));
}

int CurvesEditor::pointAt(const QPointF &w) const
{
    for (int i = 0; i < m_points.size(); ++i)
        if (QLineF(toWidget(m_points[i]), w).length() <= 7)
            return i;
    return -1;
}

void CurvesEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRectF g = graphRect();
    p.fillRect(g, QColor(28, 28, 28));

    if (!m_histogram.isEmpty()) {
        const int peak = std::max(1, *std::max_element(m_histogram.begin(), m_histogram.end()));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(90, 90, 90));
        for (int i = 0; i < 256; ++i) {
            const qreal h = std::sqrt(m_histogram[i] / double(peak)) * g.height();
            p.drawRect(QRectF(g.left() + i * g.width() / 256, g.bottom() - h, g.width() / 256 + 0.5, h));
        }
    }
    p.setPen(QColor(70, 70, 70));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(g.left() + g.width() * i / 4, g.top()), QPointF(g.left() + g.width() * i / 4, g.bottom()));
        p.drawLine(QPointF(g.left(), g.top() + g.height() * i / 4), QPointF(g.right(), g.top() + g.height() * i / 4));
    }
    p.drawLine(g.bottomLeft(), g.topRight());
    p.setBrush(Qt::NoBrush);
    p.drawRect(g);

    p.setRenderHint(QPainter::Antialiasing);
    const QList<int> lut = Filters::curveLut(values());
    QPainterPath curve;
    for (int x = 0; x < 256; ++x) {
        const QPointF pt = toWidget(QPoint(x, lut[x]));
        if (x == 0) curve.moveTo(pt); else curve.lineTo(pt);
    }
    p.setPen(QPen(QColor(230, 230, 230), 2));
    p.drawPath(curve);
    p.setPen(QPen(Qt::black, 1));
    for (int i = 0; i < m_points.size(); ++i) {
        p.setBrush(i == m_drag ? QColor(42, 130, 218) : Qt::white);
        const QPointF c = toWidget(m_points[i]);
        p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
    }
    if (m_drag >= 0) {
        p.setPen(Qt::white);
        p.drawText(g.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft,
                   tr("Input %1  Output %2").arg(m_points[m_drag].x()).arg(m_points[m_drag].y()));
    }
}

void CurvesEditor::mousePressEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    int i = pointAt(pos);
    if (e->button() == Qt::RightButton) {
        if (i > 0 && i < m_points.size() - 1) {
            m_points.removeAt(i);
            emit changed();
            update();
        }
        return;
    }
    if (i < 0) {
        const QPoint v = toValue(pos);
        auto it = std::lower_bound(m_points.begin(), m_points.end(), v,
                                   [](const QPoint &a, const QPoint &b) { return a.x() < b.x(); });
        i = int(it - m_points.begin());
        if (it != m_points.end() && it->x() == v.x()) {
            m_points[i] = v;  // a point already exists at this input level: grab it
        } else {
            m_points.insert(i, v);
        }
        emit changed();
    }
    m_drag = i;
    update();
}

void CurvesEditor::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drag < 0)
        return;
    const QRectF g = graphRect();
    const QPointF pos = e->position();
    const bool interior = m_drag > 0 && m_drag < m_points.size() - 1;
    if (interior && (pos.y() < g.top() - 30 || pos.y() > g.bottom() + 30)) {
        m_points.removeAt(m_drag);  // dragged off the graph
        m_drag = -1;
        emit changed();
        update();
        return;
    }
    QPoint v = toValue(pos);
    const int lo = m_drag > 0 ? m_points[m_drag - 1].x() + 1 : 0;
    const int hi = m_drag < m_points.size() - 1 ? m_points[m_drag + 1].x() - 1 : 255;
    v.setX(std::clamp(v.x(), lo, hi));
    if (v != m_points[m_drag]) {
        m_points[m_drag] = v;
        emit changed();
        update();
    }
}

void CurvesEditor::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = -1;
    update();
}

// ---------------------------------------------------------------------------

PreviewTarget filterTarget(Document *doc, const QString &title,
                           const std::function<QImage(const QImage &, const QList<int> &)> &func)
{
    struct State {
        DocState before;
        QImage original;
    };
    doc->prepareForPixelEdit();
    auto st = std::make_shared<State>();
    st->before = doc->state();
    st->original = doc->targetImage();

    PreviewTarget t;
    t.preview = [doc, st, func](const QList<int> &v) {
        doc->targetImage() = doc->maskedBlend(st->original, func(st->original, v));
        doc->notifyImageChanged();
    };
    t.commit = [doc, st, title] {
        doc->commit(title, st->before);
        doc->notifyStructureChanged();  // refresh thumbnails
    };
    t.cancel = [doc, st] {
        doc->targetImage() = st->original;
        doc->notifyImageChanged();
    };
    return t;
}

PreviewTarget adjustmentLayerTarget(Document *doc, int index, const QString &title, const DocState *beforeCreation)
{
    struct State {
        DocState before;
        QList<int> original;
        bool creating;
    };
    auto st = std::make_shared<State>();
    st->before = beforeCreation ? *beforeCreation : doc->state();
    st->original = doc->layer(index).adjustment.params;
    st->creating = beforeCreation != nullptr;

    PreviewTarget t;
    t.canTogglePreview = false;
    t.preview = [doc, index, st](const QList<int> &v) {
        // The dialog edits the main settings; per-channel ones (e.g. from a PSD) are kept.
        Adjustment &adj = doc->layer(index).adjustment;
        adj.params = Adjustments::withMainParams(adj.type, st->original, v);
        doc->notifyImageChanged();
    };
    t.commit = [doc, st, title] {
        doc->commit(title, st->before);
        doc->notifyStructureChanged();
    };
    t.cancel = [doc, st, index] {
        if (st->creating) {
            doc->setState(st->before);
        } else {
            doc->layer(index).adjustment.params = st->original;
            doc->notifyImageChanged();
        }
    };
    return t;
}

// ---------------------------------------------------------------------------

FilterDialog::FilterDialog(const QString &title, ParamEditor *editor, PreviewTarget target, QWidget *parent)
    : QDialog(parent), m_editor(editor), m_target(std::move(target))
{
    setWindowTitle(title);
    connect(editor, &ParamEditor::changed, this, &FilterDialog::schedulePreview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(editor, 1);
    if (m_target.canTogglePreview) {
        m_previewBox = new QCheckBox(tr("Preview"));
        m_previewBox->setChecked(true);
        connect(m_previewBox, &QCheckBox::toggled, this, [this](bool on) {
            if (on) {
                schedulePreview();
            } else {
                m_timer.stop();
                m_previewCurrent = false;
                m_target.cancel();
            }
        });
        layout->addWidget(m_previewBox);
    }
    layout->addWidget(buttons);

    m_timer.setSingleShot(true);
    m_timer.setInterval(60);
    connect(&m_timer, &QTimer::timeout, this, &FilterDialog::runPreview);
    schedulePreview();
}

void FilterDialog::schedulePreview()
{
    m_previewCurrent = false;
    if (!m_previewBox || m_previewBox->isChecked())
        m_timer.start();
}

void FilterDialog::runPreview()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_target.preview(m_editor->values());
    QApplication::restoreOverrideCursor();
    m_previewCurrent = true;
}

void FilterDialog::done(int result)
{
    m_timer.stop();
    if (result == QDialog::Accepted) {
        if (!m_previewCurrent)
            runPreview();
        m_target.commit();
    } else {
        m_target.cancel();
    }
    QDialog::done(result);
}
