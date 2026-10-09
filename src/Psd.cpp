// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Psd.h"

#include "Document.h"
#include "FileIO.h"

#include <QDataStream>
#include <QFile>
#include <QPainterPath>
#include <QMap>
#include <QObject>
#include <QSaveFile>
#include <QVariant>
#include <QtEndian>
#include <cmath>
#include <limits>
#include <new>

namespace {

const QList<QPair<QByteArray, QPainter::CompositionMode>> &blendKeys()
{
    static const QList<QPair<QByteArray, QPainter::CompositionMode>> keys = {
        {"norm", QPainter::CompositionMode_SourceOver}, {"pass", Blend::PassThrough},
        {"diss", Blend::Dissolve},                      {"dark", QPainter::CompositionMode_Darken},
        {"mul ", QPainter::CompositionMode_Multiply},   {"idiv", QPainter::CompositionMode_ColorBurn},
        {"lbrn", Blend::LinearBurn},                    {"dkCl", Blend::DarkerColor},
        {"lite", QPainter::CompositionMode_Lighten},    {"scrn", QPainter::CompositionMode_Screen},
        {"div ", QPainter::CompositionMode_ColorDodge}, {"lddg", QPainter::CompositionMode_Plus},
        {"lgCl", Blend::LighterColor},                  {"over", QPainter::CompositionMode_Overlay},
        {"sLit", QPainter::CompositionMode_SoftLight},  {"hLit", QPainter::CompositionMode_HardLight},
        {"vLit", Blend::VividLight},                    {"lLit", Blend::LinearLight},
        {"pLit", Blend::PinLight},                      {"hMix", Blend::HardMix},
        {"diff", QPainter::CompositionMode_Difference}, {"smud", QPainter::CompositionMode_Exclusion},
        {"fsub", Blend::Subtract},                      {"fdiv", Blend::Divide},
        {"hue ", Blend::Hue},                           {"sat ", Blend::Saturation},
        {"colr", Blend::Color},                         {"lum ", Blend::Luminosity},
    };
    return keys;
}

QPainter::CompositionMode modeForKey(const QByteArray &key)
{
    for (const auto &[k, m] : blendKeys())
        if (k == key)
            return m;
    return QPainter::CompositionMode_SourceOver;
}

QByteArray keyForMode(QPainter::CompositionMode mode)
{
    for (const auto &[k, m] : blendKeys())
        if (m == mode)
            return k;
    return "norm";
}

// --- Reading ---------------------------------------------------------------

class Reader {
public:
    explicit Reader(QFile &f) : m_file(f), m_in(&f) { m_in.setByteOrder(QDataStream::BigEndian); }

    // Every read checks for the end of the file: a damaged file must stop the reader with an
    // error, never leave it looping on values it can no longer read.
    quint8 u8() { return read<quint8>(); }
    quint16 u16() { return read<quint16>(); }
    qint16 i16() { return read<qint16>(); }
    quint32 u32() { return read<quint32>(); }
    qint32 i32() { return read<qint32>(); }
    qint64 i64() { return read<qint64>(); }
    double f64() { return read<double>(); }
    QByteArray bytes(qint64 n)
    {
        if (n < 0 || n > m_file.size() - m_file.pos() || n > std::numeric_limits<int>::max())
            throw QObject::tr("The file is truncated.");
        QByteArray b(n, Qt::Uninitialized);
        if (m_in.readRawData(b.data(), int(n)) != n)
            throw QObject::tr("The file is truncated.");
        return b;
    }
    qint64 pos() const { return m_file.pos(); }
    void seek(qint64 p)
    {
        m_file.seek(p);
        m_in.resetStatus();  // a skipped damaged block shouldn't affect what follows
    }
    void skip(qint64 n) { seek(m_file.pos() + n); }

private:
    template <typename T>
    T read()
    {
        T v{};
        m_in >> v;
        if (m_in.status() != QDataStream::Ok)
            throw QObject::tr("The file is truncated or damaged.");
        return v;
    }

    QFile &m_file;
    QDataStream m_in;
};

QByteArray unpackBits(const QByteArray &src, int &pos, int outLen)
{
    QByteArray out;
    out.reserve(outLen);
    while (out.size() < outLen && pos < src.size()) {
        const int n = qint8(src[pos++]);
        if (n >= 0) {
            out.append(src.mid(pos, n + 1));
            pos += n + 1;
        } else if (n != -128 && pos < src.size()) {
            out.append(QByteArray(1 - n, src[pos++]));
        }
    }
    out.resize(outLen);
    return out;
}

// Converts a plane with `depth` bits per sample to 8 bits per sample.
QByteArray to8bit(const QByteArray &plane, int depth)
{
    if (depth == 8)
        return plane;
    QByteArray out(plane.size() / 2, Qt::Uninitialized);
    for (int i = 0; i < out.size(); ++i)
        out[i] = plane[2 * i];  // big-endian: high byte first
    return out;
}

// Decodes one channel of w x h samples. `len` is the data length after the compression field.
QByteArray readChannel(Reader &r, int compression, int w, int h, int depth, qint64 len)
{
    if (w <= 0 || h <= 0)
        return {};
    if (qint64(w) * h > 2 * FileIO::maxImagePixels())
        throw QObject::tr("The file is damaged (a layer is impossibly large).");
    const int bpr = w * depth / 8;  // bytes per row
    switch (compression) {
    case 0:
        return to8bit(r.bytes(qint64(bpr) * h), depth);
    case 1: {
        QList<int> counts(h);
        qint64 total = 0;
        for (int &c : counts) {
            c = r.u16();
            total += c;
        }
        const QByteArray data = r.bytes(total);
        // PackBits expands at most 128 bytes from 2, so the claimed size must be reachable.
        if (qint64(bpr) * h > 64 * total + h)
            throw QObject::tr("The file is damaged (compressed data too short).");
        QByteArray out;
        out.reserve(qint64(bpr) * h);
        int pos = 0;
        for (int y = 0; y < h; ++y) {
            const int start = pos;
            out.append(unpackBits(data, pos, bpr));
            pos = start + counts[y];
        }
        return to8bit(out, depth);
    }
    case 2:
    case 3: {
        QByteArray packed = r.bytes(len);
        const quint32 expected = quint32(bpr) * h;
        // Deflate expands at most about 1032:1, so a bigger claim is a lie; reject it before
        // qUncompress allocates the claimed size.
        if (qint64(expected) > qint64(packed.size()) * 1100 + 65536)
            throw QObject::tr("The file is damaged (compressed data too short).");
        QByteArray header(4, 0);
        qToBigEndian(expected, header.data());
        QByteArray out = qUncompress(header + packed);
        if (out.size() != qsizetype(expected))
            throw QObject::tr("Could not decompress ZIP-compressed layer data.");
        if (compression == 3) {  // undo delta prediction per row
            for (int y = 0; y < h; ++y) {
                uchar *row = reinterpret_cast<uchar *>(out.data()) + qint64(y) * bpr;
                if (depth == 8) {
                    for (int x = 1; x < w; ++x)
                        row[x] = uchar(row[x] + row[x - 1]);
                } else {
                    for (int x = 1; x < w; ++x) {
                        const quint16 prev = qFromBigEndian<quint16>(row + 2 * (x - 1));
                        const quint16 cur = qFromBigEndian<quint16>(row + 2 * x);
                        qToBigEndian<quint16>(quint16(cur + prev), row + 2 * x);
                    }
                }
            }
        }
        return to8bit(out, depth);
    }
    default:
        throw QObject::tr("Unsupported compression method %1.").arg(compression);
    }
}

// A premultiplied canvas-size image filled with `color`, or an error message if memory runs out.
QImage newImage(const QSize &size, const QColor &color)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        throw QObject::tr("Not enough memory to open this file.");
    img.fill(color);
    return img;
}

