// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Heif.h"

#include "FileIO.h"

#include <QByteArrayList>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QScopeGuard>
#include <QTransform>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <memory>

#if defined(PAIRPAINT_HAVE_LIBHEIF)
#include <libheif/heif.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX  // keep std::min and std::max usable
#endif
#include <windows.h>
#include <propidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#endif

namespace Heif {

namespace {

[[maybe_unused]] bool tooLarge(qint64 width, qint64 height, QString *error)
{
    if (width > 0 && height > 0 && width * height <= FileIO::maxImagePixels())
        return false;
    *error = FileIO::tooLargeMessage(width, height);
    return true;
}

#if defined(PAIRPAINT_HAVE_LIBHEIF)

// A color space from HEIF's "nclx" codes (ITU-T H.273); invalid if unknown or HDR.
QColorSpace fromNclx(int primaries, int transfer)
{
    QColorSpace::Primaries p;
    switch (primaries) {
    case 1: p = QColorSpace::Primaries::SRgb; break;      // BT.709, same as sRGB
    case 12: p = QColorSpace::Primaries::DciP3D65; break;  // Display P3 (iPhone)
    default: return {};
    }
    switch (transfer) {
    case 1: case 6: case 13: case 14: case 15:  // sRGB, and the video curves displayed like it
        return QColorSpace(p, QColorSpace::TransferFunction::SRgb);
    case 8:
        return QColorSpace(p, QColorSpace::TransferFunction::Linear);
    case 4:
        return QColorSpace(p, QColorSpace::TransferFunction::Gamma, 2.2f);
    default:
        return {};  // unspecified, or HDR (PQ, HLG), which would need tone mapping
    }
}

QImage readImage(const QString &path, QString *error)
{
#if LIBHEIF_HAVE_VERSION(1, 13, 0)
    static const bool initialized = [] { heif_init(nullptr); return true; }();  // loads decoder plugins
    Q_UNUSED(initialized)
#endif
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    const QByteArray data = f.readAll();
    auto fail = [&](const heif_error &e) {
        *error = QObject::tr("Could not read the HEIF image: %1").arg(QString::fromUtf8(e.message));
        return QImage();
    };
    std::unique_ptr<heif_context, decltype(&heif_context_free)> ctx(heif_context_alloc(), heif_context_free);
    heif_context_set_maximum_image_size_limit(ctx.get(), 300000);
    heif_error err = heif_context_read_from_memory_without_copy(ctx.get(), data.constData(), size_t(data.size()), nullptr);
    if (err.code)
        return fail(err);
    heif_image_handle *handlePtr = nullptr;
    err = heif_context_get_primary_image_handle(ctx.get(), &handlePtr);
    if (err.code)
        return fail(err);
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(handlePtr, heif_image_handle_release);
    if (tooLarge(heif_image_handle_get_width(handle.get()), heif_image_handle_get_height(handle.get()), error))
        return {};

    // Decoded with its rotation and mirroring applied, as 8-bit RGBA.
    heif_image *imagePtr = nullptr;
    err = heif_decode_image(handle.get(), &imagePtr, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (err.code)
        return fail(err);
    std::unique_ptr<heif_image, decltype(&heif_image_release)> image(imagePtr, heif_image_release);
    const int w = heif_image_get_width(image.get(), heif_channel_interleaved);
    const int h = heif_image_get_height(image.get(), heif_channel_interleaved);
#if LIBHEIF_HAVE_VERSION(1, 20, 0)
    size_t stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly2(image.get(), heif_channel_interleaved, &stride);
#else
    int intStride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &intStride);
    const size_t stride = size_t(std::max(intStride, 0));
#endif
    if (!plane || tooLarge(w, h, error) || stride < size_t(w) * 4
        || heif_image_get_bits_per_pixel_range(image.get(), heif_channel_interleaved) != 8) {
        if (error->isEmpty())
            *error = QObject::tr("Could not read the HEIF image: unsupported pixel layout.");
        return {};
    }
    QImage out(w, h, QImage::Format_RGBA8888);
    if (out.isNull()) {
        *error = QObject::tr("Not enough memory to open this file.");
        return {};
    }
    for (int y = 0; y < h; ++y)
        std::memcpy(out.scanLine(y), plane + size_t(y) * stride, size_t(w) * 4);

    QColorSpace cs;
    switch (heif_image_handle_get_color_profile_type(handle.get())) {
    case heif_color_profile_type_prof:
    case heif_color_profile_type_rICC: {
        const size_t size = heif_image_handle_get_raw_color_profile_size(handle.get());
        if (size > 0 && size < 4 * 1024 * 1024) {
            QByteArray icc(qsizetype(size), Qt::Uninitialized);
            if (!heif_image_handle_get_raw_color_profile(handle.get(), icc.data()).code)
                cs = QColorSpace::fromIccProfile(icc);
        }
        break;
    }
    case heif_color_profile_type_nclx: {
        heif_color_profile_nclx *nclx = nullptr;
        if (!heif_image_handle_get_nclx_color_profile(handle.get(), &nclx).code && nclx) {
            cs = fromNclx(nclx->color_primaries, nclx->transfer_characteristics);
            heif_nclx_color_profile_free(nclx);
        }
        break;
    }
    default:
        break;
    }
    if (cs.isValid())
        out.setColorSpace(cs);
    return out;
}

#elif defined(Q_OS_WIN)

using Microsoft::WRL::ComPtr;

QString codecMessage()
{
    return QObject::tr("Windows could not decode this HEIC/HEIF image. Install \"HEIF Image Extensions\" and "
                       "\"HEVC Video Extensions\" from the Microsoft Store, then open it again.");
}

// Turns an image upright for an EXIF orientation (1..8), the way Qt does for JPEG photos.
QImage applyOrientation(const QImage &img, int orientation)
{
    static const struct { bool mirror, flip, rotate90; } steps[9] = {
        {}, {}, {true, false, false}, {true, true, false}, {false, true, false},
        {false, true, true}, {false, false, true}, {true, false, true}, {true, true, true}};
    if (orientation < 2 || orientation > 8)
        return img;
    const auto &s = steps[orientation];
    QImage out = img;
    if (s.mirror || s.flip)
        out = out.transformed(QTransform::fromScale(s.mirror ? -1 : 1, s.flip ? -1 : 1));
    if (s.rotate90)
        out = out.transformed(QTransform().rotate(90));
    return out;
}

QImage readImage(const QString &path, QString *error)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto uninit = qScopeGuard([init] {  // declared first: runs after the COM objects are released
        if (SUCCEEDED(init))
            CoUninitialize();
    });
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        *error = codecMessage();
        return {};
    }
    // Fails when no HEIF codec is installed.
    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder))
        || FAILED(decoder->GetFrame(0, &frame))) {
        *error = codecMessage();
        return {};
    }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || tooLarge(w, h, error)) {
        if (error->isEmpty())
            *error = codecMessage();
        return {};
    }
    ComPtr<IWICFormatConverter> converter;
    QImage img(int(w), int(h), QImage::Format_ARGB32);  // BGRA in memory, straight alpha
    if (img.isNull()) {
        *error = QObject::tr("Not enough memory to open this file.");
        return {};
    }
    // Fails here when the HEIF codec is installed but the HEVC one isn't.
    if (FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom))
        || FAILED(converter->CopyPixels(nullptr, UINT(img.bytesPerLine()), UINT(img.sizeInBytes()), img.bits()))) {
        *error = codecMessage();
        return {};
    }

    // WIC returns the pixels as stored; the rotation is in the metadata.
    int orientation = 1;
    ComPtr<IWICMetadataQueryReader> query;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&query))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(query->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2)
            orientation = value.uiVal;
        PropVariantClear(&value);
    }
    // The embedded color profile, if any.
    UINT count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0 && count <= 16) {
        std::vector<ComPtr<IWICColorContext>> contexts(count);
        std::vector<IWICColorContext *> raw;
        for (auto &c : contexts) {
            if (FAILED(factory->CreateColorContext(&c)))
                break;
            raw.push_back(c.Get());
        }
        UINT actual = 0;
        if (raw.size() == count && SUCCEEDED(frame->GetColorContexts(count, raw.data(), &actual))) {
            for (UINT k = 0; k < actual; ++k) {
                WICColorContextType type;
                UINT size = 0;
                if (SUCCEEDED(raw[k]->GetType(&type)) && type == WICColorContextProfile
                    && SUCCEEDED(raw[k]->GetProfileBytes(0, nullptr, &size)) && size > 0 && size < 4 * 1024 * 1024) {
                    QByteArray icc(qsizetype(size), Qt::Uninitialized);
                    if (SUCCEEDED(raw[k]->GetProfileBytes(size, reinterpret_cast<BYTE *>(icc.data()), &size))) {
                        img.setColorSpace(QColorSpace::fromIccProfile(icc));
                        break;
                    }
                }
            }
        }
    }
    return applyOrientation(img, orientation);
}

