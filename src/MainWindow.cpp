// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "MainWindow.h"

#include "Canvas.h"
#include "ColorWidgets.h"
#include "Dialogs.h"
#include "Document.h"
#include "FileIO.h"
#include "Filters.h"
#include "Icons.h"
#include "LayersPanel.h"
#include "ToolSettings.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUndoGroup>
#include <QUndoView>
#include <QVBoxLayout>
#include <QtMath>
#include <memory>

namespace {

constexpr int kMaxRecent = 10;

QList<int> optionsForTool(int id)
{
    using T = Tool;
    // Same order as MainWindow::Option.
    enum { Size, Hardness, Opacity, Tolerance, Contiguous, SampleMerged, Fill, Antialias, Radial, Font, Pressure, Range };
    switch (id) {
    case T::Brush:
    case T::Eraser: return {Size, Hardness, Opacity, Pressure};
    case T::CloneStamp:
    case T::Healing: return {Size, Hardness, Opacity, SampleMerged, Pressure};
    case T::Smudge: return {Size, Hardness, Opacity, Pressure};
    case T::Dodge:
    case T::Burn: return {Size, Hardness, Range, Opacity, Pressure};
    case T::Fill: return {Tolerance, Contiguous, SampleMerged, Opacity};
    case T::MagicWand: return {Tolerance, Contiguous, SampleMerged};
    case T::Gradient: return {Opacity, Radial};
    case T::LineShape: return {Size, Opacity, Antialias};
    case T::RectShape:
    case T::EllipseShape: return {Size, Opacity, Fill, Antialias};
    case T::Text: return {Font, Antialias};
    case T::EllipseSelect:
    case T::Lasso: return {Antialias};
    default: return {};
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowIcon(appIcon());
    setAcceptDrops(true);
    setDockNestingEnabled(true);

    m_settings = new ToolSettings(this);
    m_tools = new ToolManager(m_settings, this);
    m_undoGroup = new QUndoGroup(this);

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    createDocks();
    createMenus();
    createToolBox();
    createOptionsBar();
    createStatusBar();

    connect(m_tools, &ToolManager::toolChanged, this, &MainWindow::onToolChanged);
    // Text being typed takes the foreground color; editing existing text shows its style.
    connect(m_settings, &ToolSettings::colorsChanged, this, [this] { m_tools->current()->settingsChanged(); });
    connect(m_settings, &ToolSettings::textStyleChanged, this, [this] {
        const QSignalBlocker b1(m_fontBox), b2(m_fontSize), b3(m_bold), b4(m_italic), b5(m_antialiasBox);
        const QFont &f = m_settings->font;
        m_fontBox->setCurrentFont(f);
        m_fontSize->setValue(f.pixelSize() > 0 ? f.pixelSize() : 48);
        m_bold->setChecked(f.bold());
        m_italic->setChecked(f.italic());
        m_antialiasBox->setChecked(m_settings->antialias);
    });
    onToolChanged(m_tools->currentId());

    QSettings s;
    if (!restoreGeometry(s.value("window/geometry").toByteArray())) {
        const QRect avail = screen()->availableGeometry();
        resize(avail.size() * 0.85);
    }
    restoreState(s.value("window/state").toByteArray());
    onCurrentTabChanged();
}

Canvas *MainWindow::canvas() const { return qobject_cast<Canvas *>(m_tabs->currentWidget()); }

Document *MainWindow::doc() const
{
    Canvas *c = canvas();
    return c ? c->document() : nullptr;
}

bool MainWindow::hasPixels(Document *d)
{
    if (d->canEditPixels())
        return true;
    statusBar()->showMessage(tr("A group is selected. Select a layer inside it, or add a mask to the group."), 4000);
    return false;
}

void MainWindow::withDoc(const std::function<void(Document *)> &fn)
{
    if (Document *d = doc()) {
        m_tools->current()->cancel();
        fn(d);
    }
}

QAction *MainWindow::addAction(QMenu *menu, const QString &text, const QKeySequence &key,
                               const std::function<void()> &fn, bool needsDocument)
{
    QAction *a = menu->addAction(text);
    if (!key.isEmpty())
        a->setShortcut(key);
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    if (needsDocument)
        m_docActions << a;
    return a;
}

// ---------------------------------------------------------------------------
// UI construction

void MainWindow::createDocks()
{
    auto *colorDock = new QDockWidget(tr("Swatches"), this);
    colorDock->setObjectName("swatchesDock");
    auto *colorHolder = new QWidget;
    auto *colorLayout = new QVBoxLayout(colorHolder);
    colorLayout->setContentsMargins(6, 6, 6, 6);
    colorLayout->addWidget(new SwatchPalette(m_settings));
    colorDock->setWidget(colorHolder);
    addDockWidget(Qt::RightDockWidgetArea, colorDock);

    auto *layersDock = new QDockWidget(tr("Layers"), this);
    layersDock->setObjectName("layersDock");
    m_layers = new LayersPanel;
    connect(m_layers, &LayersPanel::newAdjustmentRequested, this, &MainWindow::newAdjustmentLayer);
    connect(m_layers, &LayersPanel::editAdjustmentRequested, this, &MainWindow::editLayer);
    connect(m_layers, &LayersPanel::editTextRequested, this, &MainWindow::editLayer);
    connect(m_layers, &LayersPanel::layerStyleRequested, this, &MainWindow::layerStyle);
    layersDock->setWidget(m_layers);
    addDockWidget(Qt::RightDockWidgetArea, layersDock);

    auto *historyDock = new QDockWidget(tr("History"), this);
    historyDock->setObjectName("historyDock");
    auto *history = new QUndoView(m_undoGroup);
    history->setEmptyLabel(tr("<Open>"));
    historyDock->setWidget(history);
    addDockWidget(Qt::RightDockWidgetArea, historyDock);

    resizeDocks({colorDock, layersDock, historyDock}, {110, 420, 200}, Qt::Vertical);
    resizeDocks({layersDock}, {280}, Qt::Horizontal);

    m_viewMenu = new QMenu(tr("&Panels"), this);
    m_viewMenu->addAction(layersDock->toggleViewAction());
    m_viewMenu->addAction(historyDock->toggleViewAction());
    m_viewMenu->addAction(colorDock->toggleViewAction());
}

void MainWindow::createMenus()
{
    // File
    QMenu *file = menuBar()->addMenu(tr("&File"));
    addAction(file, tr("&New…"), QKeySequence::New, [this] { newImage(); }, false);
    addAction(file, tr("&Open…"), QKeySequence::Open, [this] { openDialog(); }, false);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    file->addSeparator();
    addAction(file, tr("&Save"), QKeySequence::Save, [this] { saveDocument(m_tabs->currentIndex(), false); });
    addAction(file, tr("Save &As…"), QKeySequence("Ctrl+Shift+S"), [this] { saveDocument(m_tabs->currentIndex(), true); });
    addAction(file, tr("&Export As…"), QKeySequence("Ctrl+Shift+E"), [this] { exportDocument(); });
    file->addSeparator();
    addAction(file, tr("&Close"), QKeySequence("Ctrl+W"), [this] { closeTab(m_tabs->currentIndex()); });
    addAction(file, tr("&Quit"), QKeySequence("Ctrl+Q"), [this] { close(); }, false);

    // Edit
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    // Own undo/redo actions so an in-progress tool interaction (e.g. a free
    // transform) is finished before the history changes.
    m_undoAction = edit->addAction(tr("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this] {
        m_tools->current()->cancel();
        m_undoGroup->undo();
    });
    m_redoAction = edit->addAction(tr("&Redo"));
    m_redoAction->setShortcuts({QKeySequence("Ctrl+Shift+Z"), QKeySequence("Ctrl+Y")});
    connect(m_redoAction, &QAction::triggered, this, [this] {
        m_tools->current()->cancel();
        m_undoGroup->redo();
    });
    auto updateUndo = [this] {
        m_undoAction->setEnabled(m_undoGroup->canUndo());
        m_redoAction->setEnabled(m_undoGroup->canRedo());
        const QString u = m_undoGroup->undoText(), r = m_undoGroup->redoText();
        m_undoAction->setText(u.isEmpty() ? tr("&Undo") : tr("&Undo %1").arg(u));
        m_redoAction->setText(r.isEmpty() ? tr("&Redo") : tr("&Redo %1").arg(r));
    };
    connect(m_undoGroup, &QUndoGroup::canUndoChanged, this, updateUndo);
    connect(m_undoGroup, &QUndoGroup::canRedoChanged, this, updateUndo);
    connect(m_undoGroup, &QUndoGroup::undoTextChanged, this, updateUndo);
    connect(m_undoGroup, &QUndoGroup::redoTextChanged, this, updateUndo);
    connect(m_undoGroup, &QUndoGroup::activeStackChanged, this, updateUndo);
    updateUndo();
    edit->addSeparator();
    addAction(edit, tr("Cu&t"), QKeySequence::Cut, [this] { cut(); });
    addAction(edit, tr("&Copy"), QKeySequence::Copy, [this] { copy(false); });
    addAction(edit, tr("Copy &Merged"), QKeySequence("Ctrl+Shift+C"), [this] { copy(true); });
    addAction(edit, tr("&Paste"), QKeySequence::Paste, [this] { paste(); }, false);
    addAction(edit, tr("Paste as New &Image"), QKeySequence("Ctrl+Alt+V"), [this] { pasteAsNew(); }, false);
    edit->addSeparator();
    addAction(edit, tr("C&lear"), QKeySequence::Delete, [this] { withDoc([this](Document *d) { if (hasPixels(d)) d->clearSelected(); }); });
    addAction(edit, tr("&Fill with Foreground"), QKeySequence("Alt+Backspace"),
              [this] { withDoc([this](Document *d) { if (hasPixels(d)) d->fillSelected(m_settings->foreground()); }); });
    addAction(edit, tr("Fill with &Background"), QKeySequence("Ctrl+Backspace"),
              [this] { withDoc([this](Document *d) { if (hasPixels(d)) d->fillSelected(m_settings->background()); }); });
    edit->addSeparator();
    addAction(edit, tr("Free &Transform") + QStringLiteral("\tCtrl+T"), {}, [this] {
        m_tools->setCurrent(Tool::Transform);
        if (canvas())
            canvas()->activateTool();
    });

    // Image
    QMenu *image = menuBar()->addMenu(tr("&Image"));
    QMenu *adjust = image->addMenu(tr("&Adjustments"));
    const QList<QPair<Adjustment::Type, QKeySequence>> adjustments = {
        {Adjustment::BrightnessContrast, {}}, {Adjustment::Levels, QKeySequence("Ctrl+L")},
        {Adjustment::Curves, QKeySequence("Ctrl+M")}, {Adjustment::HueSaturation, QKeySequence("Ctrl+U")},
        {Adjustment::Invert, QKeySequence("Ctrl+I")}, {Adjustment::Threshold, {}}, {Adjustment::Posterize, {}},
    };
    for (const auto &entry : adjustments) {
        const Adjustment::Type type = entry.first;  // a named copy, so the lambda below can capture it
        const QKeySequence key = entry.second;
        const QString suffix = type == Adjustment::Invert ? QString() : QStringLiteral("…");
        addAction(adjust, Adjustments::name(type) + suffix, key, [this, type] { runAdjustment(type); });
    }
    adjust->addSeparator();
    addAction(adjust, tr("&Desaturate"), QKeySequence("Ctrl+Shift+U"), [this] {
        runFilter(tr("Desaturate"), {}, [](const QImage &img, const QList<int> &) { return Filters::desaturate(img); });
    });
    image->addSeparator();
    addAction(image, tr("Image &Size…"), QKeySequence("Ctrl+Alt+I"), [this] { imageSize(); });
    addAction(image, tr("&Canvas Size…"), QKeySequence("Ctrl+Alt+C"), [this] { canvasSize(); });
    addAction(image, tr("Crop to Se&lection"), {}, [this] {
        withDoc([](Document *d) {
            if (d->hasSelection())
                d->crop(d->selectionBounds());
        });
    });
    QMenu *rotate = image->addMenu(tr("&Rotate / Flip"));
    addAction(rotate, tr("Rotate 90° &Clockwise"), {}, [this] { withDoc([](Document *d) { d->rotate(90); }); });
    addAction(rotate, tr("Rotate 90° C&ounterclockwise"), {}, [this] { withDoc([](Document *d) { d->rotate(-90); }); });
    addAction(rotate, tr("Rotate &180°"), {}, [this] { withDoc([](Document *d) { d->rotate(180); }); });
    rotate->addSeparator();
    addAction(rotate, tr("Flip &Horizontal"), {}, [this] { withDoc([](Document *d) { d->flip(Qt::Horizontal); }); });
    addAction(rotate, tr("Flip &Vertical"), {}, [this] { withDoc([](Document *d) { d->flip(Qt::Vertical); }); });
    image->addSeparator();
    addAction(image, tr("&Flatten Image"), {}, [this] { withDoc([](Document *d) { d->flatten(); }); });

    // Layer
    QMenu *layer = menuBar()->addMenu(tr("&Layer"));
    addAction(layer, tr("&New Layer"), QKeySequence("Ctrl+Shift+N"), [this] { withDoc([](Document *d) { d->addLayer(); }); });
    addAction(layer, tr("&Duplicate Layer"), QKeySequence("Ctrl+J"), [this] { withDoc([](Document *d) { d->duplicateLayer(); }); });
    addAction(layer, tr("De&lete Layer"), {}, [this] { withDoc([](Document *d) { d->deleteLayer(); }); });
    addAction(layer, tr("New &Group"), {}, [this] { withDoc([](Document *d) { d->newGroup(); }); });
    addAction(layer, tr("&Group Layers"), QKeySequence("Ctrl+G"), [this] { withDoc([](Document *d) { d->groupActiveLayer(); }); });
    addAction(layer, tr("&Ungroup Layers"), QKeySequence("Ctrl+Shift+G"), [this] { withDoc([](Document *d) { d->ungroup(); }); });
    QMenu *adjLayer = layer->addMenu(tr("New &Adjustment Layer"));
    for (int t = Adjustment::BrightnessContrast; t < Adjustment::TypeCount; ++t) {
        const auto type = Adjustment::Type(t);
        addAction(adjLayer, Adjustments::name(type) + QStringLiteral("…"), {}, [this, type] { newAdjustmentLayer(type); });
    }
    addAction(layer, tr("Layer &Content Options…"), {}, [this] { if (doc()) editLayer(doc()->activeIndex()); });
    addAction(layer, tr("Create / Release &Clipping Mask"), QKeySequence("Ctrl+Alt+G"), [this] {
        withDoc([](Document *d) { d->setLayerClipped(d->activeIndex(), !d->activeLayer().clipped); });
    });
    addAction(layer, tr("Layer St&yle…"), {}, [this] { layerStyle(); });
    addAction(layer, tr("Clear Layer Style"), {}, [this] {
        withDoc([](Document *d) { d->setLayerStyle(d->activeIndex(), LayerStyle()); });
    });
    addAction(layer, tr("Rasteri&ze Layer"), {}, [this] {
        withDoc([](Document *d) { d->rasterizeLayer(d->activeIndex()); });
    });
    layer->addSeparator();
    QMenu *maskMenu = layer->addMenu(tr("Layer M&ask"));
    addAction(maskMenu, tr("&Reveal All"), {}, [this] { withDoc([](Document *d) { d->addMask(false); }); });
    addAction(maskMenu, tr("Reveal &Selection"), {}, [this] { withDoc([](Document *d) { d->addMask(true); }); });
    maskMenu->addSeparator();
    addAction(maskMenu, tr("&Edit Mask / Layer"), QKeySequence("Ctrl+\\"), [this] {
        withDoc([](Document *d) { d->setEditingMask(!d->editingMask()); });
    });
    addAction(maskMenu, tr("Enable / &Disable"), {}, [this] {
        withDoc([](Document *d) { d->setMaskEnabled(!d->activeLayer().maskEnabled); });
    });
    addAction(maskMenu, tr("&Apply"), {}, [this] { withDoc([](Document *d) { d->applyMask(); }); });
    addAction(maskMenu, tr("De&lete"), {}, [this] { withDoc([](Document *d) { d->deleteMask(); }); });
    layer->addSeparator();
    addAction(layer, tr("&Raise Layer"), QKeySequence("Ctrl+]"), [this] { withDoc([](Document *d) { d->moveLayer(1); }); });
    addAction(layer, tr("L&ower Layer"), QKeySequence("Ctrl+["), [this] { withDoc([](Document *d) { d->moveLayer(-1); }); });
    layer->addSeparator();
    addAction(layer, tr("&Merge Down"), QKeySequence("Ctrl+E"), [this] { withDoc([](Document *d) { d->mergeDown(); }); });
    addAction(layer, tr("&Flatten Image"), {}, [this] { withDoc([](Document *d) { d->flatten(); }); });

    // Select
    QMenu *select = menuBar()->addMenu(tr("&Select"));
    addAction(select, tr("&All"), QKeySequence::SelectAll, [this] { withDoc([](Document *d) { d->selectAll(); }); });
    addAction(select, tr("&Deselect"), QKeySequence("Ctrl+D"), [this] { withDoc([](Document *d) { d->deselect(); }); });
    addAction(select, tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), [this] { withDoc([](Document *d) { d->invertSelection(); }); });
    select->addSeparator();
    addAction(select, tr("&Color Range…"), {}, [this] { colorRange(); });
    addAction(select, tr("&Load Layer Transparency"), {}, [this] {
        withDoc([](Document *d) { d->selectLayerTransparency(); });
    });
    QMenu *modify = select->addMenu(tr("&Modify"));
    // Asks for a pixel amount, then applies `op` to the selection.
    auto modifyAction = [this, modify](const QString &text, const QString &label, int def, const QKeySequence &key,
                                       std::function<void(Document *, double)> op) {
        addAction(modify, text, key, [this, text, label, def, op] {
            withDoc([&](Document *d) {
                if (!d->hasSelection()) {
                    statusBar()->showMessage(tr("Make a selection first."), 3000);
                    return;
                }
                bool ok = false;
                const int v = QInputDialog::getInt(this, QString(text).remove(QLatin1Char('&')).remove(QStringLiteral("…")),
                                                   label, def, 1, 500, 1, &ok);
                if (ok) {
                    QApplication::setOverrideCursor(Qt::WaitCursor);
                    op(d, v);
                    QApplication::restoreOverrideCursor();
                }
            });
        });
    };
    modifyAction(tr("&Feather…"), tr("Feather radius (px):"), 5, QKeySequence("Shift+F6"),
                 [](Document *d, double v) { d->featherSelection(v); });
    modifyAction(tr("&Expand…"), tr("Expand by (px):"), 5, {}, [](Document *d, double v) { d->growSelection(v); });
    modifyAction(tr("&Contract…"), tr("Contract by (px):"), 5, {}, [](Document *d, double v) { d->shrinkSelection(v); });
    modifyAction(tr("&Border…"), tr("Border width (px):"), 10, {}, [](Document *d, double v) { d->borderSelection(v); });
    modifyAction(tr("&Smooth…"), tr("Smooth radius (px):"), 5, {}, [](Document *d, double v) { d->smoothSelection(v); });

    // Filter
    QMenu *filter = menuBar()->addMenu(tr("Fil&ter"));
    addAction(filter, tr("&Gaussian Blur…"), {}, [this] {
        runFilter(tr("Gaussian Blur"), {{tr("Radius"), 1, 250, 4, tr(" px")}},
                  [](const QImage &img, const QList<int> &v) { return Filters::gaussianBlur(img, v[0]); });
    });
    addAction(filter, tr("&Unsharp Mask…"), {}, [this] {
        runFilter(tr("Unsharp Mask"),
                  {{tr("Amount"), 1, 500, 100, QStringLiteral("%")},
                   {tr("Radius"), 1, 100, 2, tr(" px")},
                   {tr("Threshold"), 0, 255, 0, {}}},
                  [](const QImage &img, const QList<int> &v) { return Filters::unsharpMask(img, v[0], v[1], v[2]); });
    });
    addAction(filter, tr("Add &Noise…"), {}, [this] {
        runFilter(tr("Add Noise"), {{tr("Amount"), 1, 100, 15, QStringLiteral("%")}},
                  [](const QImage &img, const QList<int> &v) { return Filters::addNoise(img, v[0]); });
    });
    addAction(filter, tr("&Pixelate…"), {}, [this] {
        runFilter(tr("Pixelate"), {{tr("Cell size"), 2, 200, 10, tr(" px")}},
                  [](const QImage &img, const QList<int> &v) { return Filters::pixelate(img, v[0]); });
    });

    // View
    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *zoomIn = addAction(view, tr("Zoom &In"), QKeySequence::ZoomIn, [this] { if (canvas()) canvas()->zoomIn(); });
    zoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence("Ctrl+=")});
    addAction(view, tr("Zoom &Out"), QKeySequence::ZoomOut, [this] { if (canvas()) canvas()->zoomOut(); });
    addAction(view, tr("&Fit on Screen"), QKeySequence("Ctrl+0"), [this] { if (canvas()) canvas()->fitToWindow(); });
    addAction(view, tr("&Actual Pixels"), QKeySequence("Ctrl+1"), [this] { if (canvas()) canvas()->actualPixels(); });
    view->addSeparator();
    QMenu *uiSize = view->addMenu(tr("&Interface Size"));
    auto *sizeGroup = new QActionGroup(this);
    const double current = QSettings().value("ui/scale", 1.0).toDouble();
    for (int percent : {100, 125, 150, 175, 200, 250}) {
        QAction *a = uiSize->addAction(tr("%1%").arg(percent));
        a->setCheckable(true);
        a->setChecked(qAbs(current * 100 - percent) < 1);
        sizeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, percent] { setInterfaceScale(percent / 100.0); });
    }
    view->addMenu(m_viewMenu);

    // Help
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    addAction(help, tr("&About PairPaint"), {}, [this] {
        QMessageBox::about(this, tr("About PairPaint"),
                           tr("<h3>PairPaint %1</h3><p>A cross-platform layered image editor built with Qt %2.</p>"
                              "<p>Created through pair programming between a human and an AI.</p>"
                              "<p>Copyright © 2026 Peter Gniewek and PairPaint contributors.<br>"
                              "Free software, licensed under the GNU General Public License v3 or later.</p>")
                               .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    }, false);
    addAction(help, tr("About &Qt"), {}, [] { QApplication::aboutQt(); }, false);

    // Window-level shortcuts that are not in a menu.
    auto shortcut = [this](const QKeySequence &key, const std::function<void()> &fn) {
        auto *a = new QAction(this);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        QMainWindow::addAction(a);
    };
    shortcut(QKeySequence("X"), [this] { m_settings->swapColors(); });
    shortcut(QKeySequence("D"), [this] { m_settings->resetColors(); });
    shortcut(QKeySequence("]"), [this] { m_sizeSpin->setValue(m_sizeSpin->value() + std::max(1, m_sizeSpin->value() / 5)); });
    shortcut(QKeySequence("["), [this] { m_sizeSpin->setValue(m_sizeSpin->value() - std::max(1, m_sizeSpin->value() / 6)); });
}

