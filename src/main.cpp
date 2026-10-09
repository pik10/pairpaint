// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>

namespace {

void applyDarkTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const QColor window(50, 50, 50), base(35, 35, 35), text(225, 225, 225), disabled(120, 120, 120);
    const QColor highlight(42, 130, 218);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    app.setPalette(p);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication::setApplicationName(QStringLiteral("PairPaint"));
    QApplication::setOrganizationName(QStringLiteral("PairPaint"));
    // The interface size must be applied before the application object exists.
    // An explicit QT_SCALE_FACTOR from the environment wins.
    const double uiScale = QSettings().value(QStringLiteral("ui/scale"), 1.0).toDouble();
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR") && uiScale >= 0.5 && uiScale <= 4.0 && uiScale != 1.0)
        qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScale));

    // Qt's Wayland text-input support logs a warning on every focus change; it is harmless.
    // QT_LOGGING_RULES from the environment still takes precedence.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.textinput=false"));

    QApplication app(argc, argv);
    QApplication::setApplicationVersion(QStringLiteral(PAIRPAINT_VERSION));
    // Identify with the desktop entry (window icon, taskbar grouping) only when it is
    // installed; otherwise the desktop portal rejects the unknown app ID.
    const QString appId = QStringLiteral("io.github.pik10.PairPaint");
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + QStringLiteral(".desktop")).isEmpty())
        QApplication::setDesktopFileName(appId);
    applyDarkTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cross-platform layered image editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Images or projects to open."), QStringLiteral("[files...]"));
    parser.process(app);

    MainWindow window;
    window.show();
    for (const QString &f : parser.positionalArguments())
        window.openFile(f);
    return app.exec();
}