// Builds a straight-alpha ARGB32 image (size `canvas`) from channel planes covering `rect`.
QImage assemble(const QMap<int, QByteArray> &planes, const QRect &rect, const QSize &canvas, int colorMode)
{
    QImage img(canvas, QImage::Format_ARGB32);
    if (img.isNull())
        throw QObject::tr("Not enough memory to open this file.");
    img.fill(Qt::transparent);
    const QRect visible = rect & img.rect();
    const QByteArray empty;
    auto plane = [&](int id) -> const QByteArray & { auto it = planes.find(id); return it == planes.end() ? empty : *it; };
    const QByteArray &c0 = plane(0), &c1 = plane(1), &c2 = plane(2), &c3 = plane(3), &alpha = plane(-1);
    const qint64 n = qint64(rect.width()) * rect.height();
    auto at = [&](const QByteArray &p, qint64 i, int def) { return p.size() >= n ? uchar(p[i]) : def; };
    for (int y = visible.top(); y <= visible.bottom(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = visible.left(); x <= visible.right(); ++x) {
            const qint64 i = qint64(y - rect.top()) * rect.width() + (x - rect.left());
            int r, g, b;
            if (colorMode == 1) {
                r = g = b = at(c0, i, 0);
            } else if (colorMode == 4) {  // CMYK, stored inverted (255 = no ink)
                const int k = at(c3, i, 255);
                r = at(c0, i, 255) * k / 255;
                g = at(c1, i, 255) * k / 255;
                b = at(c2, i, 255) * k / 255;
            } else {
                r = at(c0, i, 0);
                g = at(c1, i, 0);
                b = at(c2, i, 0);
            }
            row[x] = qRgba(r, g, b, at(alpha, i, 255));
        }
    }
    return img;
}

struct ChannelInfo {
    int id;
    quint32 length;
};

// Photoshop "descriptors": the nested key/value structures used for layer effects and
// newer adjustment settings. Objects become QVariantMaps, lists QVariantLists, numbers doubles.
class DescriptorParser {
public:
    explicit DescriptorParser(Reader &r) : m_r(r) {}

    QVariantMap descriptor()
    {
        const Nesting nest(m_depth);
        unicode();  // class name
        QVariantMap map;
        map.insert(QStringLiteral("_class"), id());
        const quint32 count = m_r.u32();
        for (quint32 k = 0; k < count; ++k) {
            const QString key = id();
            map.insert(key, value(m_r.bytes(4)));
        }
        return map;
    }

private:
    // Objects and lists can nest; a damaged file could nest them deep enough to overflow the
    // stack, while real descriptors are only a few levels deep.
    struct Nesting {
        explicit Nesting(int &depth) : m_depth(depth)
        {
            if (++m_depth > 64)
                throw QObject::tr("Damaged descriptor.");
        }
        ~Nesting() { --m_depth; }
        int &m_depth;
    };

    QString unicode()
    {
        const quint32 n = m_r.u32();
        if (n > 100000)
            throw QObject::tr("Damaged descriptor.");
        QString s;
        for (quint32 k = 0; k < n; ++k)
            s += QChar(m_r.u16());
        while (s.endsWith(QChar(0)))
            s.chop(1);
        return s;
    }

    QString id()
    {
        const quint32 n = m_r.u32();
        return QString::fromLatin1(m_r.bytes(n == 0 ? 4 : n));
    }

    QVariant value(const QByteArray &type)
    {
        if (type == "Objc" || type == "GlbO")
            return descriptor();
        if (type == "VlLs") {
            const Nesting nest(m_depth);
            QVariantList list;
            const quint32 n = m_r.u32();
            for (quint32 k = 0; k < n; ++k)
                list << value(m_r.bytes(4));
            return list;
        }
        if (type == "doub")
            return m_r.f64();
        if (type == "UntF") {
            m_r.bytes(4);  // unit (#Pxl, #Prc, #Ang, ...)
            return m_r.f64();
        }
        if (type == "UnFl") {
            m_r.bytes(4);
            const quint32 n = m_r.u32();
            QVariantList list;
            for (quint32 k = 0; k < n; ++k)
                list << m_r.f64();
            return list;
        }
        if (type == "TEXT")
            return unicode();
        if (type == "enum") {
            id();  // enum type
            return id();
        }
        if (type == "long")
            return double(m_r.i32());
        if (type == "comp")
            return double(m_r.i64());
        if (type == "bool")
            return m_r.u8() != 0;
        if (type == "type" || type == "GlbC") {
            unicode();
            return id();
        }
        if (type == "alis" || type == "tdta" || type == "Pth ")
            return m_r.bytes(m_r.u32());
        if (type == "obj ") {
            reference();
            return QVariant();
        }
        throw QObject::tr("Unsupported descriptor value '%1'.").arg(QString::fromLatin1(type));
    }