void MainWindow::createToolBox()
{
    QToolBar *bar = new QToolBar(tr("Tools"), this);
    bar->setObjectName("toolsBar");
    bar->setOrientation(Qt::Vertical);
    bar->setMovable(false);
    addToolBar(Qt::LeftToolBarArea, bar);

    // Two columns of tool buttons, like most image editors.
    auto *box = new QWidget;
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(1);
    auto *group = new QActionGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < Tool::Count; ++i) {
        const auto id = Tool::Id(i);
        auto *a = new QAction(toolIcon(id), Tool::name(id), this);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(Tool::shortcut(id)));
        a->setToolTip(QStringLiteral("%1 (%2)").arg(Tool::name(id), Tool::shortcut(id)));
        a->setData(i);
        group->addAction(a);
        QMainWindow::addAction(a);  // keep the shortcut active
        connect(a, &QAction::triggered, this, [this, id] { m_tools->setCurrent(id); });
        m_toolActions << a;

        auto *b = new QToolButton;
        b->setDefaultAction(a);
        b->setIconSize(QSize(22, 22));
        b->setAutoRaise(true);
        grid->addWidget(b, i / 2, i % 2);
    }
    bar->addWidget(box);
    bar->addSeparator();
    bar->addWidget(new ColorSwatch(m_settings));
}

