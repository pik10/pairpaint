// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

QIcon toolIcon(Tool::Id id)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(222, 222, 222);
    const QPen pen(ink, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen dashed = pen;
    dashed.setCapStyle(Qt::FlatCap);
    dashed.setDashPattern({2.0, 1.5});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (id) {
    case Tool::Move:
        p.drawLine(32, 8, 32, 56);
        p.drawLine(8, 32, 56, 32);
        p.drawPolyline(QPolygon({QPoint(24, 16), QPoint(32, 8), QPoint(40, 16)}));
        p.drawPolyline(QPolygon({QPoint(24, 48), QPoint(32, 56), QPoint(40, 48)}));
        p.drawPolyline(QPolygon({QPoint(16, 24), QPoint(8, 32), QPoint(16, 40)}));
        p.drawPolyline(QPolygon({QPoint(48, 24), QPoint(56, 32), QPoint(48, 40)}));
        break;
    case Tool::Transform:
        p.setPen(QPen(ink, 3));
        p.drawRect(12, 12, 40, 40);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        for (const QPoint &c : {QPoint(12, 12), QPoint(52, 12), QPoint(12, 52), QPoint(52, 52),
                                QPoint(32, 12), QPoint(32, 52), QPoint(12, 32), QPoint(52, 32)})
            p.drawRect(c.x() - 5, c.y() - 5, 10, 10);
        break;
    case Tool::CloneStamp: {
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(32, 14), 9, 9);
        p.drawRect(28, 20, 8, 16);
        QPainterPath base;
        base.addRoundedRect(QRectF(12, 34, 40, 12), 4, 4);
        p.drawPath(base);
        p.drawRect(10, 50, 44, 6);
        break;
    }
    case Tool::Healing:
        p.translate(32, 32);
        p.rotate(-45);
        p.drawRoundedRect(QRectF(-26, -10, 52, 20), 10, 10);
        p.fillRect(QRectF(-9, -10, 18, 20), ink);
        p.setPen(QPen(QColor(40, 40, 40), 2));
        p.drawPoint(QPointF(-4, -4));
        p.drawPoint(QPointF(4, -4));
        p.drawPoint(QPointF(-4, 4));
        p.drawPoint(QPointF(4, 4));
        break;
    case Tool::Smudge: {
        // A pointing finger.
        p.setPen(QPen(ink, 9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(20, 46, 44, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(18, 48), 10, 10);
        p.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(34, 52, 56, 52);
        p.drawLine(40, 58, 56, 58);
        break;
    }
    case Tool::Dodge:
        // A lollipop-shaped paddle.
        p.setPen(QPen(ink, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(22, 42, 8, 58);
        p.setPen(QPen(ink, 4));
        p.drawEllipse(QPointF(38, 26), 16, 16);
        break;
    case Tool::Burn:
        // A hand pinching a ring.
        p.setPen(QPen(ink, 4));
        p.drawEllipse(QPointF(32, 30), 18, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(32, 30), 7, 7);
        p.drawRoundedRect(QRectF(22, 50, 20, 10), 4, 4);
        break;
    case Tool::RectSelect:
        p.setPen(dashed);
        p.drawRect(10, 14, 44, 36);
        break;
    case Tool::EllipseSelect:
        p.setPen(dashed);
        p.drawEllipse(8, 14, 48, 36);
        break;
    case Tool::Lasso: {
        QPainterPath path(QPointF(22, 44));
        path.cubicTo(-2, 30, 18, 4, 44, 10);
        path.cubicTo(66, 16, 56, 44, 26, 44);
        p.setPen(dashed);
        p.drawPath(path);
        p.setPen(pen);
        p.drawLine(24, 44, 18, 58);
        break;
    }
    case Tool::MagicWand:
        p.setPen(QPen(ink, 6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(10, 54, 36, 28);
        p.setPen(pen);
        p.drawLine(46, 8, 46, 28);
        p.drawLine(36, 18, 56, 18);
        p.drawLine(39, 11, 53, 25);
        p.drawLine(53, 11, 39, 25);
        break;
    case Tool::Crop:
        p.drawPolyline(QPolygon({QPoint(18, 6), QPoint(18, 46), QPoint(58, 46)}));
        p.drawPolyline(QPolygon({QPoint(6, 18), QPoint(46, 18), QPoint(46, 58)}));
        break;
    case Tool::Eyedropper:
        p.setPen(QPen(ink, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(12, 52, 38, 26);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(46, 18), 10, 10);
        break;
    case Tool::Brush: {
        p.setPen(QPen(ink, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(54, 8, 32, 32);
        QPainterPath tip(QPointF(32, 28));
        tip.quadTo(12, 26, 8, 56);
        tip.quadTo(36, 54, 36, 34);
        tip.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPath(tip);
        break;
    }
    case Tool::Eraser:
        p.translate(32, 32);
        p.rotate(-45);
        p.drawRect(-22, -11, 44, 22);
        p.fillRect(QRectF(-22, -11, 18, 22), ink);
        break;
    case Tool::Fill: {
        p.translate(28, 34);
        p.rotate(-30);
        p.drawRect(-14, -14, 28, 30);
        p.drawLine(-14, -14, -4, -26);
        p.resetTransform();
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        QPainterPath drop(QPointF(52, 34));
        drop.cubicTo(58, 46, 60, 52, 52, 54);
        drop.cubicTo(44, 52, 46, 46, 52, 34);
        p.drawPath(drop);
        break;
    }
    case Tool::Gradient: {
        QLinearGradient g(8, 0, 56, 0);
        g.setColorAt(0, ink);
        g.setColorAt(1, Qt::transparent);
        p.setBrush(g);
        p.setPen(QPen(ink, 3));
        p.drawRect(8, 14, 48, 36);
        break;
    }
    case Tool::LineShape:
        p.drawLine(10, 54, 54, 10);
        break;
    case Tool::RectShape:
        p.drawRect(10, 14, 44, 36);
        break;
    case Tool::EllipseShape:
        p.drawEllipse(8, 14, 48, 36);
        break;
    case Tool::Text: {
        QFont f(QStringLiteral("Serif"));
        f.setPixelSize(52);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 0, 64, 64), Qt::AlignCenter, QStringLiteral("T"));
        break;
    }
    case Tool::Hand:
        p.drawRoundedRect(QRectF(16, 30, 32, 26), 8, 8);
        p.drawLine(22, 30, 22, 14);
        p.drawLine(30, 30, 30, 8);
        p.drawLine(38, 30, 38, 10);
        p.drawLine(46, 32, 46, 16);
        p.drawLine(16, 42, 8, 30);
        break;
    case Tool::Zoom:
        p.drawEllipse(8, 8, 34, 34);
        p.setPen(QPen(ink, 7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(38, 38, 56, 56);
        break;
    case Tool::Count:
        break;
    }
    return QIcon(pm);
}

QIcon appIcon()
{
    QPixmap pm(256, 256);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient bg(0, 0, 256, 256);
    bg.setColorAt(0, QColor(32, 102, 196));
    bg.setColorAt(1, QColor(20, 40, 92));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(8, 8, 240, 240), 48, 48);
    p.setBrush(QColor(255, 210, 80));
    p.drawEllipse(QPointF(176, 80), 28, 28);
    QPainterPath hills(QPointF(24, 208));
    hills.lineTo(96, 104);
    hills.lineTo(140, 164);
    hills.lineTo(168, 132);
    hills.lineTo(232, 208);
    hills.closeSubpath();
    p.setBrush(QColor(240, 244, 255));
    p.drawPath(hills);
    return QIcon(pm);
}
