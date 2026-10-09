// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

class Document;

// Photoshop .psd import/export.
// Reads: RGB, grayscale and CMYK (converted to RGB), 8 or 16 bits per channel,
// raw / RLE / ZIP compressed layers with names, opacity, visibility, blend
// modes and layer masks; files without layers use the merged image.
// Writes: 8-bit RGB with pixel layers, masks, opacity, visibility and blend modes.
namespace Psd {

// `warning` (optional) describes Photoshop features that couldn't be reproduced.
Document *read(const QString &path, QString *error, QString *warning = nullptr);
// The flattened image stored in the file, as rendered by the application that saved it.
QImage readComposite(const QString &path, QString *error);
bool write(const Document *doc, const QString &path, QString *error, QString *warning);

} // namespace Psd