void MainWindow::createOptionsBar()
{
    QToolBar *bar = addToolBar(tr("Tool Options"));
    bar->setObjectName("optionsBar");
    bar->setMovable(false);

    m_toolNameLabel = new QLabel;
    m_toolNameLabel->setContentsMargins(6, 0, 10, 0);
    QFont bold = m_toolNameLabel->font();
    bold.setBold(true);
    m_toolNameLabel->setFont(bold);
    bar->addWidget(m_toolNameLabel);

    auto labelled = [&](const QString &label, QWidget *w) {
        auto *holder = new QWidget;
        auto *l = new QHBoxLayout(holder);
        l->setContentsMargins(6, 0, 6, 0);
        l->setSpacing(4);
        if (!label.isEmpty())
            l->addWidget(new QLabel(label));
        l->addWidget(w);
        return bar->addWidget(holder);
    };
    auto spin = [&](Option opt, const QString &label, int min, int max, int value, const QString &suffix,
                    const std::function<void(int)> &set) {
        auto *s = new QSpinBox;
        s->setRange(min, max);
        s->setValue(value);
        s->setSuffix(suffix);
        s->setKeyboardTracking(false);
        connect(s, &QSpinBox::valueChanged, this, [set](int v) { set(v); });
        m_optionActions[opt] = labelled(label, s);
        return s;
    };
    auto check = [&](Option opt, const QString &label, bool value, const std::function<void(bool)> &set) {
        auto *c = new QCheckBox(label);
        c->setChecked(value);
        connect(c, &QCheckBox::toggled, this, [set](bool v) { set(v); });
        m_optionActions[opt] = labelled(QString(), c);
    };

    ToolSettings *s = m_settings;
    m_sizeSpin = spin(OptSize, tr("Size:"), 1, 2000, s->size, tr(" px"), [this, s](int v) {
        s->size = v;
        if (canvas()) canvas()->update();
    });
    spin(OptHardness, tr("Hardness:"), 0, 100, s->hardness, QStringLiteral("%"), [s](int v) { s->hardness = v; });
    spin(OptOpacity, tr("Opacity:"), 1, 100, s->opacity, QStringLiteral("%"), [s](int v) { s->opacity = v; });
    m_opacityLabel = bar->widgetForAction(m_optionActions[OptOpacity])->findChild<QLabel *>();
    spin(OptTolerance, tr("Tolerance:"), 0, 255, s->tolerance, {}, [s](int v) { s->tolerance = v; });
    check(OptContiguous, tr("Contiguous"), s->contiguous, [s](bool v) { s->contiguous = v; });
    check(OptSampleMerged, tr("Sample all layers"), s->sampleMerged, [s](bool v) { s->sampleMerged = v; });
    check(OptFill, tr("Filled"), s->fillShape, [s](bool v) { s->fillShape = v; });
    check(OptAntialias, tr("Anti-alias"), s->antialias, [this, s](bool v) {
        s->antialias = v;
        m_tools->current()->settingsChanged();
    });
    m_antialiasBox = qobject_cast<QCheckBox *>(bar->widgetForAction(m_optionActions[OptAntialias])->findChild<QCheckBox *>());
    check(OptRadial, tr("Radial"), s->radial, [s](bool v) { s->radial = v; });

    auto *range = new QComboBox;
    range->addItems({tr("Shadows"), tr("Midtones"), tr("Highlights")});
    range->setCurrentIndex(s->toneRange);
    connect(range, &QComboBox::currentIndexChanged, this, [s](int v) { s->toneRange = v; });
    m_optionActions[OptRange] = labelled(tr("Range:"), range);

    auto *pressureHolder = new QWidget;
    auto *pl = new QHBoxLayout(pressureHolder);
    pl->setContentsMargins(6, 0, 6, 0);
    pl->addWidget(new QLabel(tr("Pen pressure:")));
    auto *pSize = new QCheckBox(tr("Size"));
    pSize->setChecked(s->pressureSize);
    connect(pSize, &QCheckBox::toggled, this, [s](bool v) { s->pressureSize = v; });
    auto *pOpacity = new QCheckBox(tr("Opacity"));
    pOpacity->setChecked(s->pressureOpacity);
    connect(pOpacity, &QCheckBox::toggled, this, [s](bool v) { s->pressureOpacity = v; });
    pl->addWidget(pSize);
    pl->addWidget(pOpacity);
    m_optionActions[OptPressure] = bar->addWidget(pressureHolder);

    auto *fontHolder = new QWidget;
    auto *fl = new QHBoxLayout(fontHolder);
    fl->setContentsMargins(6, 0, 6, 0);
    auto *fontBox = m_fontBox = new QFontComboBox;
    fontBox->setCurrentFont(s->font);
    auto *fontSize = m_fontSize = new QSpinBox;
    fontSize->setRange(4, 1000);
    fontSize->setValue(s->font.pixelSize());
    fontSize->setSuffix(tr(" px"));
    auto *boldBox = m_bold = new QCheckBox(tr("Bold"));
    auto *italicBox = m_italic = new QCheckBox(tr("Italic"));
    auto updateFont = [=] {
        QFont f = fontBox->currentFont();
        f.setPixelSize(fontSize->value());
        f.setBold(boldBox->isChecked());
        f.setItalic(italicBox->isChecked());
        s->font = f;
        m_tools->current()->settingsChanged();  // restyles text being typed
    };
    connect(fontBox, &QFontComboBox::currentFontChanged, this, updateFont);
    connect(fontSize, &QSpinBox::valueChanged, this, updateFont);
    connect(boldBox, &QCheckBox::toggled, this, updateFont);
    connect(italicBox, &QCheckBox::toggled, this, updateFont);
    updateFont();
    fl->addWidget(new QLabel(tr("Font:")));
    fl->addWidget(fontBox);
    fl->addWidget(fontSize);
    fl->addWidget(boldBox);
    fl->addWidget(italicBox);
    m_optionActions[OptFont] = bar->addWidget(fontHolder);

    bar->addSeparator();
    m_hintLabel = new QLabel;
    m_hintLabel->setContentsMargins(8, 0, 8, 0);
    m_hintLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    bar->addWidget(m_hintLabel);
}

