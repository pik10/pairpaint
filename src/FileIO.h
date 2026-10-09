// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QString>

class Document;

namespace FileIO {

QString openFilter();
QString saveFilter();    // project, PSD and flat image formats
QString exportFilter();  // flat image formats only

bool isProjectFile(const QString &path);
bool isPsdFile(const QString &path);
bool isLayeredFormat(const QString &path);  // .pairpaint or .psd
// Saves in the format given by the file extension (layered or flattened).
bool save(const Document *doc, const QString &path, QString *error, QString *warning);
// `warning` (optional) receives notes about content that couldn't be fully reproduced.
Document *load(const QString &path, QString *error, QString *warning = nullptr);
bool saveProject(const Document *doc, const QString &path, QString *error);
bool exportImage(const Document *doc, const QString &path, QString *error);

// Largest image (in pixels) the readers accept; damaged files can claim absurd sizes.
qint64 maxImagePixels();
void setMaxImagePixels(qint64 pixels);  // tests and the fuzzer lower it
// Every layer is a full-size image, so a file's layers together are limited too.
qint64 maxTotalPixels();

// Counts the full-size images a reader allocates; take() throws a message past the limit.
class PixelBudget {
public:
    void take(qint64 pixels);

private:
    qint64 m_used = 0;
};

} // namespace FileIO
