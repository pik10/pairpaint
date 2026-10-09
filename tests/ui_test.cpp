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
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
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

    std::printf("\n%d failure(s)\n", fails);
    d->undoStack()->setClean();
    for (auto *cc : w.findChildren<Canvas *>()) cc->document()->undoStack()->setClean();
    return fails ? 1 : 0;
}