void MainWindow::createStatusBar()
{
    m_posLabel = new QLabel;
    m_sizeLabel = new QLabel;
    m_zoomLabel = new QLabel;
    for (QLabel *l : {m_posLabel, m_sizeLabel, m_zoomLabel})
        l->setMinimumWidth(110);
    statusBar()->addPermanentWidget(m_posLabel);
    statusBar()->addPermanentWidget(m_sizeLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
}

// ---------------------------------------------------------------------------
// Documents

void MainWindow::addDocument(Document *doc)
{
    auto *c = new Canvas(doc, m_tools);
    m_undoGroup->addStack(doc->undoStack());
    connect(doc, &Document::titleChanged, this, [this, c] { updateTabTitle(c); });
    connect(doc, &Document::sizeChanged, this, &MainWindow::updateStatus);
    connect(c, &Canvas::zoomChanged, this, &MainWindow::updateStatus);
    connect(c, &Canvas::cursorMoved, this, [this](const QPointF &p) {
        m_posLabel->setText(QStringLiteral("X: %1  Y: %2").arg(qFloor(p.x())).arg(qFloor(p.y())));
    });
    const int idx = m_tabs->addTab(c, doc->displayName());
    m_tabs->setCurrentIndex(idx);
    updateTabTitle(c);
    c->setFocus();
}

void MainWindow::onCurrentTabChanged()
{
    m_tools->current()->cancel();
    Document *d = doc();
    m_undoGroup->setActiveStack(d ? d->undoStack() : nullptr);
    m_layers->setDocument(d);
    for (QAction *a : std::as_const(m_docActions))
        a->setEnabled(d);
    updateWindowTitle();
    updateStatus();
    if (canvas())
        canvas()->activateTool();
}

void MainWindow::updateTabTitle(Canvas *c)
{
    const int idx = m_tabs->indexOf(c);
    if (idx < 0)
        return;
    Document *d = c->document();
    m_tabs->setTabText(idx, d->displayName() + (d->isModified() ? QStringLiteral(" *") : QString()));
    m_tabs->setTabToolTip(idx, d->filePath());
    if (c == canvas())
        updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    Document *d = doc();
    if (!d) {
        setWindowModified(false);
        setWindowTitle(QStringLiteral("PairPaint"));
        return;
    }
    setWindowTitle(QStringLiteral("%1[*] — PairPaint").arg(d->displayName()));
    setWindowModified(d->isModified());
}

void MainWindow::updateStatus()
{
    Document *d = doc();
    if (!d) {
        m_posLabel->clear();
        m_sizeLabel->clear();
        m_zoomLabel->clear();
        return;
    }
    m_sizeLabel->setText(tr("%1 × %2 px").arg(d->size().width()).arg(d->size().height()));
    m_zoomLabel->setText(tr("Zoom: %1%").arg(canvas()->zoom() * 100, 0, 'f', canvas()->zoom() < 0.1 ? 1 : 0));
}

void MainWindow::onToolChanged(int id)
{
    for (QAction *a : std::as_const(m_toolActions))
        if (a->data().toInt() == id)
            a->setChecked(true);
    const QList<int> opts = optionsForTool(id);
    for (int i = 0; i < OptCount; ++i)
        if (m_optionActions[i])
            m_optionActions[i]->setVisible(opts.contains(i));
    // The opacity option means "exposure" for Dodge/Burn and "strength" for Smudge.
    m_opacityLabel->setText(id == Tool::Dodge || id == Tool::Burn ? tr("Exposure:")
                            : id == Tool::Smudge                 ? tr("Strength:")
                                                                 : tr("Opacity:"));
    m_toolNameLabel->setText(Tool::name(Tool::Id(id)));
    m_hintLabel->setText(Tool::hint(Tool::Id(id)));
    if (canvas())
        canvas()->activateTool();
}

// ---------------------------------------------------------------------------
// File handling

void MainWindow::newImage()
{
    QSize initial(1920, 1080);
    const QImage clip = QApplication::clipboard()->image();
    if (!clip.isNull())
        initial = clip.size();
    NewImageDialog dlg(m_settings->foreground(), m_settings->background(), initial, this);
    if (dlg.exec() == QDialog::Accepted)
        addDocument(new Document(dlg.imageSize(), dlg.background()));
}

void MainWindow::openDialog()
{
    QSettings s;
    const QString dir = s.value("lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open"), dir, FileIO::openFilter());
    for (const QString &f : files)
        openFile(f);
}

