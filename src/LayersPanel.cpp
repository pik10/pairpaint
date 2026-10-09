// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "LayersPanel.h"

#include "Document.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QMenu>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kThumb = 40;
constexpr int kGap = 4;

void drawThumb(QPainter &p, const QRect &cell, const QImage &img)
{
    const QSizeF s = QSizeF(img.size()).scaled(cell.size(), Qt::KeepAspectRatio);
    const QRectF r(cell.left() + (cell.width() - s.width()) / 2, cell.top() + (cell.height() - s.height()) / 2,
                   s.width(), s.height());
    p.save();
    p.setClipRect(r);
    for (int y = cell.top(); y < cell.bottom(); y += 5)
        for (int x = cell.left(); x < cell.right(); x += 5)
            p.fillRect(QRect(x, y, 5, 5), ((x + y) / 5) % 2 ? QColor(200, 200, 200) : Qt::white);
    p.restore();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, img);
    p.setPen(QColor(0, 0, 0, 120));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));
}

void drawBadge(QPainter &p, const QRect &cell, const QString &text)
{
    p.fillRect(cell, QColor(70, 70, 70));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(text.size() > 1 ? 12 : 24);
    p.setFont(f);
    p.setPen(QColor(230, 230, 230));
    p.drawText(cell, Qt::AlignCenter, text);
}

void drawFolder(QPainter &p, const QRect &cell)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(214, 170, 70));
    const QRectF body = QRectF(cell).adjusted(5, 12, -5, -8);
    p.drawRoundedRect(QRectF(body.left(), body.top() - 5, body.width() * 0.45, 8), 2, 2);  // tab
    p.drawRoundedRect(body, 3, 3);
    p.restore();
}

void drawTargetFrame(QPainter &p, const QRect &cell)
{
    p.setPen(QPen(QColor(255, 255, 255), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(cell).adjusted(1, 1, -1, -1));
}

} // namespace