    void reference()
    {
        const quint32 n = m_r.u32();
        for (quint32 k = 0; k < n; ++k) {
            const QByteArray form = m_r.bytes(4);
            if (form == "prop") { unicode(); id(); id(); }
            else if (form == "Clss") { unicode(); id(); }
            else if (form == "Enmr") { unicode(); id(); id(); id(); }
            else if (form == "rele") { unicode(); id(); m_r.u32(); }
            else if (form == "Idnt" || form == "indx") { m_r.u32(); }
            else if (form == "name") { unicode(); id(); unicode(); }
            else throw QObject::tr("Unsupported descriptor reference.");
        }
    }

    Reader &m_r;
    int m_depth = 0;
};

double num(const QVariantMap &m, const char *key, double fallback = 0)
{
    const QVariant v = m.value(QString::fromLatin1(key));
    const double d = v.isValid() ? v.toDouble() : fallback;
    return std::isfinite(d) ? d : fallback;
}

// A number from a file as an int in [lo, hi] (rounding a huge double to int is undefined).
int toInt(double v, int lo, int hi) { return int(std::lround(std::clamp(v, double(lo), double(hi)))); }

QColor descriptorColor(const QVariantMap &m)
{
    const QVariantMap c = m.value(QStringLiteral("Clr ")).toMap();
    if (c.contains(QStringLiteral("redFloat")))
        return QColor::fromRgbF(std::clamp(num(c, "redFloat"), 0.0, 1.0), std::clamp(num(c, "greenFloat"), 0.0, 1.0),
                                std::clamp(num(c, "blueFloat"), 0.0, 1.0));
    return QColor(toInt(num(c, "Rd  "), 0, 255), toInt(num(c, "Grn "), 0, 255), toInt(num(c, "Bl  "), 0, 255));
}

struct LayerRecord {
    QRect rect;
    QList<ChannelInfo> channels;
    QByteArray blendKey;
    int opacity = 255;
    int flags = 0;
    QString name;
    QRect maskRect;
    int maskDefault = 255;
    bool maskDisabled = false;
    int sectionType = 0;  // 1/2: group start, 3: group end marker
    QByteArray sectionBlendKey;
    bool clipped = false;
    int fillOpacity = 255;
    Adjustment adjustment;
    QColor fillColor;       // solid color fill layer
    QVariantMap effects;    // layer effects descriptor
    QString unsupported;    // name of an adjustment PairPaint can't apply
    QString fillWithoutPixelsNote;
    QString approximate;     // supported, but rendered approximately
    QPainterPath vectorMask;  // in document pixels; empty if none
    bool vectorMaskInverted = false;
};

// A vector mask: Bezier paths, stored as 26-byte records with coordinates relative to the
// document size. Returns the path in units of the document (0..1), filled = visible.
QPainterPath readVectorMask(Reader &r, qint64 end)
{
    r.u32();  // version
    const quint32 flags = r.u32();
    QPainterPath path;
    if (flags & 4)
        return path;  // disabled
    struct Knot {
        QPointF before, anchor, after;
    };
    auto point = [&] {
        const double y = r.i32() / 16777216.0, x = r.i32() / 16777216.0;  // fixed point 8.24
        return QPointF(x, y);
    };
    QList<Knot> knots;
    bool closed = false;
    int remaining = 0;
    auto flush = [&] {
        if (knots.isEmpty())
            return;
        path.moveTo(knots[0].anchor);
        for (int k = 1; k < knots.size(); ++k)
            path.cubicTo(knots[k - 1].after, knots[k].before, knots[k].anchor);
        if (closed) {
            path.cubicTo(knots.last().after, knots[0].before, knots[0].anchor);
            path.closeSubpath();
        }
        knots.clear();
    };
    while (r.pos() + 26 <= end) {
        const qint64 next = r.pos() + 26;
        const int selector = r.u16();
        if (selector == 0 || selector == 3) {  // start of a closed / open subpath
            flush();
            closed = selector == 0;
            remaining = r.u16();
        } else if ((selector == 1 || selector == 2 || selector == 4 || selector == 5) && remaining > 0) {
            Knot k;
            k.before = point();
            k.anchor = point();
            k.after = point();
            knots << k;
            --remaining;
        } else if (selector == 8) {
            if (r.u16() == 1)
                path.setFillRule(Qt::OddEvenFill);  // initial fill rule: everything visible...
        }
        r.seek(next);
    }
    flush();
    return path;
}

QImage vectorMaskImage(const QPainterPath &unitPath, bool inverted, const QSize &size)
{
    QImage mask(size, QImage::Format_ARGB32_Premultiplied);
    mask.fill(inverted ? Qt::white : Qt::black);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size.width(), size.height());
    p.fillPath(unitPath, inverted ? Qt::black : Qt::white);
    return mask;
}

// Photoshop adjustment layers that PairPaint can't apply (yet), by their data key.
QString unsupportedAdjustmentName(const QByteArray &key)
{
    static const QList<QPair<QByteArray, const char *>> names = {
        {"blnc", "Color Balance"}, {"selc", "Selective Color"}, {"mixr", "Channel Mixer"},
        {"vibA", "Vibrance"},       {"grdm", "Gradient Map"},    {"phfl", "Photo Filter"},
        {"expA", "Exposure"},       {"blwh", "Black & White"},   {"clrL", "Color Lookup"},
    };
    for (const auto &[k, n] : names)
        if (k == key)
            return QString::fromLatin1(n);
    return {};
}

