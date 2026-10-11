#include "sync_queue.h"
#include "devices.h"
#include "saves.h"
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qsavefile.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const char *reason) { throw std::runtime_error(reason); }
bool uuid(const QString &value) { return !QUuid(value).isNull() && !value.contains('/'); }
bool fingerprintValid(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
QString path(const Library &lib) { return lib.dataRoot() + "/sync/queue.json"; }
QList<PendingPeerSync> read(const Library &lib) {
    const QFileInfo info(path(lib));
    if (!info.exists() && !info.isSymbolicLink()) return {};
    if (!info.isFile() || info.isSymbolicLink() || info.size() > 512 * 1024)
        fail("Pending peer sync record is damaged.");
    QFile file(path(lib));
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read pending peer sync record.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Pending peer sync record is damaged.");
    const auto object = document.object();
    if (object["schema"] != 1 || object["profile_id"] != lib.profileId() || !object["tasks"].isArray())
        fail("Pending peer sync record belongs to another profile or version.");
    const auto array = object["tasks"].toArray();
    if (array.size() > 10000) fail("Too many pending peer sync tasks.");
    QList<PendingPeerSync> tasks;
    for (const auto &item : array) {
        if (!item.isObject()) fail("Pending peer sync task is damaged.");
        const auto entry = item.toObject();
        PendingPeerSync task{entry["fingerprint"].toString(), entry["game_id"].toString(),
                             entry["snapshot_id"].toString(), entry["queued_at"].toString()};
        if (!fingerprintValid(task.fingerprint) || !uuid(task.gameId) || !uuid(task.snapshotId) ||
            !QDateTime::fromString(task.queuedAt, Qt::ISODateWithMs).isValid())
            fail("Pending peer sync task is damaged.");
        for (const auto &known : tasks)
            if (known.fingerprint == task.fingerprint && known.gameId == task.gameId)
                fail("Pending peer sync task is duplicated.");
        tasks.append(task);
    }
    return tasks;
}
void write(const Library &lib, const QList<PendingPeerSync> &tasks) {
    const auto directory = lib.dataRoot() + "/sync";
    if (QFileInfo(directory).isSymbolicLink() || !QDir().mkpath(directory))
        fail("Cannot create peer sync storage.");
    QJsonArray array;
    for (const auto &task : tasks)
        array.append(QJsonObject{{"fingerprint", task.fingerprint}, {"game_id", task.gameId},
                                 {"snapshot_id", task.snapshotId}, {"queued_at", task.queuedAt}});
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"profile_id", lib.profileId()},
                                                  {"tasks", array}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > 512 * 1024) fail("Pending peer sync record is full.");
    QSaveFile file(path(lib));
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit())
        fail("Cannot save pending peer sync record.");
}
}

QList<PendingPeerSync> pendingPeerSync(const Library &lib) { return read(lib); }

void queuePeerSync(const Library &lib, const QString &gameId, const QString &snapshotId) {
    if (!uuid(gameId) || !uuid(snapshotId)) fail("Invalid pending peer sync identity.");
    verifiedSaveSnapshotBundle(lib, gameId, snapshotId);
    const auto peers = trustedPeers(lib);
    if (peers.isEmpty()) return;
    QLockFile lock(path(lib) + ".lock"); lock.setStaleLockTime(0);
    if (!QDir().mkpath(QFileInfo(path(lib)).absolutePath()) || !lock.tryLock(5000))
        fail("Pending peer sync record is busy.");
    auto tasks = read(lib);
    for (const auto &peer : peers) {
        bool found = false;
        for (auto &task : tasks) if (task.fingerprint == peer.fingerprint && task.gameId == gameId) {
            task.snapshotId = snapshotId;
            task.queuedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            found = true; break;
        }
        if (!found) tasks.append({peer.fingerprint, gameId, snapshotId,
                                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)});
    }
    if (tasks.size() > 10000) fail("Too many pending peer sync tasks.");
    write(lib, tasks);
}

void acknowledgePeerSync(const Library &lib, const QString &fingerprint,
                         const QString &gameId, const QString &snapshotId) {
    if (!fingerprintValid(fingerprint) || !uuid(gameId) || !uuid(snapshotId))
        fail("Invalid peer sync acknowledgement.");
    QLockFile lock(path(lib) + ".lock"); lock.setStaleLockTime(0);
    if (!QDir().mkpath(QFileInfo(path(lib)).absolutePath()) || !lock.tryLock(5000))
        fail("Pending peer sync record is busy.");
    auto tasks = read(lib);
    for (int i = 0; i < tasks.size(); ++i)
        if (tasks[i].fingerprint == fingerprint && tasks[i].gameId == gameId &&
            tasks[i].snapshotId == snapshotId) {
            tasks.removeAt(i); write(lib, tasks); return;
        }
}

} // namespace novelty
