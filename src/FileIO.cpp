// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "FileIO.h"

#include "Document.h"
#include "Heif.h"
#include "Psd.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QObject>
#include <QSaveFile>
#include <QColor>
#include <QPointF>
#include <QSize>
#include <new>

namespace {

constexpr quint32 kMagic = 0x50504E54;  // "PPNT"
constexpr quint32 kVersion = 7;  // 2: masks, adjustments, text; 3: styles; 4: groups; 5: clipping, fill;
                                  // 6: fonts stored as description strings; 7: Vibrance, Exposure,
                                  // Color Balance and White Balance adjustments
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

// Reads a project's values with bounds checks: lengths and counts in the file are checked
// against what is actually left in it, so a damaged file can't make us allocate huge
// amounts of memory or read garbage. Failures throw a message.
class ProjectReader {
public:
    explicit ProjectReader(QIODevice *device) : m_device(device), m_in(device) { m_in.setVersion(QDataStream::Qt_6_0); }

    template <typename T>
    T read()
    {
        T v{};
        m_in >> v;
        if (m_in.status() != QDataStream::Ok)
            throw QObject::tr("The file is damaged or truncated.");
        return v;
    }

    // Same layout as QDataStream's QString: byte length, then UTF-16 (big-endian).
    QString string(int maxChars)
    {
        const quint32 bytes = read<quint32>();
        if (bytes == 0xffffffffu)
            return QString();
        if (bytes % 2 || bytes > quint32(maxChars) * 2 || bytes > remaining())
            throw QObject::tr("The file is damaged.");
        const QByteArray raw = rawBytes(bytes);
        QString out(qsizetype(bytes / 2), Qt::Uninitialized);
        for (qsizetype i = 0; i < out.size(); ++i)
            out[i] = QChar(char16_t((uchar(raw[2 * i]) << 8) | uchar(raw[2 * i + 1])));
        return out;
    }

    // Same layout as QDataStream's QByteArray.
    QByteArray bytes()
    {
        const quint32 n = read<quint32>();
        if (n == 0xffffffffu)
            return QByteArray();
        if (n > remaining())
            throw QObject::tr("The file is damaged or truncated.");
        return rawBytes(n);
    }

    QDataStream &stream() { return m_in; }

private:
    quint64 remaining() const { return quint64(std::max<qint64>(0, m_device->size() - m_device->pos())); }

    QByteArray rawBytes(quint32 n)
    {
        QByteArray b(qsizetype(n), Qt::Uninitialized);
        if (m_in.readRawData(b.data(), int(n)) != int(n))
            throw QObject::tr("The file is damaged or truncated.");
        return b;
    }

    QIODevice *m_device;
    QDataStream m_in;
};

// Decodes a layer's PNG. Empty data gives `blank` (shared, so it costs no memory); anything
// else counts against the budget for the whole file.
QImage decodeImage(const QByteArray &png, const QSize &size, const QImage &blank, FileIO::PixelBudget &budget)
{
    if (png.isEmpty())
        return blank;
    QBuffer buffer;
    buffer.setData(png);
    QImageReader reader(&buffer, "PNG");
    const QSize stored = reader.size();
    if (!stored.isValid() || qint64(stored.width()) * stored.height() > FileIO::maxImagePixels())
        throw QObject::tr("The file is damaged (a layer image is invalid or too large).");
    budget.take(qint64(size.width()) * size.height());
    QImage img = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.size() != size) {
        QImage fixed(size, QImage::Format_ARGB32_Premultiplied);
        if (fixed.isNull())
            throw QObject::tr("Not enough memory to open this file.");
        fixed.fill(Qt::transparent);
        QPainter p(&fixed);
        p.drawImage(0, 0, img);
        p.end();
        img = fixed;
    }
    return img;
}

