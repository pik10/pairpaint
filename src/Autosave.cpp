// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#include "Autosave.h"

#include "Document.h"
#include "FileIO.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUndoStack>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

// One document to write: a snapshot (cheap, the images are shared until changed).
struct Autosave::Job {
    int id;
    DocState state;
    QString originalPath, name;
};

namespace {

QString copyFile(const QString &dir, int id) { return dir + QStringLiteral("/%1.pairpaint").arg(id); }
QString infoFile(const QString &dir, int id) { return dir + QStringLiteral("/%1.json").arg(id); }

} // namespace

Autosave::Autosave(QObject *parent)
    : Autosave(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/recovery"),
               std::max(0, QSettings().value(QStringLiteral("autosave/minutes"), 2).toInt()) * 60 * 1000, parent)
{
}

Autosave::Autosave(const QString &root, int intervalMs, QObject *parent) : QObject(parent), m_root(root)
{
    static int sessions = 0;  // tests run several in one process
    m_dir = QStringLiteral("%1/%2-%3-%4")
                .arg(root, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
                .arg(QCoreApplication::applicationPid())
                .arg(++sessions);
    QDir().mkpath(m_dir);
    m_lock = std::make_unique<QLockFile>(m_dir + QStringLiteral("/lock"));
    m_lock->setStaleLockTime(0);  // only a lock whose process has ended is stale, however old
    m_lock->tryLock(0);
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &Autosave::finished);
    connect(&m_timer, &QTimer::timeout, this, &Autosave::saveNow);
    m_retry.setSingleShot(true);
    m_retry.setInterval(5000);
    connect(&m_retry, &QTimer::timeout, this, &Autosave::saveNow);
    if (intervalMs > 0)
        m_timer.start(intervalMs);
}

Autosave::~Autosave()
{
    m_timer.stop();
    m_retry.stop();
    m_watcher.waitForFinished();
    if (!m_dir.isEmpty()) {
        m_lock->unlock();
        QDir(m_dir).removeRecursively();
    }
}

void Autosave::abandon()
{
    m_watcher.waitForFinished();
    m_lock->unlock();
    m_dir.clear();
}

void Autosave::track(Document *doc)
{
    const int id = m_nextId++;
    m_entries.insert(id, {doc, id, doc->isModified(), false});
    QUndoStack *stack = doc->undoStack();
    connect(stack, &QUndoStack::indexChanged, this, [this, id] {
        if (auto it = m_entries.find(id); it != m_entries.end())
            it->dirty = true;
    });
    connect(stack, &QUndoStack::cleanChanged, this, [this, id](bool clean) {
        if (!clean)
            return;
        if (auto it = m_entries.find(id); it != m_entries.end())
            it->dirty = false;
        removeCopy(id);  // saved, or undone back to the saved state
    });
    connect(doc, &QObject::destroyed, this, [this, id] {  // closed
        removeCopy(id);
        m_entries.remove(id);
    });
}

bool Autosave::hasCopy(const Document *doc) const
{
    for (const Entry &e : m_entries)
        if (e.doc == doc)
            return e.hasCopy;
    return false;
}

void Autosave::saveNow()
{
    if (m_dir.isEmpty() || m_watcher.isRunning())
        return;  // the next tick saves what changed meanwhile
    if (m_busy && m_busy()) {
        m_retry.start();  // e.g. mid-stroke: soon, not now
        return;
    }
    QList<Job> jobs;
    for (Entry &e : m_entries) {
        if (!e.doc || !e.dirty || !e.doc->isModified())
            continue;
        jobs.append({e.id, e.doc->state(), e.doc->filePath(), e.doc->displayName()});
        e.dirty = false;
        e.hasCopy = true;
        m_saving.append(e.id);
    }
    if (jobs.isEmpty())
        return;
    m_watcher.setFuture(QtConcurrent::run([dir = m_dir, jobs] {
        for (const Job &job : jobs) {
            QString error;
            if (!FileIO::saveProjectState(job.state, copyFile(dir, job.id), &error))
                continue;
            QSaveFile info(infoFile(dir, job.id));
            if (info.open(QIODevice::WriteOnly)) {
                const QJsonObject o{{QStringLiteral("original"), job.originalPath},
                                    {QStringLiteral("name"), job.name},
                                    {QStringLiteral("saved"), QDateTime::currentDateTime().toString(Qt::ISODate)}};
                info.write(QJsonDocument(o).toJson());
                info.commit();
            }
        }
    }));
}

void Autosave::waitForSaves()
{
    m_watcher.waitForFinished();
    finished();
}

void Autosave::finished()
{
    // Documents saved or closed while their copy was being written: remove the late copy.
    const QList<int> saved = std::exchange(m_saving, {});
    for (int id : saved) {
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend() || !it->doc || !it->doc->isModified())
            removeCopy(id);
    }
}

void Autosave::removeCopy(int id)
{
    if (auto it = m_entries.find(id); it != m_entries.end())
        it->hasCopy = false;
    if (m_dir.isEmpty() || m_saving.contains(id))
        return;  // still being written: finished() removes it
    QFile::remove(copyFile(m_dir, id));
    QFile::remove(infoFile(m_dir, id));
}

QStringList Autosave::orphanDirs() const
{
    QStringList dirs;
    const QFileInfoList entries = QDir(m_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &fi : entries) {
        const QString dir = fi.absoluteFilePath();
        if (dir == QFileInfo(m_dir).absoluteFilePath())
            continue;
        QLockFile lock(dir + QStringLiteral("/lock"));
        lock.setStaleLockTime(0);
        if (lock.tryLock(0)) {  // nobody holds it: that session is gone
            lock.unlock();
            dirs << dir;
        }
    }
    return dirs;
}

QList<Autosave::Recovered> Autosave::findOrphans() const
{
    QList<Recovered> found;
    for (const QString &dir : orphanDirs()) {
        const QFileInfoList infos = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
        for (const QFileInfo &fi : infos) {
            QFile f(fi.absoluteFilePath());
            const QString copy = dir + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pairpaint");
            if (!QFile::exists(copy) || !f.open(QIODevice::ReadOnly))
                continue;
            const QJsonObject o = QJsonDocument::fromJson(f.read(64 * 1024)).object();
            found.append({copy, o.value(QStringLiteral("original")).toString(), o.value(QStringLiteral("name")).toString(),
                          QDateTime::fromString(o.value(QStringLiteral("saved")).toString(), Qt::ISODate)});
        }
    }
    std::sort(found.begin(), found.end(), [](const Recovered &a, const Recovered &b) { return a.saved < b.saved; });
    return found;
}

void Autosave::discardOrphans()
{
    for (const QString &dir : orphanDirs())
        QDir(dir).removeRecursively();
}
