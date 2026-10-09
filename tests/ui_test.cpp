// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors
//
// End-to-end test of PairPaint: drives the real main window, canvas and tools
// with synthetic mouse/keyboard events (offscreen) and checks the resulting pixels.

#include "Adjustments.h"
#include "Canvas.h"
#include "Document.h"
#include "FileIO.h"
#include "Heif.h"
#include "FilterDialog.h"
#include "Filters.h"
#include "MainWindow.h"
#include "Psd.h"
#include "ToolSettings.h"
#include "Tools.h"

#include <QAction>
#include <QApplication>
#include <QColorSpace>
#include <QColorTransform>
#include <QDir>
#include <QElapsedTimer>
#include <limits>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainterPath>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <QTreeWidget>
#include <cstdio>
#include <cstring>
#include <functional>

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


// Typing text directly on the canvas with the Text tool.
static void testOnCanvasText(MainWindow &w, ToolManager *tools, ToolSettings *settings)
{
    w.addDocument(new Document(QSize(300, 200), Qt::white));
    auto *c = qobject_cast<Canvas *>(w.findChild<QTabWidget *>()->currentWidget());
    Document *d = c->document();
    c->fitToWindow();
    c->setFocus();
    auto click = [&](QPointF imagePos, Qt::KeyboardModifiers mods = {}) {
        const QPointF wp = c->mapFromImage(imagePos);
        mouse(c, QEvent::MouseButtonPress, wp, Qt::LeftButton, Qt::LeftButton, mods);
        mouse(c, QEvent::MouseButtonRelease, wp, Qt::LeftButton, Qt::NoButton, mods);
    };
    auto shortcutTaken = [&](int key, const QString &text) {
        QKeyEvent ov(QEvent::ShortcutOverride, key, Qt::NoModifier, text);
        ov.ignore();
        QApplication::sendEvent(c, &ov);
        return ov.isAccepted();
    };
    QFont font(QStringLiteral("Sans Serif"));
    font.setPixelSize(30);
    settings->setTextStyle(font, true);
    settings->setForeground(Qt::red);
    tools->setCurrent(Tool::Text);
    CHECK(!shortcutTaken(Qt::Key_B, "b"));   // not typing: B still means Brush

    const int undoBefore = d->undoStack()->count();
    click({40, 60});
    CHECK(d->layerCount() == 2 && tools->current()->capturesKeyboard());
    CHECK(shortcutTaken(Qt::Key_B, "b"));    // typing: letters go to the text, not to shortcuts
    QTest::keyClicks(c, "Hello");
    CHECK(tools->currentId() == Tool::Text);  // "H" did not switch to the Hand tool
    CHECK(d->activeLayer().text.text == "Hello");
    CHECK(alphaBounds(d->activeLayer().image).isValid());
    CHECK(d->activeLayer().image.pixelColor(alphaBounds(d->activeLayer().image).center()).alpha() >= 0);

    QTest::keyClick(c, Qt::Key_Home, Qt::ShiftModifier);  // select the line...
    QTest::keyClicks(c, "Bye");                            // ...and replace it
    CHECK(d->activeLayer().text.text == "Bye");
    QTest::keyClick(c, Qt::Key_Return);
    QTest::keyClicks(c, "Z");
    CHECK(d->activeLayer().text.text == "Bye\nZ");
    QTest::keyClick(c, Qt::Key_Backspace);
    QTest::keyClick(c, Qt::Key_Backspace);
    CHECK(d->activeLayer().text.text == "Bye");
    QTest::keyClick(c, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(c, Qt::Key_X, Qt::ControlModifier);
    CHECK(d->activeLayer().text.text.isEmpty());
    QTest::keyClick(c, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClick(c, Qt::Key_Left);
    QTest::keyClicks(c, "!");
    CHECK(d->activeLayer().text.text == "By!e");
    QTest::keyClick(c, Qt::Key_Backspace);
    QTest::keyClick(c, Qt::Key_End);
    QTest::keyClicks(c, "!");
    CHECK(d->activeLayer().text.text == "Bye!");
    CHECK(d->activeLayer().text.color == QColor(Qt::red));

    // Esc finishes: one undo step that removes the whole text layer
    QTest::keyClick(c, Qt::Key_Escape);
    CHECK(!tools->current()->capturesKeyboard());
    CHECK(d->undoStack()->count() == undoBefore + 1 && d->undoStack()->undoText() == "Text");
    CHECK(d->activeLayer().isText() && d->activeLayer().name == "Bye!");
    d->undoStack()->undo();
    CHECK(d->layerCount() == 1);
    d->undoStack()->redo();
    CHECK(d->layerCount() == 2 && d->activeLayer().text.text == "Bye!");

    // Click on the text to edit it in place; the options bar applies live
    const QRectF bounds = textBounds(d->activeLayer().text);
    click(QPointF(bounds.right() - 1, bounds.center().y()));
    CHECK(tools->current()->capturesKeyboard() && d->layerCount() == 2);
    QTest::keyClick(c, Qt::Key_End);
    QTest::keyClicks(c, "?");
    font.setPixelSize(60);
    settings->font = font;
    tools->current()->settingsChanged();
    CHECK(textBounds(d->activeLayer().text).height() > bounds.height() * 1.5);
    settings->setForeground(Qt::blue);
    CHECK(d->activeLayer().text.color == QColor(Qt::blue));
    tools->setCurrent(Tool::Move);              // switching tools also finishes
    CHECK(d->undoStack()->undoText() == "Edit Text" && d->activeLayer().text.text == "Bye!?");
    d->undoStack()->undo();
    CHECK(d->activeLayer().text.text == "Bye!" && d->activeLayer().text.color == QColor(Qt::red));

    // Clicking empty space and leaving without typing adds nothing
    tools->setCurrent(Tool::Text);
    const int layers = d->layerCount(), steps = d->undoStack()->count();
    click({200, 160});
    QTest::keyClick(c, Qt::Key_Escape);
    CHECK(d->layerCount() == layers && d->undoStack()->count() == steps);
    tools->setCurrent(Tool::Brush);
}


// Spot Healing, Polygonal Lasso, Crop ratio/straighten, Blur, Sharpen and Sponge.
static void testToolPack(MainWindow &w, ToolManager *tools, ToolSettings *settings)
{
    auto newDoc = [&](const QImage &img) {
        w.addDocument(new Document(img));
        auto *c = qobject_cast<Canvas *>(w.findChild<QTabWidget *>()->currentWidget());
        c->fitToWindow();
        return c;
    };
    auto click = [](Canvas *c, QPointF imagePos) {
        const QPointF wp = c->mapFromImage(imagePos);
        mouse(c, QEvent::MouseButtonPress, wp, Qt::LeftButton, Qt::LeftButton);
        mouse(c, QEvent::MouseButtonRelease, wp, Qt::LeftButton, Qt::NoButton);
    };
    auto key = [](Canvas *c, int k) { QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier); QApplication::sendEvent(c, &e); };
    settings->hardness = 100;
    settings->opacity = 100;
    settings->pressureSize = false;

    // --- Spot Healing: a dark spot on a gradient vanishes without choosing a source
    {
        QImage img(240, 120, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < img.width(); ++x)
            for (int y = 0; y < img.height(); ++y)
                img.setPixelColor(x, y, QColor(60 + x / 2, 60 + x / 2, 60 + x / 2));
        QPainter p(&img);
        p.setBrush(Qt::black);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(120, 60), 6, 6);
        p.end();
        Canvas *c = newDoc(img);
        Document *d = c->document();
        tools->setCurrent(Tool::SpotHealing);
        settings->size = 24;
        drag(c, {118, 60}, {122, 60});
        const int expected = 60 + 120 / 2, got = d->layer(0).image.pixelColor(120, 60).red();
        std::printf("     spot heal: centre %d (background there: %d)\n", got, expected);
        CHECK(std::abs(got - expected) <= 12);
        CHECK(d->layer(0).image.pixelColor(20, 60).red() == 70);   // untouched elsewhere
        CHECK(d->undoStack()->undoText() == "Spot Healing Brush");
    }

    // --- Polygonal Lasso
    {
        QImage img(200, 120, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        Canvas *c = newDoc(img);
        Document *d = c->document();
        tools->setCurrent(Tool::PolyLasso);
        click(c, {20, 20});
        click(c, {150, 20});
        click(c, {80, 50});
        key(c, Qt::Key_Backspace);                  // remove the last point
        click(c, {150, 100});
        CHECK(!d->hasSelection());                  // still open
        click(c, {20, 20});                         // clicking the first point closes it
        CHECK(d->hasSelection());
        const QRect b = d->selectionBounds();
        CHECK(std::abs(b.left() - 20) <= 1 && std::abs(b.right() - 150) <= 1 && std::abs(b.bottom() - 100) <= 1);
        CHECK(d->selection().constScanLine(30)[140] > 128 && d->selection().constScanLine(90)[30] < 128);  // a triangle
        d->deselect();
        click(c, {10, 10});
        click(c, {60, 10});
        const QPointF wp = c->mapFromImage({60, 60});   // double-click closes too
        mouse(c, QEvent::MouseButtonPress, wp, Qt::LeftButton, Qt::LeftButton);
        mouse(c, QEvent::MouseButtonRelease, wp, Qt::LeftButton, Qt::NoButton);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, wp, c->mapToGlobal(wp), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(c, &dbl);
        mouse(c, QEvent::MouseButtonRelease, wp, Qt::LeftButton, Qt::NoButton);
        CHECK(d->hasSelection() && d->selectionBounds().width() > 45 && d->selectionBounds().height() > 45);
        click(c, {100, 100});
        key(c, Qt::Key_Escape);                     // Esc abandons a polygon in progress
        CHECK(d->selectionBounds().width() < 60);
        tools->setCurrent(Tool::Brush);
    }

    // --- Crop: fixed aspect ratio, and straightening
    {
        QImage img(200, 200, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setPen(QPen(Qt::black, 5));
        const qreal a = qDegreesToRadians(10.0);
        p.drawLine(QPointF(100 - 90 * std::cos(a), 100 - 90 * std::sin(a)), QPointF(100 + 90 * std::cos(a), 100 + 90 * std::sin(a)));
        p.end();
        Canvas *c = newDoc(img);
        Document *d = c->document();
        tools->setCurrent(Tool::Crop);
        settings->cropRatio = 1.0;
        drag(c, {10, 10}, {110, 60});               // a square, whatever the drag shape
        key(c, Qt::Key_Return);
        CHECK(d->size() == QSize(100, 100));
        d->undoStack()->undo();
        settings->cropRatio = 0;
        d->cropRotated(QPointF(100, 100), QSize(120, 60), 10);  // the tilted line comes out level
        CHECK(d->size() == QSize(120, 60));
        const QImage out = d->layer(0).image;
        CHECK(out.pixelColor(10, 30).red() < 80 && out.pixelColor(60, 30).red() < 80 && out.pixelColor(110, 30).red() < 80);
        CHECK(out.pixelColor(60, 10).red() > 200 && out.pixelColor(60, 50).red() > 200);
        d->undoStack()->undo();
        drag(c, {50, 70}, {150, 130});              // a frame...
        drag(c, {170, 100}, {165, 130});            // ...rotated by dragging outside it
        key(c, Qt::Key_Return);
        CHECK(d->size() == QSize(100, 60) && d->undoStack()->undoText() == "Crop");
        tools->setCurrent(Tool::Brush);
    }

    // --- Blur, Sharpen and Sponge
    {
        QImage img(200, 120, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        p.fillRect(0, 0, 100, 120, Qt::black);
        p.end();
        Canvas *c = newDoc(img);
        Document *d = c->document();
        settings->size = 20;
        tools->setCurrent(Tool::Blur);
        drag(c, {100, 20}, {100, 100});
        const int edge = d->layer(0).image.pixelColor(100, 60).red();
        CHECK(edge > 30 && edge < 225);                                     // the edge got soft
        CHECK(d->layer(0).image.pixelColor(30, 60).red() == 0 && d->layer(0).image.pixelColor(170, 60).red() == 255);
        d->undoStack()->undo();

        d->applyToActive("soften", [](const QImage &i) { return Filters::gaussianBlur(i, 3); });
        const int before = d->layer(0).image.pixelColor(97, 60).red();
        tools->setCurrent(Tool::Sharpen);
        drag(c, {100, 20}, {100, 100});
        CHECK(d->layer(0).image.pixelColor(97, 60).red() < before - 5);    // dark side got darker

        QImage colour(100, 60, QImage::Format_ARGB32_Premultiplied);
        colour.fill(QColor(200, 80, 80));
        Canvas *c2 = newDoc(colour);
        Document *d2 = c2->document();
        tools->setCurrent(Tool::Sponge);
        settings->spongeSaturate = false;
        drag(c2, {20, 30}, {80, 30});
        const QColor less = d2->layer(0).image.pixelColor(50, 30);
        CHECK(less.red() - less.green() < 100);                              // less saturated
        settings->spongeSaturate = true;
        drag(c2, {20, 30}, {80, 30});
        const QColor more = d2->layer(0).image.pixelColor(50, 30);
        CHECK(more.red() - more.green() > less.red() - less.green());        // saturation back up
        settings->spongeSaturate = false;
        tools->setCurrent(Tool::Brush);
    }
    settings->pressureSize = true;
}


// Damaged and malicious files must fail cleanly and quickly, never crash or hang.
// (Regressions for bugs found by the fuzzer in tests/fuzz_files.cpp.)
static void testDamagedFiles()
{
    QString err, warn;
    auto loads = [&](const QByteArray &bytes, const QString &ext, qint64 *ms = nullptr) {
        const QString path = tmpPath(qPrintable(QStringLiteral("damaged.") + ext));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        f.close();
        QElapsedTimer timer;
        timer.start();
        Document *d = FileIO::load(path, &err, &warn);
        if (d)
            d->flattened();
        if (ms)
            *ms = timer.elapsed();
        const bool ok = d != nullptr;
        delete d;
        return ok;
    };
    auto readAll = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QString data = QStringLiteral(PAIRPAINT_TEST_DATA);

    // A layer name claiming ~4 billion characters used to make the reader loop for minutes.
    qint64 ms = 0;
    CHECK(!loads(readAll(data + "/fuzz/hang-huge-layer-name.psd"), "psd", &ms));
    CHECK(ms < 2000);
    // Found by the CI fuzzer with Qt 6.10: a corrupted font made Qt ask for 48 GB.
    CHECK(!loads(readAll(data + "/fuzz/oom-font-qt610.pairpaint"), "pairpaint"));
    CHECK(!loads(readAll(data + "/fuzz/ci-seed18-last.psd"), "psd"));

    // Write a small valid PSD to corrupt in specific ways.
    Document small(QSize(16, 8), Qt::red);
    small.addLayer();
    CHECK(Psd::write(&small, tmpPath("small.psd"), &err, &warn));
    const QByteArray psd = readAll(tmpPath("small.psd"));
    CHECK(loads(psd, "psd"));
    auto put32 = [](QByteArray &b, int at, quint32 v) {
        for (int k = 0; k < 4; ++k)
            b[at + k] = char(v >> (24 - 8 * k));
    };
    // Layer bounds whose width overflows (the first layer record starts at byte 44).
    QByteArray badRect = psd;
    put32(badRect, 44, 0x88000000u);  // top: about -2 billion
    put32(badRect, 52, 0x78000000u);  // bottom: about +2 billion
    CHECK(!loads(badRect, "psd"));
    // A header claiming a 30000 x 30000 image: too large, rejected without allocating.
    QByteArray huge = psd;
    put32(huge, 14, 30000);
    put32(huge, 18, 30000);
    FileIO::setMaxImagePixels(100'000'000);
    CHECK(!loads(huge, "psd") && err.contains("too large"));
    FileIO::setMaxImagePixels(250'000'000);

    // Cut short at every length: each attempt must end quickly without crashing.
    const QByteArray project = readAll(data + "/fuzz/seed-full.pairpaint");
    QElapsedTimer all;
    all.start();
    int projectsOpened = 0;
    for (int n = 0; n < psd.size(); n += 3)
        loads(psd.left(n), "psd");  // may open once all layers are present (only the flattened copy is cut)
    for (int n = 0; n < project.size(); n += 7)
        projectsOpened += loads(project.left(n), "pairpaint");
    CHECK(projectsOpened == 0);      // a truncated project is never accepted as complete
    CHECK(all.elapsed() < 30000);

    // Unknown blend modes and out-of-range values from files are made safe.
    CHECK(blendModeFromInt(134610944) == QPainter::CompositionMode_SourceOver);
    CHECK(blendModeFromInt(int(Blend::Hue)) == Blend::Hue);
    Layer l;
    l.opacity = std::numeric_limits<double>::quiet_NaN();
    l.fillOpacity = 7;
    l.style.shadowSize = 2000000000;
    l.style.strokeSize = -5;
    sanitizeLayer(l);
    CHECK(l.opacity == 1.0 && l.fillOpacity == 1.0 && l.style.shadowSize == 250 && l.style.strokeSize == 1);
    // Adjustment settings that would overflow (Posterize) or misread (Curves channel counts).
    const QImage gray(4, 4, QImage::Format_ARGB32_Premultiplied);
    CHECK(!Adjustments::apply(gray, Adjustment::Posterize, {2000000000}).isNull());
    CHECK(!Adjustments::apply(gray, Adjustment::Curves, {0, 0, 255, 255, -1, 500000, 3}).isNull());
}

// Builds PSD files by hand, for the attacks a mutation fuzzer is unlikely to stumble on.
namespace psdbuild {
struct Out {
    QByteArray b;
    Out &u8(int v) { b.append(char(v)); return *this; }
    Out &u16(int v) { return u8(v >> 8).u8(v); }
    Out &u32(quint32 v) { return u16(int(v >> 16)).u16(int(v & 0xffff)); }
    Out &raw(const QByteArray &d) { b.append(d); return *this; }
};
QByteArray header(int channels, int w, int h)
{
    Out o;
    o.raw("8BPS").u16(1).raw(QByteArray(6, 0)).u16(channels).u32(h).u32(w).u16(8).u16(3);
    o.u32(0).u32(0);  // color mode data, image resources
    return o.b;
}
struct LayerSpec {
    QRect rect;
    QByteArray extra;                  // tagged blocks
    QList<QPair<int, QByteArray>> ch;  // channel id, data including the compression field
};
QByteArray file(int w, int h, const QList<LayerSpec> &layers)
{
    Out info;
    info.u16(layers.size());
    for (const LayerSpec &l : layers) {
        info.u32(l.rect.top()).u32(l.rect.left()).u32(l.rect.top() + l.rect.height()).u32(l.rect.left() + l.rect.width());
        info.u16(l.ch.size());
        for (const auto &c : l.ch)
            info.u16(c.first).u32(c.second.size());
        info.raw("8BIMnorm").u8(255).u8(0).u8(0).u8(0);
        Out extra;
        extra.u32(0).u32(0).u8(3).raw("L01");  // no mask, no blending ranges, name padded to 4
        extra.raw(l.extra);
        info.u32(extra.b.size()).raw(extra.b);
    }
    for (const LayerSpec &l : layers)
        for (const auto &c : l.ch)
            info.raw(c.second);
    if (info.b.size() % 2)
        info.u8(0);
    Out o;
    o.raw(header(3, w, h));
    o.u32(4 + info.b.size() + 4).u32(info.b.size()).raw(info.b).u32(0);
    o.u16(0).raw(QByteArray(qsizetype(3) * w * h, 0));  // merged image, raw
    return o.b;
}
QByteArray block(const char *key, const QByteArray &data)
{
    Out o;
    o.raw("8BIM").raw(key).u32(data.size()).raw(data);
    if (data.size() % 2)
        o.u8(0);
    return o.b;
}
} // namespace psdbuild

// Files built to exhaust memory, time or the stack (found by code review, not by the fuzzer).
static void testHostileFiles()
{
    using namespace psdbuild;
    QString err, warn;
    auto load = [&](const QByteArray &bytes, const QString &ext) -> Document * {
        const QString path = tmpPath(qPrintable(QStringLiteral("hostile.") + ext));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return nullptr;
        f.write(bytes);
        f.close();
        return FileIO::load(path, &err, &warn);
    };
    auto loads = [&](const QByteArray &bytes, const QString &ext) {
        Document *d = load(bytes, ext);
        delete d;
        return d != nullptr;
    };

    // A sanity check of the builder: one 2x2 white layer.
    {
        LayerSpec l{QRect(1, 1, 2, 2), {}, {}};
        for (int id : {0, 1, 2, -1})
            l.ch.append({id, Out().u16(0).raw(QByteArray(4, char(255))).b});
        Document *d = load(file(4, 4, {l}), "psd");
        CHECK(d && d->layerCount() == 1 && px(d, 1, 1, 0) == QColor(Qt::white) && px(d, 0, 0, 0).alpha() == 0);
        delete d;
    }

    // Effects nested 100,000 levels deep used to overflow the stack. Now the effects are
    // dropped and the layer still opens.
    {
        Out d;
        d.u32(0).u32(16);  // effects version, descriptor version
        const int depth = 100000;
        for (int k = 0; k < depth; ++k)
            d.u32(0).u32(0).raw("null").u32(1).u32(0).raw("key ").raw("Objc");
        d.u32(0).u32(0).raw("null").u32(0);
        LayerSpec l{QRect(0, 0, 2, 2), block("lfx2", d.b), {}};
        CHECK(loads(file(4, 4, {l}), "psd"));
    }

    // Thousands of tiny layers on a big canvas: each is a full-canvas image in memory.
    FileIO::setMaxImagePixels(4'000'000);
    {
        QList<LayerSpec> empty(2000, LayerSpec{QRect(), {}, {}});
        QElapsedTimer t;
        t.start();
        Document *d = load(file(2000, 2000, empty), "psd");
        CHECK(d && d->layerCount() == 2000);  // layers without pixels share one image
        delete d;
        QList<LayerSpec> tiny(2000, LayerSpec{QRect(5, 5, 1, 1), {}, {}});
        CHECK(!loads(file(2000, 2000, tiny), "psd") && err.contains("too large to open"));
        CHECK(t.elapsed() < 10000);
    }
    // A few kilobytes of ZIP data claiming a 4-megapixel channel.
    {
        LayerSpec l{QRect(0, 0, 2000, 2000), {}, {{0, Out().u16(2).raw(qCompress(QByteArray(1000, 0)).mid(4)).b}}};
        CHECK(!loads(file(2000, 2000, {l}), "psd") && err.contains("compressed data too short"));
    }
    // The flattened image: an impossible channel count, and RLE rows claiming far more than they hold.
    {
        QByteArray tooMany = header(500, 100, 100);
        tooMany += Out().u32(0).u16(0).raw(QByteArray(100, 0)).b;
        CHECK(!loads(tooMany, "psd") && err.contains("channel count"));
        Out rle;
        rle.raw(header(56, 2000, 2000)).u32(0).u16(1);
        for (int k = 0; k < 56 * 2000; ++k)
            rle.u16(0);
        CHECK(!loads(rle.b, "psd") && err.contains("compressed data too short"));
    }
    // Projects: empty layers are free, painted ones count.
    {
        DocState s;
        s.size = QSize(2000, 2000);
        Layer blank;
        blank.image = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
        blank.image.fill(Qt::transparent);
        s.layers = QList<Layer>(40, blank);
        {
            Document d(s);
            CHECK(FileIO::saveProject(&d, tmpPath("many-empty.pairpaint"), &err));
        }
        Document *d = FileIO::load(tmpPath("many-empty.pairpaint"), &err, &warn);
        CHECK(d && d->layerCount() == 40);
        delete d;
        for (Layer &l : s.layers) {
            l.image = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
            l.image.fill(Qt::transparent);
            l.image.setPixelColor(7, 7, Qt::red);
        }
        {
            Document painted(s);
            CHECK(FileIO::saveProject(&painted, tmpPath("many-painted.pairpaint"), &err));
        }
        CHECK(!FileIO::load(tmpPath("many-painted.pairpaint"), &err, &warn) && err.contains("too large to open"));
    }
    FileIO::setMaxImagePixels(250'000'000);

    // Fonts are stored as text since version 6; check they survive, and that version 5
    // projects (which stored a serialized QFont) still open with their text.
    {
        Document d(QSize(200, 100), Qt::white);
        TextData t;
        t.text = QStringLiteral("Hello");
        t.font = QFont(QStringLiteral("Sans Serif"));
        t.font.setPixelSize(37);
        t.font.setBold(true);
        t.font.setItalic(true);
        t.color = Qt::blue;
        t.pos = QPointF(10, 20);
        d.addTextLayer(t);
        CHECK(FileIO::saveProject(&d, tmpPath("font.pairpaint"), &err));
        Document *back = FileIO::load(tmpPath("font.pairpaint"), &err, &warn);
        const TextData *bt = back && back->layerCount() == 2 ? &back->layer(1).text : nullptr;
        CHECK(bt && bt->text == t.text && bt->font.pixelSize() == 37 && bt->font.bold() && bt->font.italic()
              && bt->font.family() == t.font.family() && bt->color == t.color && bt->pos == t.pos);
        delete back;
        Document *old = FileIO::load(QStringLiteral(PAIRPAINT_TEST_DATA) + "/fuzz/seed-full.pairpaint", &err, &warn);
        bool hasText = false;
        if (old)
            for (const Layer &l : old->state().layers)
                hasText |= l.text.isValid() && l.text.font.pixelSize() > 0;
        CHECK(old && hasText);
        delete old;
    }

    // Out-of-range Hue/Saturation color ranges are clamped by position in each range.
    QList<int> hs = Adjustments::defaults(Adjustment::HueSaturation);
    hs << 999 << -5 << 400 << 10 << 999 << -999 << 500;
    hs = Adjustments::validated(Adjustment::HueSaturation, hs);
    CHECK(hs.mid(3, 7) == QList<int>({360, 0, 360, 10, 180, -100, 100}));
}

static void testPhotoFixes()
{
    auto image = [](int w, int h, const std::function<QColor(int, int)> &f) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.setPixelColor(x, y, f(x, y));
        return img;
    };
    auto range = [](const QImage &img, int channel) {
        int lo = 255, hi = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                const int v = channel == 0 ? c.red() : channel == 1 ? c.green() : c.blue();
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        return std::pair(lo, hi);
    };
    using Adjustments::Auto;
    auto autoFix = [](const QImage &img, Auto mode) {
        return Adjustments::apply(img, Adjustment::Levels, Adjustments::autoLevels(img, mode));
    };
    // A dull photo: red spans 60..180, blue only 100..140.
    const QImage dull = image(121, 4, [](int x, int) { return QColor(60 + x, 80 + x / 2, 100 + x / 3); });
    const QImage tone = autoFix(dull, Auto::Tone);
    CHECK(range(tone, 0) == std::pair(0, 255) && range(tone, 2) == std::pair(0, 255));  // every channel stretched
    const QImage contrast = autoFix(dull, Auto::Contrast);
    CHECK(range(contrast, 0).first == 0 && range(contrast, 0).second > 200);
    CHECK(range(contrast, 2).second - range(contrast, 2).first < 150);  // colors keep their balance
    // A gray ramp with a red cast becomes neutral.
    const QImage cast = image(200, 4, [](int x, int) { return QColor(std::min(255, 40 + x + 30), 40 + x, 40 + x); });
    const QImage color = autoFix(cast, Auto::Color);
    const QColor mid = color.pixelColor(100, 1);
    CHECK(std::abs(mid.red() - mid.blue()) <= 4 && std::abs(mid.red() - mid.green()) <= 4);
    QImage clear(8, 8, QImage::Format_ARGB32_Premultiplied);
    clear.fill(Qt::transparent);
    CHECK(Adjustments::autoLevels(clear, Auto::Color) == Adjustments::defaults(Adjustment::Levels));

    // Vibrance boosts dull colors more than saturated ones, and doesn't clip.
    const QImage two = image(2, 1, [](int x, int) { return x == 0 ? QColor(110, 120, 140) : QColor(30, 60, 220); });
    const QImage vib = Adjustments::apply(two, Adjustment::Vibrance, {100, 0});
    auto chroma = [](QColor c) { return std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()}); };
    const double dullGain = chroma(vib.pixelColor(0, 0)) / double(chroma(two.pixelColor(0, 0)));
    const double vividGain = chroma(vib.pixelColor(1, 0)) / double(chroma(two.pixelColor(1, 0)));
    CHECK(dullGain > 1.6 && vividGain < 1.3 && dullGain > vividGain + 0.4);
    CHECK(vib.pixelColor(1, 0).blue() <= 255 && vib.pixelColor(1, 0).red() > 0);
    const QImage gray = image(1, 1, [](int, int) { return QColor(128, 128, 128); });
    CHECK(chroma(Adjustments::apply(two, Adjustment::Vibrance, {0, -100}).pixelColor(1, 0)) <= 1);  // fully gray

    // Exposure +1 stop doubles the light: sRGB 128 is 0.216 linear, so 0.432, which is 175 in sRGB.
    CHECK(std::abs(Adjustments::apply(gray, Adjustment::Exposure, {100, 0, 100}).pixelColor(0, 0).red() - 175) <= 1);
    CHECK(Adjustments::apply(gray, Adjustment::Exposure, {0, 0, 100}).pixelColor(0, 0) == QColor(128, 128, 128));
    // White balance: warmer is redder and less blue, and a gray keeps roughly its brightness.
    const QColor warm = Adjustments::apply(gray, Adjustment::WhiteBalance, {60, 0}).pixelColor(0, 0);
    CHECK(warm.red() > 140 && warm.blue() < 110 && std::abs(warm.green() - 128) < 12);
    const QColor magenta = Adjustments::apply(gray, Adjustment::WhiteBalance, {0, 60}).pixelColor(0, 0);
    CHECK(magenta.green() < 128 && magenta.red() > 128 && magenta.blue() > 128);
    // Color Balance: red in the shadows reddens dark pixels but hardly touches light ones.
    const QImage darkLight = image(2, 1, [](int x, int) { return x == 0 ? QColor(40, 40, 40) : QColor(230, 230, 230); });
    QList<int> cb = Adjustments::defaults(Adjustment::ColorBalance);
    CHECK(cb.size() == 9);
    cb[0] = 100;
    cb << 0;  // don't preserve luminosity
    const QImage reddened = Adjustments::apply(darkLight, Adjustment::ColorBalance, cb);
    CHECK(reddened.pixelColor(0, 0).red() > 120 && reddened.pixelColor(0, 0).green() == 40);
    CHECK(std::abs(reddened.pixelColor(1, 0).red() - 230) <= 2);
    cb[9] = 1;  // preserve luminosity: redder, but the lightness stays
    const QColor kept = Adjustments::apply(darkLight, Adjustment::ColorBalance, cb).pixelColor(0, 0);
    CHECK(kept.red() > kept.green() && std::abs((std::max({kept.red(), kept.green(), kept.blue()})
                                                 + std::min({kept.red(), kept.green(), kept.blue()})) / 2 - 40) <= 1);
    CHECK(Adjustments::mainParams(Adjustment::ColorBalance, cb).size() == 9
          && Adjustments::withMainParams(Adjustment::ColorBalance, cb, QList<int>(9, 5)).value(9) == 1);

    // New adjustment layers survive a project round trip, including the hidden flag.
    QString err, warn;
    {
        Document d(QSize(20, 20), Qt::gray);
        d.addAdjustmentLayer({Adjustment::ColorBalance, cb});
        d.addAdjustmentLayer({Adjustment::Exposure, {-150, 20, 120}});
        CHECK(FileIO::saveProject(&d, tmpPath("photo-fixes.pairpaint"), &err));
        Document *back = FileIO::load(tmpPath("photo-fixes.pairpaint"), &err, &warn);
        CHECK(back && back->layerCount() == 3 && back->layer(1).adjustment.type == Adjustment::ColorBalance
              && back->layer(1).adjustment.params == cb && back->layer(2).adjustment.params == QList<int>({-150, 20, 120}));
        delete back;
    }

    // Photoshop's Vibrance, Exposure and Color Balance layers open as adjustment layers.
    {
        using namespace psdbuild;
        Out vib;
        vib.u32(16).u32(0).u32(0).raw("null").u32(2);
        vib.u32(8).raw("vibrance").raw("long").u32(35);
        vib.u32(0).raw("Strt").raw("long").u32(quint32(-20));
        Out exp;
        exp.u16(1);
        for (float f : {1.5f, -0.05f, 1.2f}) {
            quint32 bits;
            std::memcpy(&bits, &f, 4);
            exp.u32(bits);
        }
        Out bal;
        for (int v : {10, -20, 30, 0, 0, 0, -5, 0, 100})
            bal.u16(v & 0xffff);
        bal.u8(0);
        LayerSpec base{QRect(0, 0, 4, 4), {}, {}};
        for (int id : {0, 1, 2, -1})
            base.ch.append({id, Out().u16(0).raw(QByteArray(16, char(128))).b});
        const QByteArray psd = file(4, 4, {base, {QRect(), block("vibA", vib.b), {}}, {QRect(), block("expA", exp.b), {}},
                                         {QRect(), block("blnc", bal.b), {}}});
        QFile f(tmpPath("photo-fixes.psd"));
        CHECK(f.open(QIODevice::WriteOnly) && f.write(psd) == psd.size());
        f.close();
        Document *d = FileIO::load(tmpPath("photo-fixes.psd"), &err, &warn);
        CHECK(d && d->layerCount() == 4);
        if (d && d->layerCount() == 4) {
            CHECK(d->layer(1).adjustment.type == Adjustment::Vibrance && d->layer(1).adjustment.params == QList<int>({35, -20}));
            CHECK(d->layer(2).adjustment.type == Adjustment::Exposure && d->layer(2).adjustment.params == QList<int>({150, -50, 120}));
            CHECK(d->layer(3).adjustment.type == Adjustment::ColorBalance
                  && d->layer(3).adjustment.params == QList<int>({10, -20, 30, 0, 0, 0, -5, 0, 100, 0}));
            CHECK(!warn.contains("Exposure") && !warn.contains("not supported"));
        }
        delete d;
    }

    // JPEG quality: lower quality, smaller file.
    {
        Document d(image(200, 200, [](int x, int y) { return QColor((x * 7) % 256, (y * 5) % 256, (x * y) % 256); }));
        CHECK(FileIO::exportImage(&d, tmpPath("q10.jpg"), &err, 10) && FileIO::exportImage(&d, tmpPath("q95.jpg"), &err, 95));
        CHECK(QFileInfo(tmpPath("q10.jpg")).size() * 2 < QFileInfo(tmpPath("q95.jpg")).size());
        CHECK(FileIO::hasQuality("a.JPG") && FileIO::hasQuality("b.webp") && !FileIO::hasQuality("c.png"));
    }
}

