// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Autosave.h"
#include "FilterDialog.h"
#include "Tools.h"

#include <QMainWindow>
#include <functional>

class Canvas;
class Document;
class LayersPanel;
class QLabel;
class QMenu;
class QCheckBox;
class QFontComboBox;
class QSpinBox;
class QTabWidget;
class QUndoGroup;
class ToolSettings;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openFile(const QString &path);
    void addDocument(Document *doc);
    Autosave *autosave() const { return m_autosave; }
    // After a crash: offers to reopen the documents that had unsaved changes.
    void offerRecovery();
    // Opens recovery copies as unsaved documents; returns the copies that couldn't be opened.
    QList<Autosave::Recovered> restoreRecovered(const QList<Autosave::Recovered> &copies);

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    enum Option { OptSize, OptHardness, OptOpacity, OptTolerance, OptContiguous, OptSampleMerged,
                  OptFill, OptAntialias, OptRadial, OptFont, OptPressure, OptRange, OptSponge, OptCropRatio, OptSmoothing, OptCount };

    Canvas *canvas() const;
    Document *doc() const;

    void createDocks();
    void createMenus();
    void createToolBox();
    void createOptionsBar();
    void createStatusBar();

    QAction *addAction(QMenu *menu, const QString &text, const QKeySequence &key,
                       const std::function<void()> &fn, bool needsDocument = true);
    void withDoc(const std::function<void(Document *)> &fn);
    bool hasPixels(Document *d);  // false (with a message) when a group without a mask is selected
    using FilterFunc = std::function<QImage(const QImage &, const QList<int> &)>;
    void runFilter(const QString &title, const QList<FilterParam> &params, const FilterFunc &fn);
    void runAdjustment(Adjustment::Type type);
    void colorRange();
    void layerStyle();
    ParamEditor *adjustmentEditor(Adjustment::Type type, const QList<int> &values, const QImage &histogramSource);
    void newAdjustmentLayer(Adjustment::Type type);
    void editLayer(int index);  // adjustment parameters or text

    void newImage();
    void openDialog();
    bool saveDocument(int tab, bool saveAs);
    void exportDocument();
    bool maybeSave(int tab);
    bool closeTab(int tab);
    void copy(bool merged);
    void cut();
    void paste();
    void pasteAsNew();
    void imageSize();
    void canvasSize();
    void setInterfaceScale(double scale);
    void addRecentFile(const QString &path);
    void rebuildRecentMenu();

    void onCurrentTabChanged();
    void onToolChanged(int id);
    void updateTabTitle(Canvas *c);
    void updateWindowTitle();
    void updateStatus();

    ToolSettings *m_settings;
    ToolManager *m_tools;
    Autosave *m_autosave = nullptr;
    QAction *m_showGuidesAction = nullptr;
    QUndoGroup *m_undoGroup;
    QTabWidget *m_tabs;
    LayersPanel *m_layers;
    QMenu *m_viewMenu = nullptr;
    QMenu *m_recentMenu = nullptr;
    QList<QAction *> m_docActions;
    QList<QAction *> m_toolActions;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_optionActions[OptCount] = {};
    QSpinBox *m_sizeSpin = nullptr;
    QLabel *m_toolNameLabel = nullptr;
    QLabel *m_opacityLabel = nullptr;
    QFontComboBox *m_fontBox = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QCheckBox *m_bold = nullptr;
    QCheckBox *m_italic = nullptr;
    QCheckBox *m_antialiasBox = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_posLabel = nullptr;
    QLabel *m_sizeLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
};
