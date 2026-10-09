// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "ColorWidgets.h"

#include "ToolSettings.h"

#include <QColorDialog>
#include <QMouseEvent>
#include <QPainter>

namespace {
constexpr int kCell = 16;
constexpr int kGap = 2;
}

ColorSwatch::ColorSwatch(ToolSettings *settings, QWidget *parent) : QWidget(parent), m_settings(settings)
{
    setFixedSize(sizeHint());
    setToolTip(tr("Foreground / background color\nClick to choose · X swaps · D resets"));
    connect(settings, &ToolSettings::colorsChanged, this, qOverload<>(&QWidget::update));
}

void ColorSwatch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    auto square = [&](const QRect &r, const QColor &c) {
        p.fillRect(r, c);
        p.setPen(QColor(20, 20, 20));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(QColor(230, 230, 230));
        p.drawRect(r.adjusted(1, 1, -2, -2));
    };
    square(bgRect(), m_settings->background());
    square(fgRect(), m_settings->foreground());

    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(210, 210, 210), 1.5));
    const QRect s = swapRect();
    p.drawArc(s.adjusted(2, 2, -2, 10), 0 * 16, 90 * 16);
    p.drawLine(s.right() - 4, s.top() + 9, s.right() - 2, s.top() + 12);
    p.drawLine(s.right() - 2, s.top() + 12, s.right(), s.top() + 9);

    const QRect r = resetRect();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRect(r.left() + 6, r.top() + 6, 9, 9), Qt::white);
    p.setPen(QColor(20, 20, 20));
    p.drawRect(QRect(r.left() + 6, r.top() + 6, 8, 8));
    p.fillRect(QRect(r.left(), r.top(), 9, 9), Qt::black);
    p.setPen(QColor(200, 200, 200));
    p.drawRect(QRect(r.left(), r.top(), 8, 8));
}

void ColorSwatch::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (swapRect().contains(pos)) {
        m_settings->swapColors();
    } else if (resetRect().contains(pos)) {
        m_settings->resetColors();
    } else if (fgRect().contains(pos)) {
        const QColor c = QColorDialog::getColor(m_settings->foreground(), this, tr("Foreground Color"));
        if (c.isValid())
            m_settings->setForeground(c);
    } else if (bgRect().contains(pos)) {
        const QColor c = QColorDialog::getColor(m_settings->background(), this, tr("Background Color"));
        if (c.isValid())
            m_settings->setBackground(c);
    }
}

// ---------------------------------------------------------------------------

SwatchPalette::SwatchPalette(ToolSettings *settings, QWidget *parent) : QWidget(parent), m_settings(settings)
{
    for (int i = 0; i < 12; ++i) {
        const int v = qRound(i * 255.0 / 11);
        m_colors << QColor(v, v, v);
    }
    const QList<QPair<int, int>> sv = {{255, 255}, {170, 255}, {255, 170}, {255, 100}};
    for (const auto &[s, v] : sv)
        for (int h = 0; h < 12; ++h)
            m_colors << QColor::fromHsv(h * 30, s, v);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setToolTip(tr("Left click: foreground · Right click: background"));
}

int SwatchPalette::columns() const { return std::max(1, (width() + kGap) / (kCell + kGap)); }

QRect SwatchPalette::cellRect(int i) const
{
    const int c = columns();
    return QRect((i % c) * (kCell + kGap), (i / c) * (kCell + kGap), kCell, kCell);
}

QSize SwatchPalette::sizeHint() const { return {12 * (kCell + kGap), 5 * (kCell + kGap)}; }

int SwatchPalette::heightForWidth(int w) const
{
    const int c = std::max(1, (w + kGap) / (kCell + kGap));
    const int rows = (int(m_colors.size()) + c - 1) / c;
    return rows * (kCell + kGap);
}

void SwatchPalette::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    for (int i = 0; i < m_colors.size(); ++i) {
        const QRect r = cellRect(i);
        p.fillRect(r, m_colors[i]);
        p.setPen(QColor(20, 20, 20));
        p.drawRect(r.adjusted(0, 0, -1, -1));
    }
}

void SwatchPalette::mousePressEvent(QMouseEvent *e)
{
    for (int i = 0; i < m_colors.size(); ++i) {
        if (cellRect(i).contains(e->position().toPoint())) {
            if (e->button() == Qt::RightButton)
                m_settings->setBackground(m_colors[i]);
            else
                m_settings->setForeground(m_colors[i]);
            return;
        }
    }
}