// Parses the data of one "additional layer information" block into the record.
void readLayerInfo(Reader &r, const QByteArray &key, qint64 dataEnd, LayerRecord &rec)
{
    if (key == "luni") {
        const quint32 n = r.u32();
        QString name;
        for (quint32 k = 0; k < n && r.pos() + 2 <= dataEnd; ++k)
            name += QChar(r.u16());
        if (!name.isEmpty())
            rec.name = name;
    } else if (key == "lsct" || key == "lsdk") {
        rec.sectionType = int(r.u32());
        if (r.pos() + 8 <= dataEnd && r.bytes(4) == "8BIM")
            rec.sectionBlendKey = r.bytes(4);
    } else if (key == "iOpa") {
        rec.fillOpacity = r.u8();
    } else if (key == "nvrt") {
        rec.adjustment = {Adjustment::Invert, {}};
    } else if (key == "post") {
        rec.adjustment = {Adjustment::Posterize, {std::clamp(int(r.u16()), 2, 32)}};
    } else if (key == "thrs") {
        rec.adjustment = {Adjustment::Threshold, {std::clamp(int(r.u16()), 1, 255)}};
    } else if (key == "brit") {
        const int brightness = r.i16(), contrast = r.i16();
        if (rec.adjustment.type != Adjustment::BrightnessContrast)  // CgEd (newer data) wins
            rec.adjustment = {Adjustment::BrightnessContrast, {brightness, contrast, 1}};  // old files: legacy
    } else if (key == "CgEd") {
        r.u32();  // descriptor version
        const QVariantMap d = DescriptorParser(r).descriptor();
        const bool legacy = d.value(QStringLiteral("useLegacy")).toBool();
        rec.adjustment = {Adjustment::BrightnessContrast,
                          {toInt(num(d, "Brgh"), -150, 150), toInt(num(d, "Cntr"), -100, 100), legacy ? 1 : 0}};
        if (!legacy)
            rec.approximate = QStringLiteral("Brightness/Contrast (approximated)");
    } else if (key == "levl") {
        r.u16();  // version
        // Records: all channels, then red, green, blue. Each: input black/white,
        // output black/white, gamma x 100.
        QList<int> params;
        for (int c = 0; c < 4; ++c) {
            const int inBlack = r.u16(), inWhite = r.u16(), outBlack = r.u16(), outWhite = r.u16(), gamma = r.u16();
            params << std::clamp(inBlack, 0, 253) << std::clamp(inWhite, 2, 255) << std::clamp(gamma, 10, 999)
                   << std::clamp(outBlack, 0, 255) << std::clamp(outWhite, 0, 255);
        }
        rec.adjustment = {Adjustment::Levels, params};
    } else if (key == "curv") {
        r.u8();   // padding
        r.u16();  // version
        const quint32 channels = r.u32();  // bit 0: all channels, bits 1-3: red, green, blue
        QList<int> curves[4];
        for (int c = 0; c < 32; ++c) {
            if (!(channels & (1u << c)))
                continue;
            const int count = r.u16();
            QList<int> points;
            for (int k = 0; k < count && k < 64; ++k) {
                const int out = r.u16(), in = r.u16();
                points << in << out;
            }
            if (c < 4)
                curves[c] = points;
        }
        QList<int> params = curves[0].size() >= 4 ? curves[0] : QList<int>{0, 0, 255, 255};
        if (!curves[1].isEmpty() || !curves[2].isEmpty() || !curves[3].isEmpty()) {
            params << -1;
            for (int c = 1; c < 4; ++c) {
                const QList<int> pts = curves[c].size() >= 4 ? curves[c] : QList<int>{0, 0, 255, 255};
                params << int(pts.size() / 2) << pts;
            }
        }
        rec.adjustment = {Adjustment::Curves, params};
    } else if (key == "hue2") {
        r.u16();                       // version
        const bool colorize = r.u8();
        r.u8();
        r.i16(); r.i16(); r.i16();     // colorize settings
        const int hue = r.i16(), saturation = r.i16(), lightness = r.i16();  // master
        QList<int> params{hue, saturation, lightness};
        for (int k = 0; k < 6 && r.pos() + 14 <= dataEnd; ++k) {  // Reds, Yellows, Greens, ...
            const int a = r.u16(), b = r.u16(), c = r.u16(), d = r.u16();
            const int h = r.i16(), s = r.i16(), l = r.i16();
            if (h || s || l)
                params << a << b << c << d << h << s << l;
        }
        if (colorize) {
            rec.unsupported = QStringLiteral("Hue/Saturation (Colorize)");
        } else {
            rec.adjustment = {Adjustment::HueSaturation, params};
            if (params.size() > 3)
                rec.approximate = QStringLiteral("Hue/Saturation color ranges (approximated)");
        }
    } else if (key == "vmsk" || key == "vsms") {
        rec.vectorMask = readVectorMask(r, dataEnd);
    } else if (key == "GdFl" || key == "PtFl") {
        // Photoshop normally also stores the fill's pixels; only warn if it didn't.
        rec.fillWithoutPixelsNote = key == "GdFl" ? QStringLiteral("Gradient Fill") : QStringLiteral("Pattern Fill");
    } else if (key == "SoCo") {
        r.u32();
        rec.fillColor = descriptorColor(DescriptorParser(r).descriptor());
    } else if (key == "lfx2" || key == "lmfx") {
        r.u32();  // object effects version
        r.u32();  // descriptor version
        rec.effects = DescriptorParser(r).descriptor();
    } else {
        const QString name = unsupportedAdjustmentName(key);
        if (!name.isEmpty())
            rec.unsupported = name;
    }
}

