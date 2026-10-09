// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors
//
// End-to-end test of PairPaint: drives the real main window, canvas and tools
// with synthetic mouse/keyboard events (offscreen) and checks the resulting pixels.

#include "Adjustments.h"
#include "Canvas.h"
#include "Document.h"
#include "FileIO.h"
#include "FilterDialog.h"
#include "Filters.h"
#include "MainWindow.h"
#include "Psd.h"
#include "ToolSettings.h"
#include "Tools.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainterPath>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <cstdio>

static QTemporaryDir *outputDir = nullptr;
static QString tmpPath(const char *name) { return outputDir->filePath(QString::fromLatin1(name)); }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else std::printf("ok   %s\n", #c); } while (0)

static void mouse(QWidget *w, QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = {}) {
    QMouseEvent e(t, p, w->mapToGlobal(p), b, bs, m);
    QApplication::sendEvent(w, &e);
}
static void drag(Canvas *c, QPointF a, QPointF b, Qt::KeyboardModifiers m = {}) {
    QPointF wa = c->mapFromImage(a), wb = c->mapFromImage(b);
    mouse(c, QEvent::MouseButtonPress, wa, Qt::LeftButton, Qt::LeftButton, m);
    for (int i = 1; i <= 20; ++i) mouse(c, QEvent::MouseMove, wa + (wb - wa) * i / 20.0, Qt::NoButton, Qt::LeftButton, m);
    mouse(c, QEvent::MouseButtonRelease, wb, Qt::LeftButton, Qt::NoButton, m);
}
static QAction *action(QWidget *w, const QString &text) {
    for (QAction *a : w->findChildren<QAction *>()) if (a->text().remove('&') == text) return a;
    std::printf("no action %s\n", qPrintable(text)); return nullptr;
}
static QColor px(Document *d, int x, int y, int layer = -1) {
    const QImage &img = layer < 0 ? d->activeLayer().image : d->layer(layer).image;
    return img.pixelColor(x, y);
}


// Selection refinements on a plain document with known shapes.
static void testSelections()
{
    Document d(QSize(200, 200), Qt::white);
    QPainterPath sq;
    sq.addRect(50, 50, 100, 100);
    d.selectPath(sq, SelectionOp::Replace, false, "Select");
    CHECK(d.selectionBounds() == QRect(50, 50, 100, 100));

    d.growSelection(10);
    CHECK(d.selectionBounds() == QRect(40, 40, 120, 120));
    CHECK(d.selection().constScanLine(45)[100] == 255);   // edge midpoint grown fully
    CHECK(d.selection().constScanLine(42)[42] < 128);     // corner stays rounded, not square
    d.undoStack()->undo();

    d.shrinkSelection(10);
    CHECK(d.selectionBounds() == QRect(60, 60, 80, 80));
    d.undoStack()->undo();

    d.borderSelection(10);
    CHECK(d.selection().constScanLine(100)[100] == 0);    // centre is not part of the border
    CHECK(d.selection().constScanLine(100)[50] == 255);   // the original edge is
    CHECK(d.selection().constScanLine(100)[45] > 0 && d.selection().constScanLine(100)[54] > 0);  // 5 px each side
    CHECK(d.selection().constScanLine(100)[43] == 0 && d.selection().constScanLine(100)[56] == 0);
    d.undoStack()->undo();

    d.featherSelection(8);
    const int edge = d.selection().constScanLine(100)[50];
    CHECK(edge > 60 && edge < 200);                        // soft edge
    CHECK(d.selection().constScanLine(100)[100] == 255);
    d.undoStack()->undo();

    d.smoothSelection(6);
    CHECK(d.selection().constScanLine(100)[100] == 255 && d.selection().constScanLine(51)[51] < 128);
    d.undoStack()->undo();
    CHECK(d.selectionBounds() == QRect(50, 50, 100, 100));  // undo restores the original

    // Color range selects the red patch softly and nothing else
    QPainter p(&d.layer(0).image);
    p.fillRect(10, 10, 30, 30, QColor(255, 0, 0));
    p.fillRect(150, 10, 30, 30, QColor(235, 20, 20));   // near red
    p.end();
    const QImage range = Filters::colorRangeMask(d.flattened(), QColor(255, 0, 0), 60);
    CHECK(range.constScanLine(20)[20] == 255);
    CHECK(range.constScanLine(20)[160] == 255);          // within fuzziness/2
    CHECK(range.constScanLine(100)[100] == 0);           // white is far away
    CHECK(Filters::colorRangeMask(d.flattened(), QColor(255, 0, 0), 10).constScanLine(20)[160] == 0);

    // Load layer transparency
    d.addLayer();
    QPainter lp(&d.activeLayer().image);
    lp.fillRect(20, 120, 40, 30, Qt::blue);
    lp.end();
    d.selectLayerTransparency();
    CHECK(d.selectionBounds() == QRect(20, 120, 40, 30));
}


// Dodge, Burn and Smudge, driven through the canvas like a user would.
static void testRetouchTools(MainWindow &w, ToolManager *tools, ToolSettings *settings)
{
    QImage img(200, 100, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(128, 128, 128));
    QPainter p(&img);
    p.fillRect(0, 50, 200, 50, QColor(30, 30, 30));   // dark lower half
    p.end();
    w.addDocument(new Document(img));
    auto *c = qobject_cast<Canvas *>(w.findChild<QTabWidget *>()->currentWidget());
    Document *d = c->document();
    c->fitToWindow();
    settings->size = 20;
    settings->hardness = 100;
    settings->opacity = 100;

    tools->setCurrent(Tool::Dodge);
    bool exposureLabel = false;
    for (QLabel *l : w.findChildren<QLabel *>())
        exposureLabel |= l->text() == "Exposure:";
    CHECK(exposureLabel);
    settings->toneRange = 1;  // midtones
    drag(c, {20, 25}, {80, 25});
    CHECK(d->layer(0).image.pixelColor(50, 25).red() > 160);   // gray got lighter
    CHECK(d->layer(0).image.pixelColor(150, 25).red() == 128); // untouched
    d->undoStack()->undo();
    CHECK(d->layer(0).image.pixelColor(50, 25).red() == 128);

    settings->toneRange = 2;  // highlights barely affect dark pixels
    drag(c, {20, 75}, {80, 75});
    CHECK(d->layer(0).image.pixelColor(50, 75).red() <= 33);
    d->undoStack()->undo();

    tools->setCurrent(Tool::Burn);
    settings->toneRange = 1;
    drag(c, {120, 25}, {180, 25});
    CHECK(d->layer(0).image.pixelColor(150, 25).red() < 100);  // gray got darker

    // Smudge drags the dark lower half up into the gray
    tools->setCurrent(Tool::Smudge);
    settings->opacity = 80;
    const int before = d->layer(0).image.pixelColor(100, 46).red();
    drag(c, {100, 75}, {100, 40});
    const int after = d->layer(0).image.pixelColor(100, 46).red();
    std::printf("     smudge: %d -> %d\n", before, after);
    CHECK(after < before - 20);
    CHECK(d->layer(0).image.pixelColor(20, 46).red() == 128);  // away from the stroke: unchanged
    d->undoStack()->undo();
    CHECK(d->layer(0).image.pixelColor(100, 46).red() == before);

    tools->setCurrent(Tool::Brush);
    bool opacityLabel = false;
    for (QLabel *l : w.findChildren<QLabel *>())
        opacityLabel |= l->text() == "Opacity:";
    CHECK(opacityLabel);
}


// Drop shadow, outer glow and stroke, saved in projects and merged for PSD.
static void testLayerStyles()
{
    Document d(QSize(200, 200), Qt::white);
    d.addLayer();
    QPainter p(&d.activeLayer().image);
    p.fillRect(80, 80, 40, 40, Qt::black);
    p.end();
    const int layer = d.activeIndex();
    auto gray = [&](int x, int y) { return d.flattened().pixelColor(x, y).red(); };

    LayerStyle shadow;
    shadow.shadow = true;
    shadow.shadowAngle = 90;      // light from above: shadow falls straight down
    shadow.shadowDistance = 10;
    shadow.shadowSize = 4;
    shadow.shadowOpacity = 100;
    d.setLayerStyle(layer, shadow);
    CHECK(gray(100, 125) < 100);  // shadow below the square
    CHECK(gray(100, 74) == 255);  // nothing above it
    CHECK(gray(100, 100) == 0);   // the square itself is unchanged
    d.undoStack()->undo();
    CHECK(gray(100, 125) == 255);

    LayerStyle stroke;
    stroke.stroke = true;
    stroke.strokeColor = Qt::red;
    stroke.strokeSize = 4;
    d.setLayerStyle(layer, stroke);
    const QColor edge = d.flattened().pixelColor(100, 77);
    CHECK(edge.red() == 255 && edge.green() < 10);  // red ring just outside the square
    CHECK(gray(100, 70) == 255);

    LayerStyle glow;
    glow.glow = true;
    glow.glowColor = Qt::blue;
    glow.glowSize = 12;
    glow.glowOpacity = 100;
    d.setLayerStyle(layer, glow);
    const QColor g = d.flattened().pixelColor(100, 74);
    CHECK(g.blue() > g.red() + 30);                 // bluish halo near the square
    CHECK(d.flattened().pixelColor(100, 20) == QColor(Qt::white));

    // All three at once survive a save/load round-trip exactly
    LayerStyle all = shadow;
    all.stroke = true;
    all.strokeColor = Qt::red;
    all.glow = true;
    d.setLayerStyle(layer, all);
    CHECK(d.effectsMargin() > 0);
    QString err, warn;
    const QString proj = tmpPath("styles.pairpaint");
    CHECK(FileIO::saveProject(&d, proj, &err));
    Document *loaded = FileIO::load(proj, &err);
    CHECK(loaded && loaded->layer(layer).style == all);
    CHECK(loaded && loaded->flattened() == d.flattened());
    delete loaded;

    // PSD: the effects are merged into the layer so the result looks the same
    const QString psd = tmpPath("styles.psd");
    CHECK(Psd::write(&d, psd, &err, &warn));
    CHECK(warn.contains("merged"));
    Document *fromPsd = FileIO::load(psd, &err);
    CHECK(fromPsd != nullptr);
    if (fromPsd) {
        const QColor a = fromPsd->flattened().pixelColor(100, 125), b = d.flattened().pixelColor(100, 125);
        CHECK(std::abs(a.red() - b.red()) <= 2 && std::abs(a.blue() - b.blue()) <= 2);
        delete fromPsd;
    }

    // Clearing the style removes every effect
    d.setLayerStyle(layer, LayerStyle());
    CHECK(gray(100, 125) == 255 && gray(100, 77) == 255);
}


// Layer groups: compositing, structure editing, tools, Layers panel and file formats.
static void testGroups(MainWindow &w, ToolManager *tools, ToolSettings *settings)
{
    w.addDocument(new Document(QSize(200, 200), Qt::white));
    auto *c = qobject_cast<Canvas *>(w.findChild<QTabWidget *>()->currentWidget());
    Document *d = c->document();
    c->fitToWindow();
    auto px = [&](int x, int y) { return d->flattened().pixelColor(x, y); };
    auto groupCount = [](const Document *doc) {
        int n = 0;
        for (int i = 0; i < doc->layerCount(); ++i)
            n += doc->layer(i).isGroup();
        return n;
    };

    d->addLayer("Red");
    QPainter p(&d->activeLayer().image);
    p.fillRect(50, 50, 100, 100, Qt::red);
    p.end();
    const int red = d->activeIndex();
    d->groupActiveLayer();
    const int group = d->activeIndex();
    CHECK(d->layer(group).isGroup() && d->layerCount() == 4 && d->parentGroup(red + 1) == group);

    // Group opacity, visibility and mask apply to its contents
    d->setLayerOpacity(group, 0.5);
    CHECK(px(100, 100).green() > 100 && px(100, 100).red() == 255);   // pink: half red over white
    d->setLayerOpacity(group, 1.0);
    d->setLayerVisible(group, false);
    CHECK(px(100, 100) == QColor(Qt::white));
    d->setLayerVisible(group, true);
    QPainterPath left;
    left.addRect(0, 0, 100, 200);
    d->selectPath(left, SelectionOp::Replace, false, "Select");
    d->addMask(true);
    d->deselect();
    CHECK(px(75, 100) == QColor(Qt::red) && px(125, 100) == QColor(Qt::white));
    d->undoStack()->undo();
    d->undoStack()->undo();
    CHECK(d->layer(group).mask.isNull() && px(125, 100) == QColor(Qt::red));

    // An adjustment layer inside the group only affects the group's contents
    d->setActiveIndex(group);
    Adjustment inv;
    inv.type = Adjustment::Invert;
    d->addAdjustmentLayer(inv);
    CHECK(d->parentGroup(d->activeIndex()) >= 0);             // went inside the selected group
    CHECK(px(100, 100) == QColor(Qt::cyan));                   // red inverted
    CHECK(px(10, 10) == QColor(Qt::white));                    // background outside the group untouched
    d->undoStack()->undo();

    // New layers go inside a selected group; moving past its edge takes a layer out
    d->setActiveIndex(group);
    d->addLayer("Inside");
    const int inside = d->activeIndex();
    CHECK(d->parentGroup(inside) == inside + 1);
    d->setActiveIndex(1 + 1);   // "Red", the group's bottom layer (index 1 is the end marker)
    CHECK(d->activeLayer().name == "Red");
    d->moveLayer(-1);           // past the bottom edge: out of the group
    CHECK(d->activeLayer().name == "Red" && d->parentGroup(d->activeIndex()) == -1);
    d->moveLayer(1);            // and back in
    CHECK(d->parentGroup(d->activeIndex()) >= 0);
    d->undoStack()->undo();
    d->undoStack()->undo();
    d->undoStack()->undo();     // remove "Inside"

    // Painting is refused while the group itself is selected
    d->setActiveIndex(d->layerCount() - 1);
    CHECK(d->activeLayer().isGroup() && !d->canEditPixels());
    tools->setCurrent(Tool::Brush);
    settings->setForeground(Qt::blue);
    settings->opacity = 100;
    const QImage before = d->flattened();
    drag(c, {10, 180}, {190, 180});
    CHECK(d->flattened() == before);

    // The Move tool moves everything in the group
    tools->setCurrent(Tool::Move);
    drag(c, {100, 100}, {120, 100});
    CHECK(px(55, 100) == QColor(Qt::white) && px(165, 100) == QColor(Qt::red));
    d->undoStack()->undo();
    CHECK(px(55, 100) == QColor(Qt::red));

    // Layers panel shows the group as an expandable folder with the layer inside
    auto *tree = w.findChild<QTreeWidget *>();
    QTest::qWait(20);
    QTreeWidgetItem *top = tree ? tree->topLevelItem(0) : nullptr;
    CHECK(top && top->text(0).startsWith("Group") && top->childCount() == 1 && top->child(0)->text(0) == "Red");

    // Save / load keeps the structure
    QString err, warn;
    const QString proj = tmpPath("groups.pairpaint");
    CHECK(FileIO::saveProject(d, proj, &err));
    Document *loaded = FileIO::load(proj, &err);
    CHECK(loaded && groupCount(loaded) == 1 && loaded->layerCount() == d->layerCount());
    CHECK(loaded && loaded->flattened() == d->flattened());
    delete loaded;
    const QString psd = tmpPath("groups.psd");
    d->setLayerOpacity(d->layerCount() - 1, 0.6);
    CHECK(Psd::write(d, psd, &err, &warn));
    Document *fromPsd = FileIO::load(psd, &err);
    CHECK(fromPsd && groupCount(fromPsd) == 1 && fromPsd->layerCount() == d->layerCount());
    if (fromPsd) {
        const QColor a = fromPsd->flattened().pixelColor(100, 100), b = d->flattened().pixelColor(100, 100);
        CHECK(std::abs(a.green() - b.green()) <= 2);   // group opacity survived
        delete fromPsd;
    }

    // Duplicate, delete, merge and ungroup
    const int n = d->layerCount();
    d->duplicateLayer();
    CHECK(d->layerCount() == n + 3 && groupCount(d) == 2);
    d->deleteLayer();
    CHECK(d->layerCount() == n && groupCount(d) == 1);
    const QImage look = d->flattened();
    d->mergeDown();   // merge group
    CHECK(groupCount(d) == 0 && d->layerCount() == 2 && d->flattened() == look);
    d->undoStack()->undo();
    d->ungroup();
    CHECK(groupCount(d) == 0 && d->layerCount() == 2 && d->activeLayer().name == "Red");
}


// Photoshop compatibility: each file was saved by Photoshop, which also stored its own
// rendering of the image. PairPaint's rendering of the layers must match it.
static void testPhotoshopFiles()
{
    const QDir dir(QStringLiteral(PAIRPAINT_TEST_DATA "/psd-tools"));
    const QStringList files = dir.entryList({QStringLiteral("*.psd")});
    CHECK(files.size() >= 20);
    auto overWhite = [](const QImage &img) {
        QImage out(img.size(), QImage::Format_RGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        p.drawImage(0, 0, img);
        return out;
    };
    for (const QString &name : files) {
        QString err;
        Document *d = FileIO::load(dir.filePath(name), &err);
        const QImage ref = Psd::readComposite(dir.filePath(name), &err);
        if (!d || ref.isNull()) {
            std::printf("FAIL could not read %s: %s\n", qPrintable(name), qPrintable(err));
            ++fails;
            continue;
        }
        const QImage a = overWhite(d->flattened()), b = overWhite(ref);
        long differing = 0;
        for (int y = 0; y < a.height(); ++y) {
            const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x)
                if (std::max({std::abs(qRed(pa[x]) - qRed(pb[x])), std::abs(qGreen(pa[x]) - qGreen(pb[x])),
                              std::abs(qBlue(pa[x]) - qBlue(pb[x]))}) > 12)
                    ++differing;
        }
        const double percent = 100.0 * differing / (double(a.width()) * a.height());
        std::printf("%s  %-45s %.2f%% of pixels differ from Photoshop\n", percent < 1.5 ? "ok  " : "FAIL",
                    qPrintable(name), percent);
        if (percent >= 1.5)
            ++fails;
        delete d;
    }
}

// Blend modes, Fill opacity and clipping masks through the document API.
static void testBlendingAndClipping()
{
    Document d(QSize(40, 40), QColor(128, 128, 128));
    d.addLayer("Top");
    d.activeLayer().image.fill(QColor(200, 100, 50));
    const int top = d.activeIndex();
    auto px = [&] { return d.flattened().pixelColor(20, 20); };

    d.setLayerMode(top, Blend::LinearBurn);
    CHECK(px() == QColor(73, 0, 0));          // 128 + 200 - 255 = 73, others clamp to 0
    d.setLayerMode(top, Blend::Subtract);
    CHECK(px() == QColor(0, 28, 78));         // 128 - top
    d.setLayerMode(top, Blend::Luminosity);
    const QColor lum = px();
    CHECK(lum.red() == lum.green() && lum.green() == lum.blue());  // gray base keeps no hue
    d.setLayerMode(top, QPainter::CompositionMode_SourceOver);

    // Fill fades the pixels but not the layer style
    d.setLayerFillOpacity(top, 0.0);
    CHECK(px() == QColor(128, 128, 128));
    d.undoStack()->undo();
    CHECK(px() == QColor(200, 100, 50));

    // Clipping mask: the clipped layer only shows where the layer below has pixels
    d.activeLayer().image.fill(Qt::transparent);
    QPainter p(&d.activeLayer().image);
    p.fillRect(0, 0, 20, 40, QColor(200, 100, 50));   // left half only
    p.end();
    d.addLayer("Clipped");
    d.activeLayer().image.fill(Qt::blue);
    const int clipped = d.activeIndex();
    CHECK(d.flattened().pixelColor(30, 20) == QColor(Qt::blue));   // not clipped yet: covers everything
    d.setLayerClipped(clipped, true);
    CHECK(d.flattened().pixelColor(30, 20) == QColor(128, 128, 128));  // right half: base is empty
    CHECK(d.flattened().pixelColor(10, 20) == QColor(Qt::blue));       // left half: inside the base
    d.setLayerOpacity(top, 0.5);                                         // base opacity applies to the group
    CHECK(d.flattened().pixelColor(10, 20).blue() < 200);
    d.setLayerOpacity(top, 1.0);
    d.mergeDown();                                                       // merging keeps the clipping
    CHECK(d.flattened().pixelColor(30, 20) == QColor(128, 128, 128) && d.flattened().pixelColor(10, 20) == QColor(Qt::blue));
    d.undoStack()->undo();

    // Saved and reloaded in both formats
    QString err, warn;
    d.setLayerFillOpacity(clipped, 0.4);
    CHECK(FileIO::saveProject(&d, tmpPath("clip.pairpaint"), &err));
    Document *loaded = FileIO::load(tmpPath("clip.pairpaint"), &err);
    CHECK(loaded && loaded->layer(clipped).clipped && qFuzzyCompare(loaded->layer(clipped).fillOpacity, 0.4));
    delete loaded;
    d.setLayerMode(top, Blend::VividLight);
    CHECK(Psd::write(&d, tmpPath("clip.psd"), &err, &warn));
    Document *fromPsd = FileIO::load(tmpPath("clip.psd"), &err);
    CHECK(fromPsd && fromPsd->layer(clipped).clipped && std::abs(fromPsd->layer(clipped).fillOpacity - 0.4) < 0.01
          && fromPsd->layer(top).mode == Blend::VividLight);
    if (fromPsd)
        CHECK(fromPsd->flattened() == d.flattened());
    delete fromPsd;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);  // keep the user's real settings untouched
    QApplication::setOrganizationName("PairPaintTests");
    QTemporaryDir dir;
    outputDir = &dir;
    app.setStyle(QStyleFactory::create("Fusion"));
    { QPalette p; QColor win(50,50,50), base(35,35,35), text(225,225,225);
      p.setColor(QPalette::Window, win); p.setColor(QPalette::WindowText, text); p.setColor(QPalette::Base, base);
      p.setColor(QPalette::Text, text); p.setColor(QPalette::Button, win); p.setColor(QPalette::ButtonText, text);
      p.setColor(QPalette::Highlight, QColor(42,130,218)); p.setColor(QPalette::HighlightedText, Qt::white); app.setPalette(p); }
    MainWindow w; w.resize(1400, 900); w.show();
    QTest::qWait(50);
    auto *tools = w.findChild<ToolManager *>();
    auto *settings = w.findChild<ToolSettings *>();
    w.addDocument(new Document(QSize(800, 600), Qt::white));
    QTest::qWait(50);
    auto *c = w.findChild<Canvas *>();
    Document *d = c->document();
    c->fitToWindow(false);

    // Brush
    tools->setCurrent(Tool::Brush);
    settings->setForeground(Qt::red); settings->size = 30;
    drag(c, {100, 100}, {400, 100});
    CHECK(px(d, 250, 100) == QColor(Qt::red));
    CHECK(px(d, 250, 200) == QColor(Qt::white));
    CHECK(d->undoStack()->count() == 1);

    // Undo / redo
    d->undoStack()->undo();
    CHECK(px(d, 250, 100) == QColor(Qt::white));
    d->undoStack()->redo();
    CHECK(px(d, 250, 100) == QColor(Qt::red));

    // Rectangle selection + fill with foreground (blue), only inside selection
    tools->setCurrent(Tool::RectSelect);
    drag(c, {500, 300}, {600, 400});
    CHECK(d->hasSelection());
    CHECK(d->selectionBounds() == QRect(500, 300, 100, 100));
    settings->setForeground(Qt::blue);
    action(&w, "Fill with Foreground")->trigger();
    CHECK(px(d, 550, 350) == QColor(Qt::blue));
    CHECK(px(d, 450, 350) == QColor(Qt::white));

    // Brush clipped to selection
    tools->setCurrent(Tool::Brush); settings->setForeground(Qt::green);
    drag(c, {450, 350}, {650, 350});
    CHECK(px(d, 470, 350) == QColor(Qt::white));
    CHECK(px(d, 550, 350) == QColor(Qt::green));

    // Move selected pixels
    tools->setCurrent(Tool::Move);
    drag(c, {550, 350}, {550, 450});
    CHECK(d->selectionBounds() == QRect(500, 400, 100, 100));
    CHECK(px(d, 550, 450).alpha() > 0 && px(d, 550, 450) != QColor(Qt::white));
    CHECK(px(d, 550, 320).alpha() == 0);   // hole left behind

    // Deselect, new layer, gradient, blend mode
    action(&w, "Deselect")->trigger();
    CHECK(!d->hasSelection());
    action(&w, "New Layer")->trigger();
    CHECK(d->layerCount() == 2 && d->activeIndex() == 1);
    tools->setCurrent(Tool::Gradient); settings->resetColors();
    drag(c, {0, 0}, {800, 0});
    CHECK(px(d, 1, 10).red() < 10 && px(d, 798, 10).red() > 245);
    d->setLayerMode(1, QPainter::CompositionMode_Multiply);
    QImage flat = d->flattened();
    CHECK(flat.pixelColor(250, 100).green() < 5); // red * gray -> no green

    // Magic wand on layer 0 picks red stroke
    d->setActiveIndex(0);
    tools->setCurrent(Tool::MagicWand); settings->tolerance = 10;
    QPointF wp = c->mapFromImage({250, 100});
    mouse(c, QEvent::MouseButtonPress, wp, Qt::LeftButton, Qt::LeftButton);
    mouse(c, QEvent::MouseButtonRelease, wp, Qt::LeftButton, Qt::NoButton);
    CHECK(d->hasSelection());
    QRect sb = d->selectionBounds();
    std::printf("     wand bounds %d,%d %dx%d\n", sb.x(), sb.y(), sb.width(), sb.height());
    CHECK(sb.left() >= 80 && sb.left() <= 90 && sb.right() >= 410 && sb.right() <= 420);

    // Filters
    d->applyToActive("Blur", [](const QImage &i) { return Filters::gaussianBlur(i, 8); });
    CHECK(px(d, 250, 130).red() > 200 && px(d, 250, 130).green() > 200); // outside selection unchanged-ish (white)
    action(&w, "Deselect")->trigger();
    QImage inv = Filters::invert(d->layer(0).image);
    CHECK(inv.pixelColor(700, 50) == QColor(Qt::black));
    QImage pix = Filters::pixelate(d->layer(0).image, 16);
    CHECK(pix.size() == QSize(800, 600));
    QImage hs = Filters::hueSaturation(d->layer(0).image, 120, 0, 0);
    CHECK(hs.pixelColor(250, 100).green() > 200);
    CHECK(Filters::unsharpMask(d->layer(0).image, 100, 2, 0).size() == QSize(800, 600));
    CHECK(Filters::brightnessContrast(d->layer(0).image, 50, 20).size() == QSize(800,600));

    // Shapes, text layer
    tools->setCurrent(Tool::RectShape); settings->fillShape = true; settings->setForeground(Qt::yellow);
    drag(c, {650, 50}, {750, 150});
    CHECK(px(d, 700, 100) == QColor(Qt::yellow));
    tools->setCurrent(Tool::EllipseShape); settings->fillShape = false; settings->size = 6; settings->setForeground(Qt::magenta);
    drag(c, {50, 400}, {250, 550});
    tools->setCurrent(Tool::LineShape); settings->setForeground(Qt::darkCyan);
    drag(c, {300, 550}, {450, 250});
    tools->setCurrent(Tool::Eraser); settings->size = 40;
    drag(c, {100, 250}, {300, 250});
    CHECK(px(d, 200, 250).alpha() == 0);

    // Image ops
    d->rotate(90);
    CHECK(d->size() == QSize(600, 800));
    d->undoStack()->undo();
    CHECK(d->size() == QSize(800, 600));
    d->crop(QRect(10, 10, 700, 500));
    CHECK(d->size() == QSize(700, 500));
    d->undoStack()->undo();
    d->resizeCanvas(QSize(900, 700), QPoint(50, 50));
    CHECK(d->size() == QSize(900, 700));
    d->undoStack()->undo();
    d->flip(Qt::Horizontal); d->undoStack()->undo();

    // Eyedropper
    tools->setCurrent(Tool::Eyedropper);
    QPointF ep = c->mapFromImage({700.5, 100.5});
    mouse(c, QEvent::MouseButtonPress, ep, Qt::LeftButton, Qt::LeftButton);
    mouse(c, QEvent::MouseButtonRelease, ep, Qt::LeftButton, Qt::NoButton);
    std::printf("     picked %s\n", qPrintable(settings->foreground().name()));

    // Paint bucket on new layer
    d->setActiveIndex(1);
    action(&w, "New Layer")->trigger();
    tools->setCurrent(Tool::Fill); settings->setForeground(QColor(0, 128, 255)); settings->sampleMerged = false;
    QPointF fp = c->mapFromImage({10, 590});
    mouse(c, QEvent::MouseButtonPress, fp, Qt::LeftButton, Qt::LeftButton);
    mouse(c, QEvent::MouseButtonRelease, fp, Qt::LeftButton, Qt::NoButton);
    CHECK(px(d, 400, 300) == QColor(0, 128, 255)); // empty layer -> fills all
    d->setLayerOpacity(2, 0.25);
    d->setLayerMode(2, QPainter::CompositionMode_Overlay);
    CHECK(d->layerCount() == 3);

    // Save / load roundtrip
    const QString proj = tmpPath("test.pairpaint");
    QString err;
    CHECK(FileIO::saveProject(d, proj, &err));
    Document *loaded = FileIO::load(proj, &err);
    CHECK(loaded && loaded->layerCount() == 3);
    CHECK(loaded && loaded->flattened() == d->flattened());
    CHECK(loaded && loaded->layer(2).mode == QPainter::CompositionMode_Overlay && qFuzzyCompare(loaded->layer(2).opacity, 0.25));
    delete loaded;
    CHECK(FileIO::exportImage(d, tmpPath("test.jpg"), &err));
    CHECK(FileIO::exportImage(d, tmpPath("test.png"), &err));
    Document *png = FileIO::load(tmpPath("test.png"), &err);
    CHECK(png && png->size() == QSize(800, 600));
    if (png) w.addDocument(png);
    QTest::qWait(30);

    auto key = [&](int k) { QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier); QApplication::sendEvent(c, &e); };

    // ---- Layer masks
    w.findChild<QTabWidget *>()->setCurrentIndex(0);
    d->setLayerOpacity(2, 1.0);
    d->setLayerMode(2, QPainter::CompositionMode_SourceOver);
    d->setActiveIndex(2);
    tools->setCurrent(Tool::RectSelect);
    drag(c, {0, 0}, {400, 600});
    d->addMask(true);
    CHECK(!d->activeLayer().mask.isNull() && d->editingMask());
    action(&w, "Deselect")->trigger();
    QImage f1 = d->flattened();
    CHECK(f1.pixelColor(100, 300) == QColor(0, 128, 255));   // revealed
    CHECK(f1.pixelColor(700, 300) != QColor(0, 128, 255));   // hidden by mask
    CHECK(d->activeLayer().image.pixelColor(700, 300) == QColor(0, 128, 255)); // pixels untouched
    // paint black on the mask hides more
    tools->setCurrent(Tool::Brush); settings->setForeground(Qt::black); settings->size = 40; settings->hardness = 100; settings->opacity = 100;
    drag(c, {50, 100}, {350, 100});
    CHECK(d->flattened().pixelColor(200, 100) != QColor(0, 128, 255));
    CHECK(d->activeLayer().image.pixelColor(200, 100) == QColor(0, 128, 255));
    d->setMaskEnabled(false);
    CHECK(d->flattened().pixelColor(700, 300) == QColor(0, 128, 255));
    d->setMaskEnabled(true);
    d->applyMask();
    CHECK(d->activeLayer().mask.isNull() && d->activeLayer().image.pixelColor(700, 300).alpha() == 0);
    d->undoStack()->undo();
    CHECK(!d->activeLayer().mask.isNull());

    // ---- Adjustment layers
    d->setLayerVisible(2, false);
    QColor beforeAdj = d->flattened().pixelColor(700, 100);
    Adjustment invAdj; invAdj.type = Adjustment::Invert;
    d->addAdjustmentLayer(invAdj);
    CHECK(d->activeLayer().isAdjustment() && d->editingMask());
    QColor afterAdj = d->flattened().pixelColor(700, 100);
    CHECK(afterAdj.red() == 255 - beforeAdj.red() && afterAdj.blue() == 255 - beforeAdj.blue());
    // painting on an adjustment layer edits its mask
    tools->setCurrent(Tool::RectSelect); drag(c, {600, 0}, {800, 600});
    d->fillSelected(Qt::black); action(&w, "Deselect")->trigger();
    CHECK(d->flattened().pixelColor(700, 100) == beforeAdj);
    for (int i = 0; i < 4; ++i) d->undoStack()->undo();   // deselect, fill, select, invert layer
    CHECK(!d->hasSelection() && !d->activeLayer().isAdjustment());
    Adjustment curves; curves.type = Adjustment::Curves; curves.params = {0, 0, 128, 200, 255, 255};
    d->addAdjustmentLayer(curves);
    CHECK(d->flattened().pixelColor(700, 100).red() >= beforeAdj.red());
    Adjustment levels; levels.type = Adjustment::Levels; levels.params = {50, 200, 100, 0, 255};
    d->addAdjustmentLayer(levels);
    int adjCount = 0; for (int i = 0; i < d->layerCount(); ++i) adjCount += d->layer(i).isAdjustment();
    CHECK(adjCount == 2);
    d->setLayerVisible(2, true);

    // ---- Curves / levels / healing filters directly
    QList<int> lut = Filters::curveLut({0, 0, 128, 200, 255, 255});
    CHECK(lut[0] == 0 && lut[255] == 255 && lut[128] == 200 && lut[64] > 64);
    bool monotone = true; for (int i = 1; i < 256; ++i) monotone &= lut[i] >= lut[i-1];
    CHECK(monotone);
    QImage gray(10, 1, QImage::Format_ARGB32); gray.fill(QColor(100, 100, 100));
    CHECK(Filters::levels(gray, 100, 200, 1.0, 0, 255).pixelColor(0, 0).red() == 0);

    // ---- Text layers
    TextData td; td.text = "Hello"; td.font = QFont("Sans Serif"); td.font.setPixelSize(60); td.color = Qt::red; td.pos = {100, 400};
    d->addTextLayer(td);
    int textLayer = d->activeIndex();
    CHECK(d->activeLayer().isText() && alphaBounds(d->activeLayer().image).intersects(QRect(100, 400, 200, 80)));
    td.text = "Hello world"; d->setText(textLayer, td);
    CHECK(d->activeLayer().name == "Hello world");
    tools->setCurrent(Tool::Move);
    drag(c, {150, 430}, {150, 480});
    CHECK(d->activeLayer().isText() && qFuzzyCompare(d->activeLayer().text.pos.y(), 450.0));
    tools->setCurrent(Tool::Brush);
    drag(c, {120, 470}, {130, 470});
    CHECK(!d->activeLayer().isText());   // painting rasterizes
    d->undoStack()->undo(); d->undoStack()->undo();
    CHECK(d->activeLayer().isText());

    // ---- Free transform: scale layer content 2x from the top-left handle region
    d->addLayer();
    tools->setCurrent(Tool::RectShape); settings->fillShape = true; settings->setForeground(Qt::green);
    drag(c, {300, 200}, {400, 300});
    QRect b0 = alphaBounds(d->activeLayer().image);
    tools->setCurrent(Tool::Transform);
    c->activateTool();
    const QPointF br(b0.right() + 1, b0.bottom() + 1);
    drag(c, br, br + QPointF(b0.width(), b0.height()));   // bottom-right handle doubles the size
    key(Qt::Key_Return);
    QRect b1 = alphaBounds(d->activeLayer().image);
    std::printf("     transform %dx%d -> %dx%d\n", b0.width(), b0.height(), b1.width(), b1.height());
    CHECK(std::abs(b1.width() - 2 * b0.width()) <= 2 && std::abs(b1.height() - 2 * b0.height()) <= 2 && b1.topLeft() == b0.topLeft());
    CHECK(d->undoStack()->undoText() == "Free Transform");
    // rotate 45 degrees by dragging outside the box
    c->activateTool();
    drag(c, {650, 300}, {650, 650});
    key(Qt::Key_Escape);
    CHECK(alphaBounds(d->activeLayer().image) == b1);   // Esc reverted
    tools->setCurrent(Tool::Move);

    // ---- Clone stamp
    tools->setCurrent(Tool::CloneStamp); settings->size = 30; settings->hardness = 100; settings->sampleMerged = false;
    QPointF sp = c->mapFromImage({350, 250});
    mouse(c, QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    mouse(c, QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    drag(c, {650, 100}, {660, 100});
    CHECK(d->activeLayer().image.pixelColor(655, 100) == QColor(Qt::green));

    // ---- Healing brush on a flat gray image with a dark blemish
    {
        Document hd(QSize(200, 200), QColor(150, 150, 150));
        QPainter hp(&hd.layer(0).image);
        hp.fillRect(0, 0, 100, 200, QColor(120, 120, 120)); // source area: darker but uniform
        hp.setBrush(Qt::black); hp.setPen(Qt::NoPen); hp.drawEllipse(QPointF(150, 100), 5, 5);
        hp.end();
        w.addDocument(new Document(hd.layer(0).image));
        auto *hc = qobject_cast<Canvas *>(w.findChild<QTabWidget *>()->currentWidget());
        Document *hdoc = hc->document();
        hc->fitToWindow();
        tools->setCurrent(Tool::Healing); settings->size = 24; settings->hardness = 50;
        QPointF hs = hc->mapFromImage({50, 100});
        mouse(hc, QEvent::MouseButtonPress, hs, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        mouse(hc, QEvent::MouseButtonRelease, hs, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        drag(hc, {148, 100}, {152, 100});
        QColor healed = hdoc->layer(0).image.pixelColor(150, 100);
        std::printf("     healed pixel %d (clone would be 120, surroundings 150)\n", healed.red());
        CHECK(std::abs(healed.red() - 150) <= 12);
        tools->setCurrent(Tool::Brush);
        w.findChild<QTabWidget *>()->setCurrentIndex(0);
    }

    // ---- Filter dialog on a mask target + destructive curves via FilterDialog API
    d->setActiveIndex(textLayer);

    // ---- Save v2 project + PSD round-trip
    const QString proj2 = tmpPath("test2.pairpaint");
    CHECK(FileIO::saveProject(d, proj2, &err));
    Document *l2 = FileIO::load(proj2, &err);
    CHECK(l2 && l2->layerCount() == d->layerCount());
    CHECK(l2 && l2->flattened() == d->flattened());
    bool textOk = false, adjOk = false, maskOk = false;
    for (int i = 0; l2 && i < l2->layerCount(); ++i) {
        textOk |= l2->layer(i).isText() && l2->layer(i).text.text == "Hello world";
        adjOk |= l2->layer(i).adjustment.type == Adjustment::Curves && l2->layer(i).adjustment.params.size() == 6;
        maskOk |= !l2->layer(i).mask.isNull() && !l2->layer(i).isAdjustment();
    }
    CHECK(textOk && adjOk && maskOk);
    delete l2;

    QString warn;
    const QString psd = tmpPath("test.psd");
    CHECK(Psd::write(d, psd, &err, &warn));
    std::printf("     psd warning: %s\n", qPrintable(warn));
    Document *pd = FileIO::load(psd, &err);
    CHECK(pd != nullptr);
    if (!pd) std::printf("     psd error: %s\n", qPrintable(err));
    if (pd) {
        CHECK(pd->layerCount() == d->layerCount() - adjCount);
        bool psdMask = false; for (int i = 0; i < pd->layerCount(); ++i) psdMask |= !pd->layer(i).mask.isNull();
        CHECK(psdMask);
        CHECK(pd->layer(pd->layerCount() - 1).name == d->layer(d->layerCount() - 1).name);
        // compare a pixel-layer losslessly (layer 1: gradient, multiply)
        CHECK(pd->layer(1).mode == QPainter::CompositionMode_Multiply);
        QImage a = pd->layer(1).image.convertToFormat(QImage::Format_ARGB32), b = d->layer(1).image.convertToFormat(QImage::Format_ARGB32);
        CHECK(a == b);
        delete pd;
    }

    // Screenshot of first document with a selection showing
    w.findChild<QTabWidget *>()->setCurrentIndex(0);
    d->setLayerOpacity(2, 0.0);
    tools->setCurrent(Tool::EllipseSelect);
    drag(c, {450, 200}, {700, 380});
    tools->setCurrent(Tool::Brush);
    QTest::qWait(100);
    d->setLayerOpacity(2, 1.0);
    d->setActiveIndex(2);
    QTest::qWait(150);
    {
        // Curves dialog editing the curves adjustment layer, with a cancel that must restore params
        int ci = -1; for (int i = 0; i < d->layerCount(); ++i) if (d->layer(i).adjustment.type == Adjustment::Curves) ci = i;
        QList<int> orig = d->layer(ci).adjustment.params;
        auto *ed = new CurvesEditor(orig, Filters::histogram(d->flattened()));
        FilterDialog dlg("Edit Curves", ed, adjustmentLayerTarget(d, ci, "Edit Curves", nullptr), &w);
        dlg.show(); QTest::qWait(150);
        QPointF g(ed->width() / 2.0, ed->height() * 0.75);
        QMouseEvent pe(QEvent::MouseButtonPress, g, ed->mapToGlobal(g), Qt::LeftButton, Qt::LeftButton, {}); QApplication::sendEvent(ed, &pe);
        QPointF g2(g.x(), ed->height() * 0.2);
        QMouseEvent me(QEvent::MouseMove, g2, ed->mapToGlobal(g2), Qt::NoButton, Qt::LeftButton, {}); QApplication::sendEvent(ed, &me);
        QTest::qWait(150);
        CHECK(d->layer(ci).adjustment.params != orig);  // live preview applied
        QMouseEvent re(QEvent::MouseButtonRelease, g2, ed->mapToGlobal(g2), Qt::LeftButton, Qt::NoButton, {}); QApplication::sendEvent(ed, &re);
        dlg.reject();
        CHECK(d->layer(ci).adjustment.params == orig);  // cancel restored
        // creating a new adjustment layer then cancelling removes it
        const int before = d->layerCount();
        DocState st = d->state();
        Adjustment hs; hs.type = Adjustment::HueSaturation; hs.params = Adjustments::defaults(hs.type);
        d->addAdjustmentLayer(hs, false);
        CHECK(d->layerCount() == before + 1);
        FilterDialog dlg2("New", new SliderEditor(Adjustments::params(hs.type)), adjustmentLayerTarget(d, d->activeIndex(), "New", &st), &w);
        dlg2.show(); QTest::qWait(50); dlg2.reject();
        CHECK(d->layerCount() == before);
    }

    testSelections();
    testRetouchTools(w, tools, settings);
    testLayerStyles();
    testGroups(w, tools, settings);
    testBlendingAndClipping();
    testPhotoshopFiles();
    {
        // Regression: destroying a window with unsaved changes used to crash.
        auto *other = new MainWindow;
        auto *doc = new Document(QSize(50, 50), Qt::white);
        other->addDocument(doc);
        doc->addLayer();
        CHECK(doc->isModified());
        delete other;
        CHECK(true);
    }

    std::printf("\n%d failure(s)\n", fails);
    d->undoStack()->setClean();
    for (auto *cc : w.findChildren<Canvas *>()) cc->document()->undoStack()->setClean();
    return fails ? 1 : 0;
}
