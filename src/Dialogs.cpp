// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Dialogs.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QSpinBox *pixelSpin(int value)
{
    auto *s = new QSpinBox;
    s->setRange(1, 30000);
    s->setValue(value);
    s->setSuffix(QObject::tr(" px"));
    return s;
}

} // namespace

NewImageDialog::NewImageDialog(const QColor &fg, const QColor &bg, const QSize &initial, QWidget *parent)
    : QDialog(parent), m_fg(fg), m_bg(bg)
{
    setWindowTitle(tr("New Image"));
    m_preset = new QComboBox;
    const QList<QPair<QString, QSize>> presets = {
        {tr("Custom"), QSize()},
        {tr("800 × 600"), {800, 600}},
        {tr("HD 1280 × 720"), {1280, 720}},
        {tr("Full HD 1920 × 1080"), {1920, 1080}},
        {tr("QHD 2560 × 1440"), {2560, 1440}},
        {tr("4K UHD 3840 × 2160"), {3840, 2160}},
        {tr("Square 1080 × 1080"), {1080, 1080}},
        {tr("A4 @ 300 dpi 2480 × 3508"), {2480, 3508}},
        {tr("Letter @ 300 dpi 2550 × 3300"), {2550, 3300}},
    };
    for (const auto &[name, size] : presets)
        m_preset->addItem(name, size);

    m_width = pixelSpin(initial.width());
    m_height = pixelSpin(initial.height());
    m_background = new QComboBox;
    m_background->addItems({tr("White"), tr("Black"), tr("Transparent"), tr("Foreground color"),
                            tr("Background color")});

    connect(m_preset, &QComboBox::activated, this, [this](int i) {
        const QSize s = m_preset->itemData(i).toSize();
        if (s.isValid()) {
            QSignalBlocker a(m_width), b(m_height);
            m_width->setValue(s.width());
            m_height->setValue(s.height());
        }
    });
    auto toCustom = [this] { m_preset->setCurrentIndex(0); };
    connect(m_width, &QSpinBox::valueChanged, this, toCustom);
    connect(m_height, &QSpinBox::valueChanged, this, toCustom);

    auto *form = new QFormLayout;
    form->addRow(tr("Preset:"), m_preset);
    form->addRow(tr("Width:"), m_width);
    form->addRow(tr("Height:"), m_height);
    form->addRow(tr("Background:"), m_background);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

QSize NewImageDialog::imageSize() const { return {m_width->value(), m_height->value()}; }

QColor NewImageDialog::background() const
{
    switch (m_background->currentIndex()) {
    case 1: return Qt::black;
    case 2: return Qt::transparent;
    case 3: return m_fg;
    case 4: return m_bg;
    default: return Qt::white;
    }
}

// ---------------------------------------------------------------------------

SizeDialog::SizeDialog(const QString &title, const QSize &current, bool canvasMode, QWidget *parent)
    : QDialog(parent), m_current(current)
{
    setWindowTitle(title);
    m_width = pixelSpin(current.width());
    m_height = pixelSpin(current.height());

    auto *form = new QFormLayout;
    form->addRow(tr("Current size:"), new QLabel(tr("%1 × %2 px").arg(current.width()).arg(current.height())));
    form->addRow(tr("Width:"), m_width);
    form->addRow(tr("Height:"), m_height);

    if (!canvasMode) {
        m_keepAspect = new QCheckBox(tr("Constrain proportions"));
        m_keepAspect->setChecked(true);
        form->addRow(QString(), m_keepAspect);
        connect(m_width, &QSpinBox::valueChanged, this, [this](int w) {
            if (m_keepAspect->isChecked()) {
                QSignalBlocker b(m_height);
                m_height->setValue(std::max(1, qRound(w * double(m_current.height()) / m_current.width())));
            }
        });
        connect(m_height, &QSpinBox::valueChanged, this, [this](int h) {
            if (m_keepAspect->isChecked()) {
                QSignalBlocker b(m_width);
                m_width->setValue(std::max(1, qRound(h * double(m_current.width()) / m_current.height())));
            }
        });
    } else {
        auto *grid = new QGridLayout;
        grid->setSpacing(2);
        m_anchor = new QButtonGroup(this);
        for (int i = 0; i < 9; ++i) {
            auto *b = new QToolButton;
            b->setCheckable(true);
            b->setFixedSize(26, 26);
            m_anchor->addButton(b, i);
            grid->addWidget(b, i / 3, i % 3);
        }
        m_anchor->button(4)->setChecked(true);
        auto *holder = new QWidget;
        holder->setLayout(grid);
        form->addRow(tr("Anchor:"), holder);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

QSize SizeDialog::newSize() const { return {m_width->value(), m_height->value()}; }

QPoint SizeDialog::offset() const
{
    if (!m_anchor)
        return {};
    const int id = m_anchor->checkedId();
    const int ax = id % 3, ay = id / 3;
    const QSize n = newSize();
    return QPoint((n.width() - m_current.width()) * ax / 2, (n.height() - m_current.height()) * ay / 2);
}

// ---------------------------------------------------------------------------

TextDialog::TextDialog(const TextData &initial, QWidget *parent)
    : QDialog(parent), m_initial(initial), m_color(initial.color)
{
    setWindowTitle(initial.isValid() ? tr("Edit Text") : tr("Add Text"));
    m_edit = new QPlainTextEdit(initial.text);
    m_edit->setMinimumSize(420, 140);
    m_font = new QFontComboBox;
    m_font->setCurrentFont(initial.font);
    m_size = new QSpinBox;
    m_size->setRange(4, 2000);
    m_size->setSuffix(tr(" px"));
    m_size->setValue(initial.font.pixelSize() > 0 ? initial.font.pixelSize() : 48);
    m_bold = new QCheckBox(tr("Bold"));
    m_bold->setChecked(initial.font.bold());
    m_italic = new QCheckBox(tr("Italic"));
    m_italic->setChecked(initial.font.italic());
    m_antialias = new QCheckBox(tr("Anti-alias"));
    m_antialias->setChecked(initial.antialias);
    m_colorButton = new QPushButton;
    m_colorButton->setFixedWidth(60);
    connect(m_colorButton, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_color, this, tr("Text Color"));
        if (c.isValid()) {
            m_color = c;
            updateColorButton();
        }
    });
    updateColorButton();

    auto *row1 = new QHBoxLayout;
    row1->addWidget(m_font, 1);
    row1->addWidget(m_size);
    row1->addWidget(m_colorButton);
    auto *row2 = new QHBoxLayout;
    row2->addWidget(m_bold);
    row2->addWidget(m_italic);
    row2->addWidget(m_antialias);
    row2->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(row1);
    layout->addLayout(row2);
    layout->addWidget(m_edit, 1);
    layout->addWidget(buttons);
    m_edit->setFocus();
}

void TextDialog::updateColorButton()
{
    m_colorButton->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #888;").arg(m_color.name()));
    m_colorButton->setToolTip(m_color.name());
}

TextData TextDialog::data() const
{
    TextData t = m_initial;
    t.text = m_edit->toPlainText();
    QFont f = m_font->currentFont();
    f.setPixelSize(m_size->value());
    f.setBold(m_bold->isChecked());
    f.setItalic(m_italic->isChecked());
    t.font = f;
    t.color = m_color;
    t.antialias = m_antialias->isChecked();
    return t;
}

void TextDialog::editLayer(Document *doc, int index, QWidget *parent)
{
    if (!doc->layer(index).isText())
        return;
    TextDialog dlg(doc->layer(index).text, parent);
    if (dlg.exec() == QDialog::Accepted && dlg.data().isValid())
        doc->setText(index, dlg.data());
}

// ---------------------------------------------------------------------------

namespace {

// A button showing a color; clicking it opens a color picker.
QPushButton *colorButton(QColor *color, QWidget *parent, const std::function<void()> &changed)
{
    auto *b = new QPushButton(parent);
    b->setFixedWidth(48);
    auto refresh = [b, color] {
        b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #888;").arg(color->name()));
    };
    refresh();
    QObject::connect(b, &QPushButton::clicked, b, [b, color, refresh, changed] {
        const QColor c = QColorDialog::getColor(*color, b->window());
        if (c.isValid()) {
            *color = c;
            refresh();
            changed();
        }
    });
    return b;
}

} // namespace

LayerStyleDialog::LayerStyleDialog(Document *doc, int layer, QWidget *parent)
    : QDialog(parent), m_doc(doc), m_layer(layer), m_before(doc->state()), m_original(doc->layer(layer).style)
{
    setWindowTitle(tr("Layer Style — %1").arg(doc->layer(layer).name));
    const LayerStyle &s = m_original;
    m_shadowColor = s.shadowColor;
    m_glowColor = s.glowColor;
    m_strokeColor = s.strokeColor;
    auto update = [this] { preview(); };

    auto section = [&](const QString &title, bool on) {
        auto *box = new QGroupBox(title);
        box->setCheckable(true);
        box->setChecked(on);
        connect(box, &QGroupBox::toggled, this, update);
        return box;
    };
    auto spinBox = [&](int min, int max, int value, const QString &suffix) {
        auto *sp = new QSpinBox;
        sp->setRange(min, max);
        sp->setValue(value);
        sp->setSuffix(suffix);
        connect(sp, &QSpinBox::valueChanged, this, update);
        return sp;
    };

    m_shadow = section(tr("Drop Shadow"), s.shadow);
    m_shadowOpacity = spinBox(0, 100, s.shadowOpacity, QStringLiteral("%"));
    m_shadowAngle = spinBox(-180, 360, s.shadowAngle, QStringLiteral("°"));
    m_shadowDistance = spinBox(0, 500, s.shadowDistance, tr(" px"));
    m_shadowSize = spinBox(0, 250, s.shadowSize, tr(" px"));
    auto *sf = new QFormLayout(m_shadow);
    sf->addRow(tr("Color:"), colorButton(&m_shadowColor, this, update));
    sf->addRow(tr("Opacity:"), m_shadowOpacity);
    sf->addRow(tr("Angle:"), m_shadowAngle);
    sf->addRow(tr("Distance:"), m_shadowDistance);
    sf->addRow(tr("Size:"), m_shadowSize);

    m_glow = section(tr("Outer Glow"), s.glow);
    m_glowOpacity = spinBox(0, 100, s.glowOpacity, QStringLiteral("%"));
    m_glowSize = spinBox(1, 250, s.glowSize, tr(" px"));
    auto *gf = new QFormLayout(m_glow);
    gf->addRow(tr("Color:"), colorButton(&m_glowColor, this, update));
    gf->addRow(tr("Opacity:"), m_glowOpacity);
    gf->addRow(tr("Size:"), m_glowSize);

    m_stroke = section(tr("Stroke (outside)"), s.stroke);
    m_strokeOpacity = spinBox(0, 100, s.strokeOpacity, QStringLiteral("%"));
    m_strokeSize = spinBox(1, 250, s.strokeSize, tr(" px"));
    auto *kf = new QFormLayout(m_stroke);
    kf->addRow(tr("Color:"), colorButton(&m_strokeColor, this, update));
    kf->addRow(tr("Opacity:"), m_strokeOpacity);
    kf->addRow(tr("Size:"), m_strokeSize);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_shadow);
    layout->addWidget(m_glow);
    layout->addWidget(m_stroke);
    layout->addWidget(buttons);
}