// Maps Photoshop's drop shadow, outer glow and stroke to PairPaint's layer style.
LayerStyle styleFromEffects(const QVariantMap &fx, int globalAngle, QStringList *notes)
{
    LayerStyle st;
    if (fx.isEmpty() || !fx.value(QStringLiteral("masterFXSwitch"), true).toBool())
        return st;
    // Newer files keep each kind of effect in a list ("dropShadowMulti"); older ones a single object.
    auto effect = [&](const char *single, const char *multi) {
        QVariantList candidates = fx.value(QString::fromLatin1(multi)).toList();
        candidates << fx.value(QString::fromLatin1(single));
        for (const QVariant &v : candidates) {
            const QVariantMap m = v.toMap();
            if (!m.isEmpty() && m.value(QStringLiteral("enab")).toBool())
                return m;
        }
        return QVariantMap();
    };
    const QVariantMap shadow = effect("DrSh", "dropShadowMulti");
    if (!shadow.isEmpty()) {
        st.shadow = true;
        st.shadowColor = descriptorColor(shadow);
        st.shadowOpacity = toInt(num(shadow, "Opct", 75), 0, 100);
        st.shadowAngle = shadow.value(QStringLiteral("uglg"), true).toBool() ? globalAngle : toInt(num(shadow, "lagl", 120), -360, 360);
        st.shadowDistance = toInt(num(shadow, "Dstn", 5), 0, 500);
        st.shadowSize = toInt(num(shadow, "blur", 5), 0, 250);
    }
    const QVariantMap glow = effect("OrGl", "outerGlowMulti");
    if (!glow.isEmpty()) {
        st.glow = true;
        st.glowColor = descriptorColor(glow);
        st.glowOpacity = toInt(num(glow, "Opct", 75), 0, 100);
        st.glowSize = toInt(num(glow, "blur", 5), 1, 250);
    }
    const QVariantMap stroke = effect("FrFX", "frameFXMulti");
    if (!stroke.isEmpty()) {
        st.stroke = true;
        st.strokeColor = descriptorColor(stroke);
        st.strokeOpacity = toInt(num(stroke, "Opct", 100), 0, 100);
        st.strokeSize = toInt(num(stroke, "Sz  ", 3), 1, 250);
        if (stroke.value(QStringLiteral("Styl")).toString() != QLatin1String("OutF"))
            *notes << QObject::tr("inside/center strokes (shown as outside strokes)");
    }
    static const char *others[] = {"IrSh", "IrGl", "ebbl", "ChFX", "SoFi", "GrFl", "patternFill",
                                   "innerShadowMulti", "solidFillMulti", "gradientFillMulti"};
    for (const char *k : others)
        if (!effect(k, "").isEmpty())
            *notes << QObject::tr("some layer effects (inner shadow/glow, bevel, satin, overlays)");
    return st;
}

QRect readRect(Reader &r)
{
    const qint64 top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    constexpr qint64 limit = 1 << 24;  // far beyond any real layer, small enough to never overflow
    if (std::abs(top) > limit || std::abs(left) > limit || bottom < top || right < left || bottom - top > limit
        || right - left > limit)
        throw QObject::tr("The file is damaged (invalid layer bounds).");
    return QRect(int(left), int(top), int(right - left), int(bottom - top));
}