void MainWindow::openFile(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *c = qobject_cast<Canvas *>(m_tabs->widget(i));
        if (c && c->document()->filePath() == abs) {
            m_tabs->setCurrentIndex(i);
            return;
        }
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    QString warning;
    Document *d = FileIO::load(abs, &error, &warning);
    QApplication::restoreOverrideCursor();
    if (!d) {
        QMessageBox::critical(this, tr("Open"), tr("Could not open %1:\n%2").arg(QFileInfo(abs).fileName(), error));
        return;
    }
    d->setFilePath(abs);
    addDocument(d);
    addRecentFile(abs);
    QSettings().setValue("lastDir", QFileInfo(abs).absolutePath());
    if (!warning.isEmpty())
        QMessageBox::information(this, QFileInfo(abs).fileName(), warning);
}

bool MainWindow::saveDocument(int tab, bool saveAs)
{
    auto *c = qobject_cast<Canvas *>(m_tabs->widget(tab));
    if (!c)
        return false;
    m_tools->current()->cancel();
    Document *d = c->document();
    QString path = d->filePath();
    bool layered = d->layerCount() > 1;
    for (int i = 0; i < d->layerCount(); ++i)
        layered |= !d->layer(i).mask.isNull() || d->layer(i).isAdjustment() || d->layer(i).isText();

    // Saving layers to a flat format would lose them, so default to the
    // project format in that case.
    if (saveAs || path.isEmpty() || (layered && !FileIO::isLayeredFormat(path))) {
        QString suggested = path.isEmpty()
            ? QDir(QSettings().value("lastDir", QDir::homePath()).toString()).filePath(d->displayName())
            : path;
        if ((layered && !FileIO::isLayeredFormat(suggested)) || path.isEmpty())
            suggested = QFileInfo(suggested).path() + QLatin1Char('/') + QFileInfo(suggested).completeBaseName() + QStringLiteral(".pairpaint");
        QString selectedFilter;
        path = QFileDialog::getSaveFileName(this, tr("Save As"), suggested, FileIO::saveFilter(), &selectedFilter);
        if (path.isEmpty())
            return false;
        if (QFileInfo(path).suffix().isEmpty()) {
            const int star = selectedFilter.indexOf(QStringLiteral("*."));
            const QString ext = star >= 0 ? selectedFilter.mid(star + 2).section(QRegularExpression("[ )]"), 0, 0)
                                          : QStringLiteral("pairpaint");
            path += QLatin1Char('.') + ext;
        }
    }

    QString error, warning;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = FileIO::save(d, path, &error, &warning);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::critical(this, tr("Save"), tr("Could not save %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    if (!warning.isEmpty())
        QMessageBox::warning(this, tr("Save"), warning);
    d->setFilePath(path);
    d->undoStack()->setClean();
    addRecentFile(path);
    QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

void MainWindow::exportDocument()
{
    Document *d = doc();
    if (!d)
        return;
    m_tools->current()->cancel();
    const QString base = d->filePath().isEmpty()
        ? QDir(QSettings().value("lastDir", QDir::homePath()).toString()).filePath(d->displayName())
        : d->filePath();
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Export As"),
                                                QFileInfo(base).path() + QLatin1Char('/') + QFileInfo(base).completeBaseName() + QStringLiteral(".png"),
                                                FileIO::exportFilter(), &selectedFilter);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");
    QString error;
    if (!FileIO::exportImage(d, path, &error))
        QMessageBox::critical(this, tr("Export"), tr("Could not export:\n%1").arg(error));
    else
        statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3000);
}

bool MainWindow::maybeSave(int tab)
{
    auto *c = qobject_cast<Canvas *>(m_tabs->widget(tab));
    if (!c || !c->document()->isModified())
        return true;
    m_tabs->setCurrentIndex(tab);
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved Changes"),
        tr("Save changes to \"%1\" before closing?").arg(c->document()->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveDocument(tab, false);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(int tab)
{
    if (tab < 0 || !maybeSave(tab))
        return false;
    m_tools->current()->cancel();
    QWidget *w = m_tabs->widget(tab);
    m_tabs->removeTab(tab);
    w->deleteLater();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    while (m_tabs->count() > 0) {
        if (!closeTab(m_tabs->count() - 1)) {
            e->ignore();
            return;
        }
    }
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls())
            if (url.isLocalFile())
                openFile(url.toLocalFile());
    } else if (mime->hasImage()) {
        addDocument(new Document(qvariant_cast<QImage>(mime->imageData())));
    }
    e->acceptProposedAction();
}

void MainWindow::setInterfaceScale(double scale)
{
    QSettings().setValue("ui/scale", scale);
    const auto answer = QMessageBox::question(
        this, tr("Interface Size"),
        tr("The new interface size takes effect after restarting PairPaint.\n\nRestart now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return;
    // Close normally (offers to save open documents), then start a fresh instance
    // that reopens the saved files.
    QStringList files;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *c = qobject_cast<Canvas *>(m_tabs->widget(i)); c && !c->document()->filePath().isEmpty())
            files << c->document()->filePath();
    if (!close())
        return;
    QProcess::startDetached(QApplication::applicationFilePath(), files);
}

void MainWindow::addRecentFile(const QString &path)
{
    QSettings s;
    QStringList files = s.value("recentFiles").toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > kMaxRecent)
        files.removeLast();
    s.setValue("recentFiles", files);
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList files = QSettings().value("recentFiles").toStringList();
    for (const QString &f : files) {
        QAction *a = m_recentMenu->addAction(QFileInfo(f).fileName());
        a->setToolTip(f);
        connect(a, &QAction::triggered, this, [this, f] { openFile(f); });
    }
    if (files.isEmpty()) {
        m_recentMenu->addAction(tr("(none)"))->setEnabled(false);
    } else {
        m_recentMenu->addSeparator();
        connect(m_recentMenu->addAction(tr("Clear List")), &QAction::triggered, this,
                [] { QSettings().remove("recentFiles"); });
    }
}

// ---------------------------------------------------------------------------
// Edit / image commands

void MainWindow::copy(bool merged)
{
    if (Document *d = doc())
        QApplication::clipboard()->setImage(d->copySelected(merged));
}

void MainWindow::cut()
{
    withDoc([this](Document *d) {
        if (!hasPixels(d))
            return;
        copy(false);
        d->clearSelected();
    });
}

void MainWindow::paste()
{
    const QImage img = QApplication::clipboard()->image();
    if (img.isNull()) {
        statusBar()->showMessage(tr("The clipboard does not contain an image."), 3000);
        return;
    }
    if (Document *d = doc()) {
        m_tools->current()->cancel();
        d->pasteImage(img);
        m_tools->setCurrent(Tool::Move);
    } else {
        addDocument(new Document(img));
    }
}

void MainWindow::pasteAsNew()
{
    const QImage img = QApplication::clipboard()->image();
    if (img.isNull()) {
        statusBar()->showMessage(tr("The clipboard does not contain an image."), 3000);
        return;
    }
    addDocument(new Document(img));
}

void MainWindow::imageSize()
{
    withDoc([this](Document *d) {
        SizeDialog dlg(tr("Image Size"), d->size(), false, this);
        if (dlg.exec() == QDialog::Accepted) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            d->resizeImage(dlg.newSize());
            QApplication::restoreOverrideCursor();
        }
    });
}

