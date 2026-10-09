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
Document *load(const QString &path, QString *error);
bool saveProject(const Document *doc, const QString &path, QString *error);
bool exportImage(const Document *doc, const QString &path, QString *error);

} // namespace FileIO