// Reads the merged (flattened) image Photoshop stores after the layers.
QImage readMergedImage(Reader &r, qint64 offset, int channelCount, int width, int height, int depth, int colorMode)
{
    r.seek(offset);
    const QSize size(width, height);
    const int compression = r.u16();
    QMap<int, QByteArray> planes;
    const int used = std::min(channelCount, colorMode == 4 ? 5 : colorMode == 1 ? 2 : 4);
    const int colorChannels = colorMode == 4 ? 4 : colorMode == 1 ? 1 : 3;
    const int bpr = width * depth / 8;
    if (compression == 1) {
        QList<int> counts(qsizetype(channelCount) * height);
        for (int &c : counts)
            c = r.u16();
        for (int c = 0; c < channelCount; ++c) {
            // PackBits expands at most 128 bytes from 2, so the rows must hold enough data.
            qint64 total = 0;
            for (int y = 0; y < height; ++y)
                total += counts[qsizetype(c) * height + y];
            if (qint64(bpr) * height > 64 * total + height)
                throw QObject::tr("The file is damaged (compressed data too short).");
            QByteArray plane;
            for (int y = 0; y < height; ++y) {
                const QByteArray row = r.bytes(counts[qsizetype(c) * height + y]);
                int pos = 0;
                plane.append(unpackBits(row, pos, bpr));
            }
            if (c < used)
                planes[c < colorChannels ? c : -1] = to8bit(plane, depth);
        }
    } else if (compression == 0) {
        for (int c = 0; c < channelCount; ++c) {
            const QByteArray plane = r.bytes(qint64(bpr) * height);
            if (c < used)
                planes[c < colorChannels ? c : -1] = to8bit(plane, depth);
        }
    } else {
        throw QObject::tr("Unsupported compression of the merged image.");
    }
    QImage img = assemble(planes, QRect(QPoint(0, 0), size), size, colorMode);
    if (planes.contains(-1)) {
        // Photoshop stores the merged colors blended with white where they are transparent;
        // undo that so the colors are the real ones.
        for (int y = 0; y < img.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int a = qAlpha(row[x]);
                if (a == 0 || a == 255)
                    continue;
                auto unmatte = [a](int c) { return std::clamp((c - (255 - a)) * 255 / a, 0, 255); };
                row[x] = qRgba(unmatte(qRed(row[x])), unmatte(qGreen(row[x])), unmatte(qBlue(row[x])), a);
            }
        }
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

Document *readPsd(QFile &f, QStringList &notes, QImage *mergedOnly = nullptr)
{
    Reader r(f);
    if (r.bytes(4) != "8BPS")
        throw QObject::tr("Not a Photoshop file.");
    const int version = r.u16();
    if (version == 2)
        throw QObject::tr("Large document (PSB) files are not supported.");
    if (version != 1)
        throw QObject::tr("Unknown PSD version %1.").arg(version);
    r.skip(6);
    const int channelCount = r.u16();
    if (channelCount < 1 || channelCount > 56)  // the limits Photoshop documents
        throw QObject::tr("The file is damaged (invalid channel count).");
    const int height = int(r.u32());
    const int width = int(r.u32());
    const int depth = r.u16();
    const int colorMode = r.u16();
    if (depth != 8 && depth != 16)
        throw QObject::tr("%1-bit images are not supported (only 8 and 16 bit).").arg(depth);
    if (colorMode != 1 && colorMode != 3 && colorMode != 4)
        throw QObject::tr("Only RGB, grayscale and CMYK Photoshop files are supported.");
    if (width <= 0 || height <= 0 || width > 300000 || height > 300000)
        throw QObject::tr("Invalid image size.");
    if (qint64(width) * height > FileIO::maxImagePixels())
        throw QObject::tr("The image is too large (%1 × %2 pixels).").arg(width).arg(height);
    const QSize size(width, height);

    r.skip(r.u32());  // color mode data
    int globalAngle = 120;  // Photoshop's default lighting angle for effects
    {
        const quint32 len = r.u32();
        const qint64 end = r.pos() + len;
        while (r.pos() + 12 <= end) {
            if (r.bytes(4) != "8BIM")
                break;
            const int id = r.u16();
            const int nameLen = r.u8();
            r.skip(nameLen + ((nameLen + 1) % 2));  // Pascal string padded to even length
            const quint32 size = r.u32();
            const qint64 next = r.pos() + size + (size % 2);
            if (id == 1037 && size >= 4)  // global light angle
                globalAngle = r.i32();
            r.seek(next);
        }
        r.seek(end);
    }
    const quint32 layerMaskLen = r.u32();
    const qint64 layerMaskEnd = r.pos() + layerMaskLen;
    if (mergedOnly) {
        *mergedOnly = readMergedImage(r, layerMaskEnd, channelCount, width, height, depth, colorMode);
        return nullptr;
    }

    DocState state;
    state.size = size;
    if (layerMaskLen > 0) {
        const quint32 layerInfoLen = r.u32();
        if (layerInfoLen > 0) {
            const int count = std::abs(r.i16());
            QList<LayerRecord> records;
            for (int i = 0; i < count; ++i) {
                LayerRecord rec;
                rec.rect = readRect(r);
                const int nch = r.u16();
                for (int c = 0; c < nch; ++c) {
                    const int id = r.i16();
                    const quint32 len = r.u32();
                    rec.channels.append({id, len});
                }
                r.bytes(4);  // "8BIM"
                rec.blendKey = r.bytes(4);
                rec.opacity = r.u8();
                rec.clipped = r.u8() != 0;
                rec.flags = r.u8();
                r.u8();  // filler
                const quint32 extraLen = r.u32();
                const qint64 extraEnd = r.pos() + extraLen;

                const quint32 maskLen = r.u32();
                const qint64 maskEnd = r.pos() + maskLen;
                if (maskLen >= 18) {
                    rec.maskRect = readRect(r);
                    rec.maskDefault = r.u8();
                    rec.maskDisabled = r.u8() & 2;
                }
                r.seek(maskEnd);
                r.skip(r.u32());  // blending ranges
                const int nameLen = r.u8();
                rec.name = QString::fromLocal8Bit(r.bytes(nameLen));
                r.skip((4 - (1 + nameLen) % 4) % 4);

                while (r.pos() + 12 <= extraEnd) {
                    const QByteArray sig = r.bytes(4);
                    if (sig != "8BIM" && sig != "8B64")
                        break;
                    const QByteArray key = r.bytes(4);
                    const quint32 len = r.u32();
                    const qint64 dataEnd = r.pos() + len;
                    try {
                        readLayerInfo(r, key, dataEnd, rec);
                    } catch (const QString &) {
                        // An unreadable block only loses that feature, not the whole file.
                    }
                    r.seek(dataEnd);
                }
                r.seek(extraEnd);
                records.append(rec);
            }

            // Every layer is a full-canvas image. Layers without pixels of their own (groups,
            // adjustments, empty layers) share one blank image; the rest count against a budget,
            // so a small file can't claim thousands of huge layers.
            const QImage blank = newImage(size, Qt::transparent);
            QImage white;
            FileIO::PixelBudget budget;
            const qint64 canvasPixels = qint64(width) * height;
            for (const LayerRecord &rec : records) {
                QMap<int, QByteArray> planes;
                for (const ChannelInfo &ch : rec.channels) {
                    const qint64 start = r.pos();
                    const QRect area = ch.id == -2 ? rec.maskRect : rec.rect;
                    if (ch.length >= 2) {
                        const int compression = r.u16();
                        if (ch.id >= -2)
                            planes[ch.id] = readChannel(r, compression, area.width(), area.height(), depth, ch.length - 2);
                    }
                    r.seek(start + ch.length);
                }
                Layer l;
                // Groups: type 1/2 is the group header (open/closed), 3 the marker below its layers.
                if (rec.sectionType == 1 || rec.sectionType == 2) {
                    l.kind = LayerKind::Group;
                    l.collapsed = rec.sectionType == 2;
                } else if (rec.sectionType == 3) {
                    l.kind = LayerKind::GroupEnd;
                }
                l.name = rec.name.isEmpty() ? QObject::tr("Layer") : rec.name;
                l.opacity = rec.opacity / 255.0;
                l.fillOpacity = rec.fillOpacity / 255.0;
                l.visible = !(rec.flags & 2);
                l.clipped = rec.clipped && l.kind == LayerKind::Normal;
                l.mode = modeForKey(l.isGroup() && !rec.sectionBlendKey.isEmpty() ? rec.sectionBlendKey : rec.blendKey);
                const bool hasPixels = l.kind == LayerKind::Normal && rec.adjustment.type == Adjustment::None
                                       && !(rec.rect & QRect(QPoint(0, 0), size)).isEmpty();
                if (hasPixels) {
                    budget.take(canvasPixels);
                    l.image = assemble(planes, rec.rect, size, colorMode).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                } else {
                    l.image = blank;
                }
                if (rec.adjustment.type != Adjustment::None) {
                    l.adjustment = rec.adjustment;  // adjustment layers have no pixels of their own
                } else if (l.kind == LayerKind::Normal && rec.fillColor.isValid()
                           && (!rec.vectorMask.isEmpty() || alphaBounds(l.image).isEmpty())) {
                    // Solid color fill layers, including shape layers (a fill cut out by a vector
                    // outline): rebuilt the way Photoshop renders them, so edges are exact.
                    if (!hasPixels)
                        budget.take(canvasPixels);
                    l.image = newImage(size, rec.fillColor);
                } else if (!rec.unsupported.isEmpty()) {
                    notes << rec.unsupported;
                } else if (!rec.fillWithoutPixelsNote.isEmpty() && alphaBounds(l.image).isEmpty()) {
                    notes << rec.fillWithoutPixelsNote;
                }
                l.style = styleFromEffects(rec.effects, globalAngle, &notes);
                if (!rec.approximate.isEmpty())
                    notes << rec.approximate;
                if (planes.contains(-2) && !rec.maskRect.isEmpty()) {
                    budget.take(canvasPixels);
                    QImage mask = newImage(size, QColor(rec.maskDefault, rec.maskDefault, rec.maskDefault));
                    const QByteArray &m = planes[-2];
                    const QRect visible = rec.maskRect & mask.rect();
                    if (m.size() >= qint64(rec.maskRect.width()) * rec.maskRect.height()) {
                        for (int y = visible.top(); y <= visible.bottom(); ++y) {
                            QRgb *row = reinterpret_cast<QRgb *>(mask.scanLine(y));
                            for (int x = visible.left(); x <= visible.right(); ++x) {
                                const int v = uchar(m[qint64(y - rec.maskRect.top()) * rec.maskRect.width() + (x - rec.maskRect.left())]);
                                row[x] = qRgb(v, v, v);
                            }
                        }
                    }
                    l.mask = mask;
                    l.maskEnabled = !rec.maskDisabled;
                }
                if (!rec.vectorMask.isEmpty()) {
                    // Shape layers: the vector outline becomes (part of) the layer mask.
                    budget.take(canvasPixels);
                    const QImage vector = vectorMaskImage(rec.vectorMask, false, size);
                    if (l.mask.isNull()) {
                        l.mask = vector;
                    } else {
                        QPainter mp(&l.mask);
                        mp.setCompositionMode(QPainter::CompositionMode_Multiply);
                        mp.drawImage(0, 0, vector);
                    }
                    l.maskEnabled = true;
                }
                if (l.isAdjustment() && l.mask.isNull()) {
                    if (white.isNull())
                        white = newImage(size, Qt::white);
                    l.mask = white;  // shared until painted on
                }
                sanitizeLayer(l);
                state.layers.append(l);  // PSD stores layers bottom to top
            }
        }
    }

    if (state.layers.isEmpty()) {
        // No layers: use the merged image data.
        Layer l;
        l.name = QObject::tr("Background");
        l.image = readMergedImage(r, layerMaskEnd, channelCount, width, height, depth, colorMode);
        state.layers.append(l);
    }
    state.active = int(state.layers.size()) - 1;
    return new Document(state);
}

// --- Writing ---------------------------------------------------------------

QByteArray packBits(const uchar *data, int len)
{
    QByteArray out;
    int i = 0;
    while (i < len) {
        int run = 1;
        while (i + run < len && run < 128 && data[i + run] == data[i])
            ++run;
        if (run >= 3) {
            out.append(char(1 - run));
            out.append(char(data[i]));
            i += run;
            continue;
        }
        int lit = 0;
        while (i + lit < len && lit < 128) {
            if (i + lit + 2 < len && data[i + lit] == data[i + lit + 1] && data[i + lit] == data[i + lit + 2])
                break;
            ++lit;
        }
        out.append(char(lit - 1));
        out.append(reinterpret_cast<const char *>(data + i), lit);
        i += lit;
    }
    return out;
}

// RLE-encodes a w x h plane: row byte counts followed by the packed rows.
QByteArray encodePlane(const QByteArray &plane, int w, int h, QByteArray *countsOut = nullptr)
{
    QByteArray counts, data;
    for (int y = 0; y < h; ++y) {
        const QByteArray row = packBits(reinterpret_cast<const uchar *>(plane.constData()) + qint64(y) * w, w);
        char c[2];
        qToBigEndian<quint16>(quint16(row.size()), c);
        counts.append(c, 2);
        data.append(row);
    }
    if (countsOut) {
        *countsOut = counts;
        return data;
    }
    return counts + data;
}

// Extracts planes (R, G, B, A) from an image, unpremultiplied.
QList<QByteArray> planesOf(const QImage &src)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    QList<QByteArray> planes(4, QByteArray(qint64(w) * h, Qt::Uninitialized));
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const qint64 i = qint64(y) * w + x;
            planes[0][i] = char(qRed(row[x]));
            planes[1][i] = char(qGreen(row[x]));
            planes[2][i] = char(qBlue(row[x]));
            planes[3][i] = char(qAlpha(row[x]));
        }
    }
    return planes;
}