void MainWindow::canvasSize()
{
    withDoc([this](Document *d) {
        SizeDialog dlg(tr("Canvas Size"), d->size(), true, this);
        if (dlg.exec() == QDialog::Accepted)
            d->resizeCanvas(dlg.newSize(), dlg.offset());
    });
}

void MainWindow::layerStyle()
{
    withDoc([this](Document *d) {
        if (d->activeLayer().isAdjustment() || d->activeLayer().isGroup()) {
            statusBar()->showMessage(tr("Layer styles can only be added to pixel and text layers."), 3000);
            return;
        }
        LayerStyleDialog dlg(d, d->activeIndex(), this);
        dlg.exec();
    });
}

void MainWindow::colorRange()
{
    withDoc([&](Document *d) {
        // Selects pixels close to the foreground color, previewing the selection live.
        struct State {
            DocState before;
            QImage original;
            QImage sample;
        };
        auto st = std::make_shared<State>();
        st->before = d->state();
        st->original = d->selection();
        st->sample = d->flattened();
        const QColor color = m_settings->foreground();
        PreviewTarget target;
        target.preview = [d, st, color](const QList<int> &v) {
            d->setSelection(Filters::colorRangeMask(st->sample, color, v.value(0)));
        };
        target.commit = [d, st] { d->commit(tr("Color Range"), st->before); };
        target.cancel = [d, st] { d->setSelection(st->original); };
        FilterDialog dlg(tr("Color Range (foreground color %1)").arg(color.name()),
                         new SliderEditor({{tr("Fuzziness"), 0, 255, 40, {}}}), target, this);
        dlg.exec();
    });
}

