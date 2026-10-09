// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

class Document;

// Keeps recovery copies of documents with unsaved changes, so a crash or power cut loses at
// most a few minutes of work. Copies are written as projects, in the background, to a folder
// of this session that is locked while PairPaint runs. A folder whose lock is free belongs to
// a session that ended without cleaning up (a crash): its copies can be recovered.
class Autosave : public QObject {
    Q_OBJECT
public:
    struct Recovered {
        QString copyPath;      // the recovery copy (a .pairpaint project)
        QString originalPath;  // the file the document was opened from or saved to; may be empty
        QString name;          // the name shown when it was open, e.g. "photo.jpg" or "Untitled-2"
        QDateTime saved;
    };

    // `root` holds one folder per session (default: the app data folder). `intervalMs` 0 never
    // saves on its own (tests call saveNow()).
    explicit Autosave(QObject *parent = nullptr);
    Autosave(const QString &root, int intervalMs, QObject *parent = nullptr);
    ~Autosave() override;  // a normal exit: this session's copies are removed

    // Watches a document: changed documents get a copy; saving it (or undoing back to the
    // saved state) or closing it removes the copy.
    void track(Document *doc);
    // While this returns true (e.g. during a brush stroke), saving waits a few seconds.
    void setBusyCheck(std::function<bool()> busy) { m_busy = std::move(busy); }

    void saveNow();
    void waitForSaves();
    // For tests: leaves this session's copies behind and unlocked, as a crash would.
    void abandon();
    QString sessionDir() const { return m_dir; }
    bool hasCopy(const Document *doc) const;

    // Copies left behind by sessions that crashed, oldest first.
    QList<Recovered> findOrphans() const;
    // Removes the folders of crashed sessions (after their copies were recovered or discarded).
    void discardOrphans();
    // Removes these recovered copies only, and folders left without copies; other copies stay.
    void discardRecovered(const QList<Recovered> &copies);

private:
    struct Entry {
        QPointer<Document> doc;
        int id = 0;
        bool dirty = false;   // changed since the last copy
        bool hasCopy = false;
    };
    struct Job;
    void removeCopy(int id);
    void finished();
    QStringList orphanDirs() const;

    QString m_root, m_dir;
    std::unique_ptr<QLockFile> m_lock;
    QTimer m_timer;
    QTimer m_retry;  // while busy
    QHash<int, Entry> m_entries;
    int m_nextId = 1;
    std::function<bool()> m_busy;
    QFutureWatcher<QList<int>> m_watcher;  // the job's result: ids whose copy couldn't be written
    QList<int> m_saving;  // ids being written by the running job
};