QByteArray maskPlane(const QImage &mask)
{
    const int w = mask.width(), h = mask.height();
    QByteArray plane(qint64(w) * h, Qt::Uninitialized);
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        for (int x = 0; x < w; ++x)
            plane[qint64(y) * w + x] = char((qRed(row[x]) * 77 + qGreen(row[x]) * 150 + qBlue(row[x]) * 29) >> 8);
    }
    return plane;
}

} // namespace

namespace Psd {

Document *read(const QString &path, QString *error, QString *warning)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return nullptr;
    }
    try {
        QStringList notes;
        Document *doc = readPsd(f, notes);
        notes.removeDuplicates();
        if (warning && !notes.isEmpty())
            *warning = QObject::tr("This file uses Photoshop features PairPaint can't reproduce yet, so it may look "
                                   "different: %1.").arg(notes.join(QStringLiteral(", ")));
        return doc;
    } catch (const QString &message) {
        *error = message;
        return nullptr;
    } catch (const std::bad_alloc &) {
        *error = QObject::tr("The file is damaged or too large to open (out of memory).");
        return nullptr;
    }
}

QImage readComposite(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    try {
        QImage merged;
        QStringList notes;
        readPsd(f, notes, &merged);
        return merged;
    } catch (const QString &message) {
        *error = message;
        return {};
    } catch (const std::bad_alloc &) {
        *error = QObject::tr("Out of memory.");
        return {};
    }
}