#endif

} // namespace

bool hasDecoder()
{
#if defined(PAIRPAINT_HAVE_LIBHEIF) || defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

bool isHeif(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    // The "ftyp" box lists the file's brands: HEVC-coded HEIF (what iPhones write) is ours to
    // decode; AVIF and other HEIF flavors are left to Qt's plugins.
    const QByteArray head = f.read(256);
    if (head.size() < 16 || head.mid(4, 4) != "ftyp")
        return false;
    const int boxSize = int(qFromBigEndian<quint32>(head.constData()));
    const QByteArray major = head.mid(8, 4);
    if (major == "avif" || major == "avis")
        return false;
    static const QByteArrayList hevc = {"heic", "heix", "heim", "heis", "hevc", "hevx"};
    if (hevc.contains(major))
        return true;
    for (int at = 16; at + 4 <= std::min(boxSize, int(head.size())); at += 4)  // compatible brands
        if (hevc.contains(head.mid(at, 4)))
            return true;
    return false;
}

QImage read(const QString &path, QString *error)
{
#if defined(PAIRPAINT_HAVE_LIBHEIF) || defined(Q_OS_WIN)
    return readImage(path, error);
#else
    *error = QObject::tr("This version of PairPaint can't open HEIC/HEIF images.");
    return {};
#endif
}

} // namespace Heif
