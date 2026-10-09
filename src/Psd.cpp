// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Psd.h"

#include "Document.h"

#include <QDataStream>
#include <QFile>
#include <QMap>
#include <QObject>
#include <QSaveFile>
#include <QtEndian>

namespace {

const QList<QPair<QByteArray, QPainter::CompositionMode>> &blendKeys()
{
    static const QList<QPair<QByteArray, QPainter::CompositionMode>> keys = {
        {"norm", QPainter::CompositionMode_SourceOver}, {"mul ", QPainter::CompositionMode_Multiply},
        {"scrn", QPainter::CompositionMode_Screen},     {"over", QPainter::CompositionMode_Overlay},
        {"dark", QPainter::CompositionMode_Darken},     {"lite", QPainter::CompositionMode_Lighten},
        {"div ", QPainter::CompositionMode_ColorDodge}, {"idiv", QPainter::CompositionMode_ColorBurn},
        {"hLit", QPainter::CompositionMode_HardLight},  {"sLit", QPainter::CompositionMode_SoftLight},
        {"diff", QPainter::CompositionMode_Difference}, {"smud", QPainter::CompositionMode_Exclusion},
        {"lddg", QPainter::CompositionMode_Plus},
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

    quint8 u8() { quint8 v = 0; m_in >> v; return v; }
    quint16 u16() { quint16 v = 0; m_in >> v; return v; }
    qint16 i16() { qint16 v = 0; m_in >> v; return v; }
    quint32 u32() { quint32 v = 0; m_in >> v; return v; }
    qint32 i32() { qint32 v = 0; m_in >> v; return v; }
    QByteArray bytes(qint64 n)
    {
        if (n < 0 || n > m_file.size() - m_file.pos())
            throw QObject::tr("The file is truncated.");
        QByteArray b(n, Qt::Uninitialized);
        if (m_in.readRawData(b.data(), int(n)) != n)
            throw QObject::tr("The file is truncated.");
        return b;
    }
    qint64 pos() const { return m_file.pos(); }
    void seek(qint64 p) { m_file.seek(p); }
    void skip(qint64 n) { m_file.seek(m_file.pos() + n); }
    void check() const
    {
        if (m_in.status() != QDataStream::Ok)
            throw QObject::tr("The file is truncated or damaged.");
    }

private:
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
    const int bpr = w * depth / 8;  // bytes per row
    if (w <= 0 || h <= 0)
        return {};
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

// Builds a straight-alpha ARGB32 image (size `canvas`) from channel planes covering `rect`.
QImage assemble(const QMap<int, QByteArray> &planes, const QRect &rect, const QSize &canvas, int colorMode)
{
    QImage img(canvas, QImage::Format_ARGB32);
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
};

QRect readRect(Reader &r)
{
    const qint32 top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    return QRect(left, top, right - left, bottom - top);
}

Document *readPsd(QFile &f)
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
    const QSize size(width, height);

    r.skip(r.u32());  // color mode data
    r.skip(r.u32());  // image resources
    const quint32 layerMaskLen = r.u32();
    const qint64 layerMaskEnd = r.pos() + layerMaskLen;

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
                r.u8();  // clipping
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
                    if (key == "luni") {
                        const quint32 n = r.u32();
                        QString name;
                        for (quint32 k = 0; k < n && r.pos() + 2 <= dataEnd; ++k)
                            name += QChar(r.u16());
                        if (!name.isEmpty())
                            rec.name = name;
                    } else if (key == "lsct" || key == "lsdk") {
                        rec.sectionType = int(r.u32());
                    }
                    r.seek(dataEnd);
                }
                r.seek(extraEnd);
                records.append(rec);
            }
            r.check();

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
                if (rec.sectionType != 0)
                    continue;  // group markers carry no pixels
                Layer l;
                l.name = rec.name.isEmpty() ? QObject::tr("Layer") : rec.name;
                l.opacity = rec.opacity / 255.0;
                l.visible = !(rec.flags & 2);
                l.mode = modeForKey(rec.blendKey);
                l.image = assemble(planes, rec.rect, size, colorMode).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                if (planes.contains(-2) && !rec.maskRect.isEmpty()) {
                    QImage mask(size, QImage::Format_ARGB32_Premultiplied);
                    mask.fill(QColor(rec.maskDefault, rec.maskDefault, rec.maskDefault));
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
                state.layers.append(l);  // PSD stores layers bottom to top
            }
            r.check();
        }
    }

    if (state.layers.isEmpty()) {
        // No layers: use the merged image data.
        r.seek(layerMaskEnd);
        const int compression = r.u16();
        QMap<int, QByteArray> planes;
        const int used = std::min(channelCount, colorMode == 4 ? 5 : colorMode == 1 ? 2 : 4);
        const int colorChannels = colorMode == 4 ? 4 : colorMode == 1 ? 1 : 3;
        const int bpr = width * depth / 8;
        if (compression == 1) {
            QList<int> counts(channelCount * height);
            for (int &c : counts)
                c = r.u16();
            for (int c = 0; c < channelCount; ++c) {
                QByteArray plane;
                for (int y = 0; y < height; ++y) {
                    const QByteArray row = r.bytes(counts[c * height + y]);
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
        r.check();
        Layer l;
        l.name = QObject::tr("Background");
        l.image = assemble(planes, QRect(QPoint(0, 0), size), size, colorMode).convertToFormat(QImage::Format_ARGB32_Premultiplied);
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

Document *read(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return nullptr;
    }
    try {
        return readPsd(f);
    } catch (const QString &message) {
        *error = message;
        return nullptr;
    }
}

bool write(const Document *doc, const QString &path, QString *error, QString *warning)
{
    const int w = doc->size().width(), h = doc->size().height();
    QByteArray layerInfo;
    QDataStream li(&layerInfo, QIODevice::WriteOnly);
    li.setByteOrder(QDataStream::BigEndian);

    QList<const Layer *> layers;
    int skipped = 0;
    for (int i = 0; i < doc->layerCount(); ++i) {
        if (doc->layer(i).isAdjustment())
            ++skipped;
        else
            layers << &doc->layer(i);
    }
    if (skipped)
        *warning = QObject::tr("%n adjustment layer(s) cannot be stored in PSD by PairPaint and were left out. "
                               "Save as .pairpaint to keep them.", nullptr, skipped);

    // Encode channel data first so the records can state each channel's length.
    QList<QList<QPair<int, QByteArray>>> channelData;
    for (const Layer *l : layers) {
        const QList<QByteArray> planes = planesOf(l->image);
        QList<QPair<int, QByteArray>> chans = {{-1, planes[3]}, {0, planes[0]}, {1, planes[1]}, {2, planes[2]}};
        if (!l->mask.isNull())
            chans.append({-2, maskPlane(l->mask)});
        for (auto &c : chans)
            c.second = encodePlane(c.second, w, h);
        channelData << chans;
    }

    li << qint16(layers.size());
    for (int i = 0; i < layers.size(); ++i) {
        const Layer *l = layers[i];
        li << qint32(0) << qint32(0) << qint32(h) << qint32(w);
        li << quint16(channelData[i].size());
        for (const auto &[id, data] : channelData[i])
            li << qint16(id) << quint32(2 + data.size());
        li.writeRawData("8BIM", 4);
        li.writeRawData(keyForMode(l->mode).constData(), 4);
        li << quint8(qRound(l->opacity * 255)) << quint8(0) << quint8(l->visible ? 0 : 2) << quint8(0);

        QByteArray extra;
        QDataStream ex(&extra, QIODevice::WriteOnly);
        ex.setByteOrder(QDataStream::BigEndian);
        if (!l->mask.isNull()) {
            ex << quint32(20) << qint32(0) << qint32(0) << qint32(h) << qint32(w)
               << quint8(255) << quint8(l->maskEnabled ? 0 : 2) << quint16(0);
        } else {
            ex << quint32(0);
        }
        ex << quint32(0);  // blending ranges
        QByteArray name = l->name.toLocal8Bit().left(255);
        ex << quint8(name.size());
        ex.writeRawData(name.constData(), int(name.size()));
        for (int pad = (4 - (1 + name.size()) % 4) % 4; pad > 0; --pad)
            ex << quint8(0);
        // Unicode name
        ex.writeRawData("8BIM", 4);
        ex.writeRawData("luni", 4);
        quint32 len = 4 + 2 * quint32(l->name.size());
        const quint32 padded = (len + 3) & ~3u;
        ex << padded << quint32(l->name.size());
        for (QChar c : l->name)
            ex << quint16(c.unicode());
        for (quint32 k = len; k < padded; ++k)
            ex << quint8(0);
        li << quint32(extra.size());
        li.writeRawData(extra.constData(), int(extra.size()));
    }
    for (const auto &chans : channelData)
        for (const auto &[id, data] : chans) {
            li << quint16(1);  // RLE
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