Document *loadProject(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return nullptr;
    }
    try {
        ProjectReader r(&f);
        const quint32 magic = r.read<quint32>(), version = r.read<quint32>();
        if (magic != kMagic || version > kVersion) {
            *error = QObject::tr("Not a PairPaint project, or it was written by a newer version.");
            return nullptr;
        }
        DocState s;
        s.size = r.read<QSize>();
        const qint32 active = r.read<qint32>(), count = r.read<qint32>();
        if (s.size.isEmpty() || count <= 0 || count > 10000)
            throw QObject::tr("The file is damaged.");
        if (qint64(s.size.width()) * s.size.height() > FileIO::maxImagePixels())
            throw QObject::tr("The image is too large (%1 × %2 pixels).").arg(s.size.width()).arg(s.size.height());
        // Layers without pixels share one blank image; everything else is counted.
        QImage blank(s.size, QImage::Format_ARGB32_Premultiplied);
        if (blank.isNull())
            throw QObject::tr("Not enough memory to open this file.");
        blank.fill(Qt::transparent);
        FileIO::PixelBudget budget;
        for (int i = 0; i < count; ++i) {
            Layer l;
            l.name = r.string(10000);
            l.visible = r.read<bool>();
            l.opacity = r.read<double>();
            l.mode = blendModeFromInt(r.read<qint32>());  // never cast an unknown number to the enum
            l.image = decodeImage(r.bytes(), s.size, blank, budget);
            if (version >= 2) {
                const QByteArray maskPng = r.bytes();
                l.maskEnabled = r.read<bool>();
                const qint32 adjType = r.read<qint32>();
                const quint32 paramCount = r.read<quint32>();
                if (paramCount > quint32(Adjustments::kMaxParams))
                    throw QObject::tr("The file is damaged.");
                QList<int> params;
                for (quint32 k = 0; k < paramCount; ++k)
                    params << r.read<qint32>();
                const bool hasText = r.read<bool>();
                if (!maskPng.isEmpty())
                    l.mask = decodeImage(maskPng, s.size, blank, budget);
                if (adjType > Adjustment::None && adjType < Adjustment::TypeCount) {
                    l.adjustment.type = Adjustment::Type(adjType);
                    l.adjustment.params = params;
                    if (l.mask.isNull()) {
                        budget.take(qint64(s.size.width()) * s.size.height());
                        l.mask = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
                        l.mask.fill(Qt::white);
                    }
                }
                if (hasText) {
                    l.text.text = r.string(100000);
                    if (version >= 6) {
                        // Font as a short description string, read with a length limit.
                        l.text.font.fromString(r.string(1000));
                        l.text.font.setPixelSize(std::clamp(r.read<qint32>(), 1, 2000));
                    } else {
                        // Older files stored Qt's own font format. Qt reads it itself, so a
                        // damaged one is caught as out-of-memory by FileIO::load.
                        r.stream() >> l.text.font;
                    }
                    l.text.color = r.read<QColor>();
                    l.text.pos = r.read<QPointF>();
                    l.text.antialias = r.read<bool>();
                }
            }
            if (version >= 3) {
                LayerStyle &st = l.style;
                st.shadow = r.read<bool>();
                st.shadowColor = r.read<QColor>();
                st.glow = r.read<bool>();
                st.glowColor = r.read<QColor>();
                st.stroke = r.read<bool>();
                st.strokeColor = r.read<QColor>();
                qint32 v[11];
                for (qint32 &x : v)
                    x = r.read<qint32>();
                st.shadowOpacity = v[0];
                st.shadowAngle = v[1];
                st.shadowDistance = v[2];
                st.shadowSize = v[3];
                st.glowOpacity = v[4];
                st.glowSize = v[5];
                st.strokeOpacity = v[6];
                st.strokeSize = v[7];
            }
            if (version >= 4) {
                const qint32 kind = r.read<qint32>();
                l.collapsed = r.read<bool>();
                if (kind == int(LayerKind::Group) || kind == int(LayerKind::GroupEnd))
                    l.kind = LayerKind(kind);
            }
            if (version >= 5) {
                l.clipped = r.read<bool>();
                l.fillOpacity = r.read<double>();
            }
            sanitizeLayer(l);
            s.layers.append(l);
        }
        s.active = active;
        return new Document(s);
    } catch (const QString &message) {
        *error = message;
        return nullptr;
    }
}

} // namespace