LayersPanel::LayersPanel(QWidget *parent) : QWidget(parent)
{
    m_mode = new QComboBox;
    for (const auto &[name, mode] : blendModes())
        m_mode->addItem(name, int(mode));
    m_mode->setToolTip(tr("Blend mode"));

    m_opacity = new QSlider(Qt::Horizontal);
    m_opacity->setRange(0, 100);
    m_opacityLabel = new QLabel(QStringLiteral("100%"));
    m_opacityLabel->setMinimumWidth(36);

    m_list = new QTreeWidget;
    m_list->setHeaderHidden(true);
    m_list->setIndentation(14);
    m_list->setExpandsOnDoubleClick(false);  // double-click renames; the arrow opens and closes groups
    m_list->setIconSize(QSize(2 * kThumb + kGap, kThumb));
    m_list->setEditTriggers(QAbstractItemView::EditKeyPressed);

    auto *opacityRow = new QHBoxLayout;
    opacityRow->addWidget(new QLabel(tr("Opacity")));
    opacityRow->addWidget(m_opacity, 1);
    opacityRow->addWidget(m_opacityLabel);

    // Which part of the layer painting tools and filters affect.
    m_editLayer = new QToolButton;
    m_editLayer->setText(tr("Edit Layer"));
    m_editMask = new QToolButton;
    m_editMask->setText(tr("Edit Mask"));
    auto *targetGroup = new QButtonGroup(this);
    for (QToolButton *b : {m_editLayer, m_editMask}) {
        b->setCheckable(true);
        b->setAutoRaise(true);
        targetGroup->addButton(b);
    }
    m_editLayer->setToolTip(tr("Paint and filter the layer's pixels"));
    m_editMask->setToolTip(tr("Paint and filter the layer mask (white reveals, black hides)"));
    connect(m_editLayer, &QToolButton::clicked, this, [this] { if (m_doc) m_doc->setEditingMask(false); });
    connect(m_editMask, &QToolButton::clicked, this, [this] { if (m_doc) m_doc->setEditingMask(true); });
    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(m_editLayer);
    targetRow->addWidget(m_editMask);
    targetRow->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    auto addButton = [&](const QString &text, const QString &tip, std::function<void(Document *)> fn) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, [this, fn] {
            if (m_doc)
                fn(m_doc);
        });
        buttons->addWidget(b);
        m_docWidgets << b;
        return b;
    };
    addButton(QStringLiteral("+"), tr("New layer"), [](Document *d) { d->addLayer(); });
    addButton(QStringLiteral("⧉"), tr("Duplicate layer"), [](Document *d) { d->duplicateLayer(); });
    addButton(QStringLiteral("❐"), tr("New group (Ctrl+G groups the selected layer)"), [](Document *d) { d->newGroup(); });
    addButton(QStringLiteral("◐"), tr("Add layer mask (from the selection if there is one)"),
              [](Document *d) { d->addMask(true); });

    auto *adjButton = new QToolButton;
    adjButton->setText(QStringLiteral("◑"));
    adjButton->setToolTip(tr("New adjustment layer"));
    adjButton->setAutoRaise(true);
    adjButton->setPopupMode(QToolButton::InstantPopup);
    auto *adjMenu = new QMenu(adjButton);
    for (int t = Adjustment::BrightnessContrast; t < Adjustment::TypeCount; ++t) {
        const auto type = Adjustment::Type(t);
        connect(adjMenu->addAction(Adjustments::name(type) + QStringLiteral("…")), &QAction::triggered, this,
                [this, type] { emit newAdjustmentRequested(type); });
    }
    adjButton->setMenu(adjMenu);
    buttons->addWidget(adjButton);
    m_docWidgets << adjButton;

    auto *fx = new QToolButton;
    fx->setText(QStringLiteral("fx"));
    fx->setToolTip(tr("Layer style (drop shadow, glow, stroke)"));
    fx->setAutoRaise(true);
    connect(fx, &QToolButton::clicked, this, &LayersPanel::layerStyleRequested);
    buttons->addWidget(fx);
    m_docWidgets << fx;

    addButton(QStringLiteral("▲"), tr("Raise layer"), [](Document *d) { d->moveLayer(1); });
    addButton(QStringLiteral("▼"), tr("Lower layer"), [](Document *d) { d->moveLayer(-1); });
    addButton(QStringLiteral("⤓"), tr("Merge down"), [](Document *d) { d->mergeDown(); });
    buttons->addStretch();
    addButton(QStringLiteral("✕"), tr("Delete layer"), [](Document *d) { d->deleteLayer(); });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_mode);
    layout->addLayout(opacityRow);
    layout->addWidget(m_list, 1);
    layout->addLayout(targetRow);
    layout->addLayout(buttons);
    m_docWidgets << m_mode << m_opacity << m_list;

    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!m_updating && m_doc && item)
            m_doc->setActiveIndex(item->data(0, Qt::UserRole).toInt());
    });
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_updating || !m_doc)
            return;
        const int i = item->data(0, Qt::UserRole).toInt();
        if (i < 0 || i >= m_doc->layerCount())
            return;
        const bool visible = item->checkState(0) == Qt::Checked;
        if (visible != m_doc->layer(i).visible)
            m_doc->setLayerVisible(i, visible);
        if (item->text(0) != m_doc->layer(i).name)
            m_doc->renameLayer(i, item->text(0));
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &LayersPanel::onDoubleClicked);
    // Remember open/closed groups (a view setting, so not part of undo).
    auto setCollapsed = [this](QTreeWidgetItem *item, bool collapsed) {
        const int i = item->data(0, Qt::UserRole).toInt();
        if (!m_updating && m_doc && i >= 0 && i < m_doc->layerCount())
            m_doc->layer(i).collapsed = collapsed;
    };
    connect(m_list, &QTreeWidget::itemExpanded, this, [setCollapsed](QTreeWidgetItem *it) { setCollapsed(it, false); });
    connect(m_list, &QTreeWidget::itemCollapsed, this, [setCollapsed](QTreeWidgetItem *it) { setCollapsed(it, true); });
    connect(m_mode, &QComboBox::activated, this, [this](int idx) {
        if (m_doc)
            m_doc->setLayerMode(m_doc->activeIndex(), QPainter::CompositionMode(m_mode->itemData(idx).toInt()));
    });
    connect(m_opacity, &QSlider::valueChanged, this, [this](int v) {
        m_opacityLabel->setText(QStringLiteral("%1%").arg(v));
        if (!m_updating && m_doc)
            m_doc->setLayerOpacity(m_doc->activeIndex(), v / 100.0);
    });

    // Rebuild asynchronously: document changes can be triggered from inside
    // this list's own signal handlers.
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &LayersPanel::rebuild);
    m_thumbTimer.setSingleShot(true);
    m_thumbTimer.setInterval(250);
    connect(&m_thumbTimer, &QTimer::timeout, this, &LayersPanel::refreshThumbnails);

    rebuild();
}

void LayersPanel::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    if (doc) {
        connect(doc, &Document::structureChanged, this, &LayersPanel::scheduleRebuild);
        connect(doc, &Document::imageChanged, this, [this] { m_thumbTimer.start(); });
    }
    rebuild();
}

void LayersPanel::scheduleRebuild() { m_rebuildTimer.start(); }

