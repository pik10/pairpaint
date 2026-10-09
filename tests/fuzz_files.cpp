// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors
//
// Mutation fuzzer for PairPaint's file readers (PSD and .pairpaint). It corrupts seed files
// in random ways, opens them, and renders and re-saves whatever loads. Build it with
// -DPAIRPAINT_SANITIZE=ON so memory errors stop it immediately.
//
//   fuzz_files <iterations> <random seed> <seed files...>
//
// Before every attempt the current input is written to fuzz-current.<ext>, so after a crash
// that file reproduces it. Inputs that take longer than 20 seconds are saved as fuzz-hang.<ext>.

#include "Document.h"
#include "FileIO.h"
#include "Psd.h"

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

QByteArray mutate(QByteArray data, QRandomGenerator &rng)
{
    static const quint32 interesting32[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                            0x10000, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu};
    const int count = 1 + int(rng.bounded(4));
    for (int m = 0; m < count && !data.isEmpty(); ++m) {
        const int pos = int(rng.bounded(quint32(data.size())));
        switch (rng.bounded(8)) {
        case 0:  // flip a bit
            data[pos] = char(data[pos] ^ (1 << rng.bounded(8)));
            break;
        case 1:  // random byte
            data[pos] = char(rng.bounded(256));
            break;
        case 2: {  // an "interesting" 32-bit value, e.g. a huge length or size
            const quint32 v = interesting32[rng.bounded(quint32(std::size(interesting32)))];
            for (int k = 0; k < 4 && pos + k < data.size(); ++k)
                data[pos + k] = char(v >> (24 - 8 * k));
            break;
        }
        case 3: {  // nudge a 16-bit value up or down
            if (pos + 1 < data.size()) {
                quint16 v = quint16((uchar(data[pos]) << 8) | uchar(data[pos + 1]));
                v = quint16(v + int(rng.bounded(65)) - 32);
                data[pos] = char(v >> 8);
                data[pos + 1] = char(v);
            }
            break;
        }
        case 4:  // truncate
            data.truncate(pos);
            break;
        case 5:  // delete a chunk
            data.remove(pos, 1 + int(rng.bounded(64)));
            break;
        case 6: {  // duplicate a chunk
            const QByteArray chunk = data.mid(pos, 1 + int(rng.bounded(64)));
            data.insert(int(rng.bounded(quint32(data.size()))), chunk);
            break;
        }
        default: {  // insert random bytes
            QByteArray junk(1 + int(rng.bounded(16)), Qt::Uninitialized);
            for (char &c : junk)
                c = char(rng.bounded(256));
            data.insert(pos, junk);
            break;
        }
        }
    }
    return data;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    FileIO::setMaxImagePixels(4'000'000);  // keep corrupted "huge" images quick to reject or load
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <iterations> <random seed> <seed files...>\n", argv[0]);
        return 2;
    }
    const long iterations = std::atol(argv[1]);
    QRandomGenerator rng(quint32(std::atol(argv[2])));
    QList<QPair<QByteArray, QString>> seeds;  // contents, extension
    for (int i = 3; i < argc; ++i) {
        QFile f(QString::fromLocal8Bit(argv[i]));
        if (f.open(QIODevice::ReadOnly))
            seeds.append({f.readAll(), QFileInfo(f).suffix()});
    }
    if (seeds.isEmpty())
        return 2;
    QTemporaryDir work;

    // Watchdog: an input that makes PairPaint hang is a bug too.
    std::atomic<long> started{0};
    std::atomic<int> current{-1};
    std::thread([&] {
        long last = -1;
        int stuck = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const long now = started.load();
            stuck = (now == last) ? stuck + 1 : 0;
            last = now;
            if (stuck >= 20 && current.load() >= 0) {
                const QString ext = seeds[current.load()].second;
                QFile::copy(QStringLiteral("fuzz-current.") + ext, QStringLiteral("fuzz-hang.") + ext);
                std::fprintf(stderr, "HANG: input saved as fuzz-hang.%s\n", qPrintable(ext));
                std::_Exit(3);
            }
        }
    }).detach();

    long loaded = 0;
    for (long i = 0; i < iterations; ++i) {
        const int s = int(rng.bounded(quint32(seeds.size())));
        const QByteArray input = mutate(seeds[s].first, rng);
        const QString ext = seeds[s].second;
        {
            QFile cur(QStringLiteral("fuzz-current.") + ext);  // kept if we crash
            if (cur.open(QIODevice::WriteOnly))
                cur.write(input);
        }
        const QString path = work.filePath(QStringLiteral("input.") + ext);
        {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return 2;
            f.write(input);
        }
        current = s;
        started = i;
        QString err, warning;
        Document *doc = FileIO::load(path, &err, &warning);
        if (ext == QLatin1String("psd"))
            Psd::readComposite(path, &err);
        if (doc) {
            ++loaded;
            // Render, and save again in both layered formats.
            doc->flattened();
            Psd::write(doc, work.filePath(QStringLiteral("out.psd")), &err, &warning);
            FileIO::saveProject(doc, work.filePath(QStringLiteral("out.pairpaint")), &err);
            delete doc;
        }
        if ((i + 1) % 500 == 0)
            std::fprintf(stderr, "%ld inputs, %ld opened\n", i + 1, loaded);
    }
    std::fprintf(stderr, "done: %ld inputs, %ld opened, no crashes\n", iterations, loaded);
    QFile::remove(QStringLiteral("fuzz-current.psd"));
    QFile::remove(QStringLiteral("fuzz-current.pairpaint"));
    return 0;
}
