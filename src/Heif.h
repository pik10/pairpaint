// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QString>

// HEIC/HEIF photos (the default format of iPhone cameras). Qt only reads them on macOS, through
// its plugin; elsewhere PairPaint decodes them itself: with libheif on Linux, and on Windows
// with the system's codec ("HEIF Image Extensions" and "HEVC Video Extensions").
namespace Heif {

// Whether this build has its own HEIF decoder (if not, Qt's plugins are used, as for any image).
bool hasDecoder();
// Whether the file is a HEIF file, judged by its contents.
bool isHeif(const QString &path);
// The primary image, turned upright, with its color space set (not yet converted). On failure
// returns a null image and sets `error`.
QImage read(const QString &path, QString *error);

} // namespace Heif