QIcon LayersPanel::thumbnailFor(int i) const
{
    const Layer &l = m_doc->layer(i);
    QPixmap pm(2 * kThumb + kGap, kThumb);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QRect left(0, 0, kThumb, kThumb), right(kThumb + kGap, 0, kThumb, kThumb);
    if (l.isGroup())
        drawFolder(p, left);
    else if (l.isAdjustment())
        drawBadge(p, left, Adjustments::shortName(l.adjustment.type));
    else
        drawThumb(p, left, l.image);
    if (l.isText()) {
        p.fillRect(QRect(left.right() - 13, left.bottom() - 13, 14, 14), QColor(42, 130, 218));
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(11);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRect(left.right() - 13, left.bottom() - 13, 14, 14), Qt::AlignCenter, QStringLiteral("T"));
    }
    if (l.style.any()) {
        const QRect badge(left.left(), left.bottom() - 12, 16, 13);
        p.fillRect(badge, QColor(200, 120, 30));
        QFont f = p.font();
        f.setBold(true);
        f.setItalic(true);
        f.setPixelSize(10);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(badge, Qt::AlignCenter, QStringLiteral("fx"));
    }
    if (!l.mask.isNull()) {
        drawThumb(p, right, l.mask);
        if (!l.maskEnabled) {
            p.setPen(QPen(QColor(220, 40, 40), 2));
            p.drawLine(right.topLeft(), right.bottomRight());
            p.drawLine(right.topRight(), right.bottomLeft());
        }
    }
    if (i == m_doc->activeIndex() && !l.mask.isNull())
        drawTargetFrame(p, m_doc->editingMask() ? right : left);
    return QIcon(pm);
}

void LayersPanel::rebuild()
{
    m_updating = true;
    m_list->clear();
    for (QWidget *w : m_docWidgets)
        w->setEnabled(m_doc);
    if (m_doc) {
        QList<QTreeWidgetItem *> parents;  // open groups while walking from the top layer down
        QTreeWidgetItem *current = nullptr;
        QList<QPair<QTreeWidgetItem *, bool>> groups;
        for (int i = m_doc->layerCount() - 1; i >= 0; --i) {
            const Layer &l = m_doc->layer(i);
            if (l.isGroupEnd()) {
                if (!parents.isEmpty())
                    parents.removeLast();
                continue;
            }
            auto *item = new QTreeWidgetItem;
            item->setIcon(0, thumbnailFor(i));
            item->setText(0, l.name);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
            item->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
            item->setData(0, Qt::UserRole, i);
            item->setToolTip(0, l.isAdjustment() ? tr("Double-click to edit the adjustment · F2 renames")
                                : l.isText()     ? tr("Double-click to edit the text · F2 renames")
                                                 : tr("Double-click to rename · checkbox toggles visibility"));
            if (parents.isEmpty())
                m_list->addTopLevelItem(item);
            else
                parents.last()->addChild(item);
            if (l.isGroup()) {
                parents.append(item);
                groups.append({item, l.collapsed});
            }
            if (i == m_doc->activeIndex())
                current = item;
        }
        for (const auto &[item, collapsed] : groups)
            item->setExpanded(!collapsed);
        if (current) {
            m_list->setCurrentItem(current);
            m_list->scrollToItem(current);
        }
        const Layer &a = m_doc->activeLayer();
        m_mode->setCurrentIndex(std::max(0, m_mode->findData(int(a.mode))));
        m_mode->setEnabled(!a.isAdjustment());
        m_opacity->setValue(qRound(a.opacity * 100));
    }
    updateTargetButtons();
    m_updating = false;
}

void LayersPanel::updateTargetButtons()
{
    const bool hasMask = m_doc && !m_doc->activeLayer().mask.isNull();
    const bool adjustment = m_doc && (m_doc->activeLayer().isAdjustment() || m_doc->activeLayer().isGroup());
    m_editLayer->setEnabled(hasMask && !adjustment);
    m_editMask->setEnabled(hasMask);
    const bool mask = m_doc && m_doc->editingMask();
    m_editMask->setChecked(mask);
    m_editLayer->setChecked(!mask);
}

void LayersPanel::refreshThumbnails()
{
    if (!m_doc)
        return;
    m_updating = true;
    for (QTreeWidgetItemIterator it(m_list); *it; ++it) {
        const int i = (*it)->data(0, Qt::UserRole).toInt();
        if (i >= 0 && i < m_doc->layerCount())
            (*it)->setIcon(0, thumbnailFor(i));
    }
    m_updating = false;
}

void LayersPanel::onDoubleClicked(QTreeWidgetItem *item)
{
    if (!m_doc)
        return;
    const int i = item->data(0, Qt::UserRole).toInt();
    if (i < 0 || i >= m_doc->layerCount())
        return;
    if (m_doc->layer(i).isAdjustment())
        emit editAdjustmentRequested(i);
    else if (m_doc->layer(i).isText())
        emit editTextRequested(i);
    else
        m_list->editItem(item, 0);
}