void MainWindow::runFilter(const QString &title, const QList<FilterParam> &params, const FilterFunc &fn)
{
    withDoc([&](Document *d) {
        if (!hasPixels(d))
            return;
        if (params.isEmpty()) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            d->applyToActive(title, [&](const QImage &img) { return fn(img, {}); });
            QApplication::restoreOverrideCursor();
            return;
        }
        FilterDialog dlg(title, new SliderEditor(params), filterTarget(d, title, fn), this);
        dlg.exec();
    });
}

ParamEditor *MainWindow::adjustmentEditor(Adjustment::Type type, const QList<int> &values, const QImage &histogramSource)
{
    if (type == Adjustment::Curves)
        return new CurvesEditor(values, Filters::histogram(histogramSource));
    return new SliderEditor(Adjustments::params(type), values);
}

void MainWindow::runAdjustment(Adjustment::Type type)
{
    withDoc([&](Document *d) {
        const QString title = Adjustments::name(type);
        const FilterFunc fn = [type](const QImage &img, const QList<int> &v) { return Adjustments::apply(img, type, v); };
        if (type == Adjustment::Invert) {
            runFilter(title, {}, fn);
            return;
        }
        if (!hasPixels(d))
            return;
        d->prepareForPixelEdit();
        FilterDialog dlg(title, adjustmentEditor(type, Adjustments::defaults(type), d->targetImage()),
                         filterTarget(d, title, fn), this);
        dlg.exec();
    });
}