LayerStyle LayerStyleDialog::current() const
{
    LayerStyle s;
    s.shadow = m_shadow->isChecked();
    s.shadowColor = m_shadowColor;
    s.shadowOpacity = m_shadowOpacity->value();
    s.shadowAngle = m_shadowAngle->value();
    s.shadowDistance = m_shadowDistance->value();
    s.shadowSize = m_shadowSize->value();
    s.glow = m_glow->isChecked();
    s.glowColor = m_glowColor;
    s.glowOpacity = m_glowOpacity->value();
    s.glowSize = m_glowSize->value();
    s.stroke = m_stroke->isChecked();
    s.strokeColor = m_strokeColor;
    s.strokeOpacity = m_strokeOpacity->value();
    s.strokeSize = m_strokeSize->value();
    return s;
}

void LayerStyleDialog::preview()
{
    m_doc->layer(m_layer).style = current();
    m_doc->notifyImageChanged();
}

void LayerStyleDialog::done(int result)
{
    if (result == QDialog::Accepted && !(current() == m_original)) {
        m_doc->layer(m_layer).style = current();
        m_doc->commit(current().any() ? tr("Layer Style") : tr("Clear Layer Style"), m_before);
        m_doc->notifyImageChanged();
        m_doc->notifyStructureChanged();
    } else {
        m_doc->layer(m_layer).style = m_original;
        m_doc->notifyImageChanged();
    }
    QDialog::done(result);
}