bool write(const Document *doc, const QString &path, QString *error, QString *warning)
{
    const int w = doc->size().width(), h = doc->size().height();
    QByteArray layerInfo;
    QDataStream li(&layerInfo, QIODevice::WriteOnly);
    li.setByteOrder(QDataStream::BigEndian);

    QList<Layer> layers;
    int skipped = 0, baked = 0;
    for (int i = 0; i < doc->layerCount(); ++i) {
        Layer l = doc->layer(i);
        if (l.isAdjustment()) {
            ++skipped;
            continue;
        }
        if (l.style.any()) {
            // PairPaint effects are not written as Photoshop effects; merge them into the pixels.
            l.image = doc->renderLayer(i);
            l.mask = QImage();
            l.style = LayerStyle();
            ++baked;
        }
        layers << l;
    }
    QStringList notes;
    if (skipped)
        notes << QObject::tr("%n adjustment layer(s) cannot be stored in PSD by PairPaint and were left out.",
                             nullptr, skipped);
    if (baked)
        notes << QObject::tr("Layer styles on %n layer(s) were merged into the layer pixels.", nullptr, baked);
    if (!notes.isEmpty())
        *warning = notes.join(QLatin1Char(' ')) + QObject::tr(" Save as .pairpaint to keep everything editable.");

    // Encode channel data first so the records can state each channel's length.
    QList<QList<QPair<int, QByteArray>>> channelData;
    // Group headers and end markers have no pixels: empty channels (an empty plane means
    // "raw, no data"), and a zero-sized layer rectangle below.
    for (const Layer &l : layers) {
        QList<QPair<int, QByteArray>> chans;
        if (l.kind == LayerKind::Normal) {
            const QList<QByteArray> planes = planesOf(l.image);
            chans = {{-1, planes[3]}, {0, planes[0]}, {1, planes[1]}, {2, planes[2]}};
        } else {
            chans = {{-1, {}}, {0, {}}, {1, {}}, {2, {}}};
        }
        if (!l.mask.isNull() && !l.isGroupEnd())
            chans.append({-2, maskPlane(l.mask)});
        for (auto &c : chans)
            if (!c.second.isEmpty())
                c.second = encodePlane(c.second, w, h);
        channelData << chans;
    }

    li << qint16(layers.size());
    for (int i = 0; i < layers.size(); ++i) {
        const Layer &l = layers[i];
        if (l.kind == LayerKind::Normal)
            li << qint32(0) << qint32(0) << qint32(h) << qint32(w);
        else
            li << qint32(0) << qint32(0) << qint32(0) << qint32(0);
        li << quint16(channelData[i].size());
        for (const auto &[id, data] : channelData[i])
            li << qint16(id) << quint32(2 + data.size());
        li.writeRawData("8BIM", 4);
        li.writeRawData(keyForMode(l.mode).constData(), 4);
        li << quint8(qRound(l.opacity * 255)) << quint8(l.clipped ? 1 : 0) << quint8(l.visible ? 0 : 2) << quint8(0);

        QByteArray extra;
        QDataStream ex(&extra, QIODevice::WriteOnly);
        ex.setByteOrder(QDataStream::BigEndian);
        if (!l.mask.isNull() && !l.isGroupEnd()) {
            ex << quint32(20) << qint32(0) << qint32(0) << qint32(h) << qint32(w)
               << quint8(255) << quint8(l.maskEnabled ? 0 : 2) << quint16(0);
        } else {
            ex << quint32(0);
        }
        ex << quint32(0);  // blending ranges
        const QString layerName = l.isGroupEnd() ? QStringLiteral("</Layer group>") : l.name;
        QByteArray name = layerName.toLocal8Bit().left(255);
        ex << quint8(name.size());
        ex.writeRawData(name.constData(), int(name.size()));
        for (int pad = (4 - (1 + name.size()) % 4) % 4; pad > 0; --pad)
            ex << quint8(0);
        // Unicode name
        ex.writeRawData("8BIM", 4);
        ex.writeRawData("luni", 4);
        quint32 len = 4 + 2 * quint32(layerName.size());
        const quint32 padded = (len + 3) & ~3u;
        ex << padded << quint32(layerName.size());
        for (QChar c : layerName)
            ex << quint16(c.unicode());
        for (quint32 k = len; k < padded; ++k)
            ex << quint8(0);
        if (l.fillOpacity < 1.0) {
            ex.writeRawData("8BIM", 4);
            ex.writeRawData("iOpa", 4);
            ex << quint32(4) << quint8(qRound(l.fillOpacity * 255)) << quint8(0) << quint16(0);
        }
        if (l.kind != LayerKind::Normal) {
            // Section divider: 1 = open group, 2 = closed group, 3 = end marker.
            ex.writeRawData("8BIM", 4);
            ex.writeRawData("lsct", 4);
            ex << quint32(12) << quint32(l.isGroupEnd() ? 3 : l.collapsed ? 2 : 1);
            ex.writeRawData("8BIM", 4);
            ex.writeRawData(keyForMode(l.mode).constData(), 4);
        }
        li << quint32(extra.size());
        li.writeRawData(extra.constData(), int(extra.size()));
    }
    for (const auto &chans : channelData)
        for (const auto &[id, data] : chans) {
            li << quint16(data.isEmpty() ? 0 : 1);  // raw (nothing) or RLE
            li.writeRawData(data.constData(), int(data.size()));
        }
    if (layerInfo.size() % 2)
        layerInfo.append(char(0));

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    QDataStream out(&f);
    out.setByteOrder(QDataStream::BigEndian);
    out.writeRawData("8BPS", 4);
    out << quint16(1);
    out.writeRawData("\0\0\0\0\0\0", 6);
    out << quint16(3) << quint32(h) << quint32(w) << quint16(8) << quint16(3);
    out << quint32(0);  // color mode data
    out << quint32(0);  // image resources
    out << quint32(4 + layerInfo.size() + 4);
    out << quint32(layerInfo.size());
    out.writeRawData(layerInfo.constData(), int(layerInfo.size()));
    out << quint32(0);  // global layer mask info

    // Merged image, composited over white, RLE.
    QImage merged(doc->size(), QImage::Format_RGB32);
    merged.fill(Qt::white);
    QPainter p(&merged);
    p.drawImage(0, 0, doc->flattened());
    p.end();
    const QList<QByteArray> planes = planesOf(merged);
    QByteArray counts, data;
    for (int c = 0; c < 3; ++c) {
        QByteArray cc;
        data.append(encodePlane(planes[c], w, h, &cc));
        counts.append(cc);
    }
    out << quint16(1);
    out.writeRawData(counts.constData(), int(counts.size()));
    out.writeRawData(data.constData(), int(data.size()));

    if (out.status() != QDataStream::Ok || !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace Psd
