// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Document.h"

#include <QColor>
#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFontComboBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class NewImageDialog : public QDialog {
    Q_OBJECT
public:
    NewImageDialog(const QColor &fg, const QColor &bg, const QSize &initial, QWidget *parent = nullptr);
    QSize imageSize() const;
    QColor background() const;

private:
    QComboBox *m_preset;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QComboBox *m_background;
    QColor m_fg, m_bg;
};

// Used for both Image Size (scales) and Canvas Size (adds/removes border, with an anchor).
class SizeDialog : public QDialog {
    Q_OBJECT
public:
    SizeDialog(const QString &title, const QSize &current, bool canvasMode, QWidget *parent = nullptr);
    QSize newSize() const;
    QPoint offset() const;  // canvas mode: where the old image lands in the new canvas

private:
    QSize m_current;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QCheckBox *m_keepAspect = nullptr;
    QButtonGroup *m_anchor = nullptr;
};

// Creates or edits the text of a text layer.
class TextDialog : public QDialog {
    Q_OBJECT
public:
    explicit TextDialog(const TextData &initial, QWidget *parent = nullptr);
    TextData data() const;

    // Opens the dialog for an existing text layer and applies the result (undoable).
    static void editLayer(Document *doc, int index, QWidget *parent);

private:
    void updateColorButton();

    TextData m_initial;
    QColor m_color;
    QPlainTextEdit *m_edit;
    QFontComboBox *m_font;
    QSpinBox *m_size;
    QCheckBox *m_bold;
    QCheckBox *m_italic;
    QCheckBox *m_antialias;
    QPushButton *m_colorButton;
};