// HEIC photos (iPhone): opened upright, with Display P3 colors converted to sRGB.
static void testHeic()
{
    const QString sample = QStringLiteral(PAIRPAINT_TEST_DATA) + "/heic/sample.heic";
    // Builds expected to open HEIC set PAIRPAINT_EXPECT_HEIF (CI on Linux and macOS); on Windows it
    // depends on whether the system's HEIF and HEVC codecs are installed.
    const bool expected = !qEnvironmentVariable("PAIRPAINT_EXPECT_HEIF").isEmpty();
    CHECK(Heif::isHeif(sample) && !Heif::isHeif(QStringLiteral(PAIRPAINT_TEST_DATA) + "/fuzz/seed-small.pairpaint"));
    QString err, warn;
    if (!Heif::hasDecoder()) {  // through a Qt plugin: show what it returns, to diagnose color problems
        QImageReader reader(sample);
        const QImage raw = reader.read();
        std::printf("HEIC via Qt: format '%s', %dx%d, color space '%s', pixel (36,16) %s\n", reader.format().constData(),
                    raw.width(), raw.height(), qPrintable(raw.colorSpace().description()),
                    raw.isNull() ? "-" : qPrintable(raw.pixelColor(std::min(36, raw.width() - 1), std::min(16, raw.height() - 1)).name()));
    }
    Document *d = FileIO::load(sample, &err, &warn);
    std::printf("HEIC: %s%s\n", d ? "opened" : "not opened: ", qPrintable(err));
    if (!d) {
        CHECK(!expected);
#ifdef Q_OS_WIN
        CHECK(err.contains("HEIF Image Extensions"));  // tells the user what to install
#endif
        return;
    }
    CHECK(FileIO::openFilter().contains("*.heic"));
    CHECK(d->size() == QSize(48, 64));  // stored 64 x 48, rotated for display
    // The stored colors are Display P3; the document has them in sRGB.
    const QColorTransform toSrgb =
        QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::SRgb).transformationToColorSpace(QColorSpace::SRgb);
    auto near = [](QColor a, QColor b) {
        return std::abs(a.red() - b.red()) <= 6 && std::abs(a.green() - b.green()) <= 6 && std::abs(a.blue() - b.blue()) <= 6;
    };
    const QColor blue = toSrgb.map(QColor(40, 60, 200)), red = toSrgb.map(QColor(200, 60, 40));
    const QColor green = toSrgb.map(QColor(60, 180, 60));
    CHECK(!near(red, QColor(200, 60, 40)));  // the conversion is large enough to see
    CHECK(near(px(d, 12, 16, 0), blue) && near(px(d, 36, 16, 0), red));
    CHECK(near(px(d, 12, 48, 0), QColor(128, 128, 128)) && near(px(d, 36, 48, 0), green));
    std::printf("     top left %s, top right %s (expected %s, %s)\n", qPrintable(px(d, 12, 16, 0).name()),
                qPrintable(px(d, 36, 16, 0).name()), qPrintable(blue.name()), qPrintable(red.name()));
    delete d;

    // Damaged: cut short, and a header claiming an enormous image.
    QFile f(sample);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    for (int n : {12, 100, 300, int(bytes.size()) - 50}) {
        QFile cut(tmpPath("cut.heic"));
        CHECK(cut.open(QIODevice::WriteOnly) && cut.write(bytes.left(n)) == n);
        cut.close();
        Document *broken = FileIO::load(tmpPath("cut.heic"), &err, &warn);
        if (Heif::hasDecoder())  // macOS may show a partly decoded image instead; either is fine
            CHECK(!broken && !err.isEmpty());
        delete broken;
    }
    FileIO::setMaxImagePixels(1000);
    CHECK(!FileIO::load(sample, &err, &warn) && err.contains("too large"));
    FileIO::setMaxImagePixels(250'000'000);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);  // keep the user's real settings untouched
    QApplication::setOrganizationName("PairPaintTests");
    QTemporaryDir dir;
    outputDir = &dir;
    if (qEnvironmentVariableIsSet("PAIRPAINT_KEEP_TEST_FILES")) {  // e.g. to make new fuzzing seeds
        dir.setAutoRemove(false);
        std::printf("test files kept in %s\n", qPrintable(dir.path()));
    }
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
    testOnCanvasText(w, tools, settings);
    testToolPack(w, tools, settings);
    testPhotoshopFiles();
    testDamagedFiles();
    testHostileFiles();
    testPhotoFixes();
    testHeic();
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