void MainWindow::newAdjustmentLayer(Adjustment::Type type)
{
    withDoc([&](Document *d) {
        Adjustment adj;
        adj.type = type;
        adj.params = Adjustments::defaults(type);
        if (type == Adjustment::Invert) {
            d->addAdjustmentLayer(adj);
            return;
        }
        const DocState before = d->state();
        const QImage below = d->flattened();
        d->addAdjustmentLayer(adj, false);
        const QString title = tr("New %1 Layer").arg(Adjustments::name(type));
        FilterDialog dlg(title, adjustmentEditor(type, adj.params, below),
                         adjustmentLayerTarget(d, d->activeIndex(), title, &before), this);
        dlg.exec();
    });
}

void MainWindow::editLayer(int index)
{
    withDoc([&](Document *d) {
        if (index < 0 || index >= d->layerCount())
            return;
        const Layer &l = d->layer(index);
        if (l.isText()) {
            TextDialog::editLayer(d, index, this);
        } else if (l.isAdjustment() && l.adjustment.type != Adjustment::Invert) {
            const Adjustment::Type type = l.adjustment.type;
            const QString title = tr("Edit %1").arg(Adjustments::name(type));
            FilterDialog dlg(title, adjustmentEditor(type, Adjustments::mainParams(type, l.adjustment.params), d->flattened()),
                             adjustmentLayerTarget(d, index, title, nullptr), this);
            dlg.exec();
        } else {
            statusBar()->showMessage(tr("Only text and adjustment layers have content options."), 3000);
        }
    });
}
