// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "FileIO.h"

#include "Document.h"
#include "Psd.h"

#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QObject>
#include <QSaveFile>

namespace {

constexpr quint32 kMagic = 0x50504E54;  // "PPNT"
constexpr quint32 kVersion = 3;  // 2: masks, adjustment layers, text layers; 3: layer styles
const QString kProjectSuffix = QStringLiteral("pairpaint");

QString projectFilterEntry() { return QObject::tr("PairPaint Project (*.pairpaint)"); }

QString writableImageFilters()
{
    const QList<QByteArray> fmts = QImageWriter::supportedImageFormats();
    const QList<QPair<QString, QString>> known = {
        {"PNG", "*.png"}, {"JPEG", "*.jpg *.jpeg"}, {"WebP", "*.webp"},
        {"BMP", "*.bmp"}, {"TIFF", "*.tif *.tiff"},
    };
    QStringList out;
    for (const auto &[name, pattern] : known) {
        const QByteArray ext = pattern.section(' ', 0, 0).mid(2).toLatin1();
        if (fmts.contains(ext))
            out << QStringLiteral("%1 (%2)").arg(name, pattern);
    }
    return out.join(QStringLiteral(";;"));
}

QImage decodeImage(const QByteArray &png, const QSize &size)
{
    QImage img = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.size() != size) {
        QImage fixed(size, QImage::Format_ARGB32_Premultiplied);
        fixed.fill(Qt::transparent);
        QPainter p(&fixed);
        p.drawImage(0, 0, img);
        p.end();
        img = fixed;
    }
    return img;
}

QByteArray encodePng(const QImage &img)
{
    QByteArray png;
    if (img.isNull())
        return png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

Document *loadProject(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return nullptr;
    }
    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (magic != kMagic || version > kVersion) {
        *error = QObject::tr("Not a PairPaint project, or it was written by a newer version.");
        return nullptr;
    }
    DocState s;
    qint32 active = 0, count = 0;
    in >> s.size >> active >> count;
    if (in.status() != QDataStream::Ok || s.size.isEmpty() || count <= 0 || count > 10000) {
        *error = QObject::tr("The file is damaged.");
        return nullptr;
    }
    for (int i = 0; i < count; ++i) {
        Layer l;
        double opacity = 1.0;
        qint32 mode = 0;
        QByteArray png;
        in >> l.name >> l.visible >> opacity >> mode >> png;
        l.opacity = opacity;
        l.mode = QPainter::CompositionMode(mode);
        l.image = decodeImage(png, s.size);
        if (version >= 2) {
            QByteArray maskPng;
            qint32 adjType = 0;
            QList<qint32> params;
            bool hasText = false;
            in >> maskPng >> l.maskEnabled >> adjType >> params >> hasText;
            if (!maskPng.isEmpty())
                l.mask = decodeImage(maskPng, s.size);
            if (adjType > Adjustment::None && adjType < Adjustment::TypeCount) {
                l.adjustment.type = Adjustment::Type(adjType);
                l.adjustment.params = QList<int>(params.begin(), params.end());
                if (l.mask.isNull()) {
                    l.mask = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
                    l.mask.fill(Qt::white);
                }
            }
            if (hasText)
                in >> l.text.text >> l.text.font >> l.text.color >> l.text.pos >> l.text.antialias;
        }
        if (version >= 3) {
            LayerStyle &st = l.style;
            qint32 v[11] = {};
            in >> st.shadow >> st.shadowColor >> st.glow >> st.glowColor >> st.stroke >> st.strokeColor;
            for (qint32 &x : v)
                in >> x;
            st.shadowOpacity = v[0];
            st.shadowAngle = v[1];
            st.shadowDistance = v[2];
            st.shadowSize = v[3];
            st.glowOpacity = v[4];
            st.glowSize = v[5];
            st.strokeOpacity = v[6];
            st.strokeSize = v[7];
        }
        s.layers.append(l);
    }
    if (in.status() != QDataStream::Ok) {
        *error = QObject::tr("The file is damaged or truncated.");
        return nullptr;
    }
    s.active = active;
    return new Document(s);
}

} // namespace