namespace FileIO {

namespace {
qint64 g_maxImagePixels = 250'000'000;  // about 1 GB per layer
}

Document *loadUnchecked(const QString &path, QString *error, QString *warning);

qint64 maxImagePixels() { return g_maxImagePixels; }
qint64 maxTotalPixels() { return 8 * g_maxImagePixels; }

void PixelBudget::take(qint64 pixels)
{
    m_used += pixels;
    if (m_used > maxTotalPixels())
        throw QObject::tr("The file is too large to open (its layers need more than %1 megapixels in total).")
            .arg(maxTotalPixels() / 1'000'000);
}
void setMaxImagePixels(qint64 pixels) { g_maxImagePixels = pixels; }

QString tooLargeMessage(qint64 width, qint64 height)
{
    return QObject::tr("The image is too large (%1 × %2 pixels). PairPaint opens images up to %3 megapixels.")
        .arg(width).arg(height).arg(maxImagePixels() / 1'000'000);
}

QString openFilter()
{
    QStringList patterns{QStringLiteral("*.pairpaint"), QStringLiteral("*.psd")};
    for (const QByteArray &fmt : QImageReader::supportedImageFormats())
        patterns << QStringLiteral("*.") + QString::fromLatin1(fmt);
    if (Heif::hasDecoder())
        patterns << QStringLiteral("*.heic") << QStringLiteral("*.heif") << QStringLiteral("*.hif");
    patterns.removeDuplicates();
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

Document *load(const QString &path, QString *error, QString *warning)
{
    // A damaged file can make a reader (ours or Qt's) ask for an impossible amount of memory.
    // That must be a "damaged file" error, not a crash.
    try {
        return loadUnchecked(path, error, warning);
    } catch (const std::bad_alloc &) {
        *error = QObject::tr("The file is damaged or too large to open (out of memory).");
        return nullptr;
    }
}

Document *loadUnchecked(const QString &path, QString *error, QString *warning)
{
    if (isProjectFile(path))
        return loadProject(path, error);
    if (isPsdFile(path))
        return Psd::read(path, error, warning);

    QImage img;
    if (Heif::hasDecoder() && Heif::isHeif(path)) {
        img = Heif::read(path, error);
        if (img.isNull())
            return nullptr;
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);  // honor EXIF orientation
        const QSize size = reader.size();
        if (size.isValid() && qint64(size.width()) * size.height() > maxImagePixels()) {
            *error = tooLargeMessage(size.width(), size.height());
            return nullptr;
        }
        // Qt's own decoding limit (256 MB by default) would refuse large photos PairPaint can edit.
        QImageReader::setAllocationLimit(int(std::min<qint64>(maxImagePixels() * 4 / (1024 * 1024) + 1, 1 << 30)));
        img = reader.read();
        if (img.isNull()) {
            *error = reader.errorString();
            return nullptr;
        }
#ifdef Q_OS_MACOS
        // Qt's macOS HEIF plugin already converts the pixels to sRGB but still labels them with
        // the photo's own color space; converting again would oversaturate them.
        if (reader.format() == "heic" || reader.format() == "heif")
            img.setColorSpace(QColorSpace::SRgb);
#endif
    }
    // PairPaint edits in sRGB. Photos with a wider color space (Display P3 from phones, Adobe RGB)
    // are converted, or their colors would look dull.
    const QColorSpace cs = img.colorSpace();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const bool rgb = cs.colorModel() == QColorSpace::ColorModel::Rgb;
#else
    const bool rgb = true;
#endif
    if (cs.isValid() && rgb && cs != QColorSpace(QColorSpace::SRgb)) {
        img = img.convertToFormat(QImage::Format_ARGB32);
        img.convertToColorSpace(QColorSpace::SRgb);
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
            << encodePng(l.isAdjustment() || alphaBounds(l.image).isEmpty() ? QImage() : l.image);  // empty: blank
        out << encodePng(l.mask) << l.maskEnabled << qint32(l.adjustment.type)
            << QList<qint32>(l.adjustment.params.begin(), l.adjustment.params.end()) << l.text.isValid();
        if (l.text.isValid())
            out << l.text.text << l.text.font.toString() << qint32(l.text.font.pixelSize()) << l.text.color
                << l.text.pos << l.text.antialias;
        const LayerStyle &st = l.style;
        out << st.shadow << st.shadowColor << st.glow << st.glowColor << st.stroke << st.strokeColor;
        for (int v : {st.shadowOpacity, st.shadowAngle, st.shadowDistance, st.shadowSize, st.glowOpacity,
                      st.glowSize, st.strokeOpacity, st.strokeSize, 0, 0, 0})  // 3 spare slots
            out << qint32(v);
        out << qint32(l.kind) << l.collapsed;
        out << l.clipped << double(l.fillOpacity);
    }
    if (out.status() != QDataStream::Ok || !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

bool hasQuality(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "jpg" || suffix == "jpeg" || suffix == "webp";
}

bool exportImage(const Document *doc, const QString &path, QString *error, int quality)
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
    if (hasQuality(path))
        writer.setQuality(quality > 0 ? std::min(quality, 100) : 92);
    if (!writer.write(img)) {
        *error = writer.errorString();
        return false;
    }
    return true;
}

} // namespace FileIO