namespace FileIO {

QString openFilter()
{
    QStringList patterns{QStringLiteral("*.pairpaint"), QStringLiteral("*.psd")};
    for (const QByteArray &fmt : QImageReader::supportedImageFormats())
        patterns << QStringLiteral("*.") + QString::fromLatin1(fmt);
    return QObject::tr("All Supported (%1)").arg(patterns.join(' ')) + QStringLiteral(";;")
         + projectFilterEntry() + QStringLiteral(";;") + QObject::tr("Photoshop (*.psd)") + QStringLiteral(";;")
         + QObject::tr("All Files (*)");
}

QString saveFilter()
{
    return projectFilterEntry() + QStringLiteral(";;") + QObject::tr("Photoshop (*.psd)") + QStringLiteral(";;")
         + writableImageFilters();
}

QString exportFilter() { return writableImageFilters(); }

bool isProjectFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(kProjectSuffix, Qt::CaseInsensitive) == 0;
}

bool isPsdFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("psd"), Qt::CaseInsensitive) == 0;
}

bool isLayeredFormat(const QString &path) { return isProjectFile(path) || isPsdFile(path); }

bool save(const Document *doc, const QString &path, QString *error, QString *warning)
{
    if (isProjectFile(path))
        return saveProject(doc, path, error);
    if (isPsdFile(path))
        return Psd::write(doc, path, error, warning);
    return exportImage(doc, path, error);
}

Document *load(const QString &path, QString *error)
{
    if (isProjectFile(path))
        return loadProject(path, error);
    if (isPsdFile(path))
        return Psd::read(path, error);

    QImageReader reader(path);
    reader.setAutoTransform(true);  // honor EXIF orientation
    const QImage img = reader.read();
    if (img.isNull()) {
        *error = reader.errorString();
        return nullptr;
    }
    return new Document(img);
}

bool saveProject(const Document *doc, const QString &path, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    QDataStream out(&f);
    out.setVersion(QDataStream::Qt_6_0);
    const DocState &s = doc->state();
    out << kMagic << kVersion << s.size << qint32(s.active) << qint32(s.layers.size());
    for (const Layer &l : s.layers) {
        out << l.name << l.visible << double(l.opacity) << qint32(l.mode)
            << encodePng(l.isAdjustment() ? QImage() : l.image);
        out << encodePng(l.mask) << l.maskEnabled << qint32(l.adjustment.type)
            << QList<qint32>(l.adjustment.params.begin(), l.adjustment.params.end()) << l.text.isValid();
        if (l.text.isValid())
            out << l.text.text << l.text.font << l.text.color << l.text.pos << l.text.antialias;
        const LayerStyle &st = l.style;
        out << st.shadow << st.shadowColor << st.glow << st.glowColor << st.stroke << st.strokeColor;
        for (int v : {st.shadowOpacity, st.shadowAngle, st.shadowDistance, st.shadowSize, st.glowOpacity,
                      st.glowSize, st.strokeOpacity, st.strokeSize, 0, 0, 0})  // 3 spare slots
            out << qint32(v);
    }
    if (out.status() != QDataStream::Ok || !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

bool exportImage(const Document *doc, const QString &path, QString *error)
{
    QImage img = doc->flattened();
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool noAlpha = suffix == "jpg" || suffix == "jpeg" || suffix == "bmp";
    if (noAlpha) {
        QImage opaque(img.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        QPainter p(&opaque);
        p.drawImage(0, 0, img);
        p.end();
        img = opaque;
    }
    QImageWriter writer(path);
    if (suffix == "jpg" || suffix == "jpeg" || suffix == "webp")
        writer.setQuality(92);
    if (!writer.write(img)) {
        *error = writer.errorString();
        return false;
    }
    return true;
}

} // namespace FileIO
