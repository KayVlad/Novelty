#include "filesystem_checks.h"
#include "filesystem_durability.h"
#include "save_server_store.h"
#include "core.h"
#include "saves.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlockfile.h>
#include <QtCore/quuid.h>
#include <climits>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool exists(const QString &path) { return checkedExists(path); }
void noLinks(const QString &path) {
    auto current = QString("/");
    for (const auto &part : QDir::cleanPath(QFileInfo(path).absoluteFilePath()).split('/', Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymbolicLink()) fail("Server storage cannot pass through symbolic links: " + current);
    }
}
bool validKey(const QString &key) {
    if (key.isEmpty() || key.toUtf8().size() > 256) return false;
    for (const auto c : key) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
QString gameRoot(const QString &root, const QString &key) {
    if (!validKey(key)) fail("Server game key is invalid.");
    return root + "/games/" + QString::fromLatin1(QCryptographicHash::hash(
        key.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString revisionFolder(const QString &game, int revision, const QString &id) {
    return game + "/revisions/" + QString::number(revision) + '-' + id;
}
QString freshId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void copyTree(const QString &source, const QString &destination, qint64 &bytes, int &entries) {
    noLinks(source); noLinks(destination);
    const QFileInfo info(source);
    if (info.isSymbolicLink() || ++entries > 20000) fail("Save bundle has an unsafe link or too many entries.");
    if (info.isDir()) {
        if (!QDir().mkpath(destination)) fail("Cannot stage server save directory.");
        for (const auto &child : checkedDirectoryEntries(source))
            copyTree(child.absoluteFilePath(), destination + '/' + child.fileName(), bytes, entries);
    } else if (info.isFile()) {
        if (info.size() < 0 || info.size() > 512LL * 1024 * 1024 - bytes)
            fail("Save bundle exceeds the 512 MiB server transfer limit.");
        bytes += info.size();
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source, destination))
            fail("Cannot stage server save file.");
    } else fail("Save bundle contains an unsupported entry.");
}
ServerSaveRevision readCurrent(const QString &root, const QString &serverId, const QString &key,
                               bool verifyContents, bool allowPendingPayload = false) {
    const auto game = gameRoot(root, key);
    noLinks(game);
    const auto path = game + "/current.json";
    if (!exists(path)) {
        const QDir revisions(game + "/revisions");
        if (!allowPendingPayload && !revisions.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                         QDir::NoDotAndDotDot).isEmpty())
            fail("Server save pointer is missing while revision payloads exist. Reconcile the store before uploading.");
        return {serverId, 0, {}, {}, 0};
    }
    noLinks(path);
    const auto record = readJson(path);
    const auto revision = record["revision"].toInt();
    const auto id = record["snapshot_id"].toString();
    const auto nonce = record["storage_id"].toString();
    if (record["schema"].toInt() != 1 || record["game_key"].toString() != key ||
        record["server_id"].toString() != serverId || revision <= 0 ||
        QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id ||
        QUuid(nonce).isNull() || QUuid(nonce).toString(QUuid::WithoutBraces) != nonce)
        fail("Current server save pointer is invalid.");
    const auto bundle = revisionFolder(game, revision, nonce);
    noLinks(bundle);
    if (!QFileInfo(bundle).isDir()) fail("Current server save bundle is missing.");
    int files = record["files"].toInt(-1);
    if (files > 20000) fail("Current server save file count is invalid.");
    if (verifyContents || files < 0) {
        const auto manifest = verifyPortableSaveBundle(bundle);
        if (manifest["id"].toString() != id) fail("Current server save bundle does not match its pointer.");
        files = manifest["files"].toInt(-1);
    }
    return {serverId, revision, id, bundle, files};
}
QJsonObject pointerRecord(const QString &key, const QString &serverId, int revision,
                          const QString &snapshotId, const QString &storageId, int files) {
    return {{"schema", 1}, {"game_key", key}, {"server_id", serverId},
            {"revision", revision}, {"snapshot_id", snapshotId}, {"storage_id", storageId}, {"files", files}};
}
void recoverPending(const QString &root, const QString &serverId, const QString &key) {
    const auto game = gameRoot(root, key);
    const auto intent = game + "/.inProgress";
    if (!exists(intent)) return;
    noLinks(intent);
    if (QFileInfo(intent).size() < 1 || QFileInfo(intent).size() > 4096)
        fail("Server save publication intent is invalid.");
    const auto record = readJson(intent);
    const auto expected = record["expected_revision"].toInt(-1);
    const auto revision = record["revision"].toInt(-1);
    const auto snapshotId = record["snapshot_id"].toString();
    const auto storageId = record["storage_id"].toString();
    if (record["schema"].toInt() != 1 || record["game_key"].toString() != key ||
        record["server_id"].toString() != serverId || expected < 0 || expected >= INT_MAX ||
        revision != expected + 1 || QUuid(snapshotId).isNull() ||
        QUuid(storageId).isNull() || QUuid(storageId).toString(QUuid::WithoutBraces) != storageId)
        fail("Server save publication intent is invalid.");
    const auto stage = game + "/.stage-" + storageId;
    const auto final = revisionFolder(game, revision, storageId);
    noLinks(stage); noLinks(final);
    // A first-upload replay must not turn an unrelated orphan into hidden history.
    // For later uploads, only older revisions and this intent's target may exist.
    const QDir revisions(game + "/revisions");
    for (const auto &name : revisions.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                 QDir::NoDotAndDotDot)) {
        if (name == QFileInfo(final).fileName()) continue;
        bool numeric = false;
        const auto foundRevision = name.section('-', 0, 0).toInt(&numeric);
        if (!numeric || foundRevision <= 0 || foundRevision >= revision)
            fail("Server save publication intent conflicts with another revision payload.");
    }
    const auto current = readCurrent(root, serverId, key, true, true);
    if (current.revision == revision) {
        if (current.snapshotId != snapshotId || current.bundleDirectory != final || exists(stage))
            fail("Server save publication intent disagrees with the current revision.");
        persistTree(final); persistParent(final);
        writeDurableJson(game + "/current.json", readJson(game + "/current.json"));
        removeDurableFile(intent);
        return;
    }
    if (current.revision != expected || (exists(stage) && exists(final)))
        fail("Server save publication intent disagrees with the current revision.");
    const auto phase = record["phase"].toString();
    if (phase == "copying") {
        if (exists(final)) fail("An unverified server stage was published unexpectedly.");
        if (exists(stage) && !QDir(stage).removeRecursively()) fail("Cannot clean interrupted server copy.");
        persistParent(stage);
        removeDurableFile(intent);
        return;
    }
    if (!phase.isEmpty() && phase != "verified") fail("Unknown server publication phase.");
    const auto source = exists(final) ? final : stage;
    if (!exists(source)) {
        // Publication did not start. Keep the previously committed pointer.
        removeDurableFile(intent);
        return;
    }
    const auto manifest = verifyPortableSaveBundle(source);
    if (manifest["id"].toString() != snapshotId)
        fail("Server save publication payload does not match its intent.");
    persistTree(source); persistParent(source);
    if (source == stage && !QDir().rename(stage, final))
        fail("Cannot finish publishing verified server save payload.");
    persistParent(final);
    writeDurableJson(game + "/current.json", pointerRecord(key, serverId, revision, snapshotId, storageId, manifest["files"].toInt()));
    const auto published = readCurrent(root, serverId, key, true);
    if (published.revision != revision || published.snapshotId != snapshotId || published.bundleDirectory != final)
        fail("Cannot verify recovered server save publication.");
    removeDurableFile(intent);
}
}

ServerRevisionConflict::ServerRevisionConflict(int revision)
    : std::runtime_error("Server save revision changed; the upload was not published."),
      currentRevision(revision) {}

SaveServerStore::SaveServerStore(const QString &root) : root_(QDir::cleanPath(QFileInfo(root).absoluteFilePath())) {
    noLinks(root_);
    if (!QDir().mkpath(root_)) fail("Cannot create save server storage.");
    if (!QFile::setPermissions(root_, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner))
        fail("Cannot make save server storage private to its owner.");
    noLinks(root_ + "/store.lock");
    QLockFile lock(root_ + "/store.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Save server storage is busy.");
    const auto identity = root_ + "/server.json";
    if (!exists(identity)) {
        if (!QDir(root_ + "/games").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            fail("Server identity is missing from an existing save store.");
        serverId_ = freshId();
        writeDurableJson(identity, {{"schema", 1}, {"server_id", serverId_}});
    } else {
        noLinks(identity);
        const auto record = readJson(identity);
        serverId_ = record["server_id"].toString();
        if (record["schema"].toInt() != 1 || QUuid(serverId_).isNull())
            fail("Save server identity is invalid.");
    }
}

void SaveServerStore::recover() {
    QLockFile lock(root_ + "/store.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Save server storage is busy.");
    if (!exists(root_ + "/games")) return;
    for (const auto &entry : checkedDirectoryEntries(root_ + "/games", 0)) {
        noLinks(entry.absoluteFilePath());
        const auto game = entry.absoluteFilePath();
        const auto source = exists(game + "/.inProgress") ? game + "/.inProgress" : game + "/current.json";
        if (!exists(source)) continue; // ambiguous legacy stages are retained
        noLinks(source);
        const auto record = readJson(source);
        const auto key = record["game_key"].toString();
        if (gameRoot(root_, key) != game) fail("Server game directory does not match its identity.");
        recoverPending(root_, serverId_, key);
        if (exists(game + "/current.json")) {
            auto pointer = readJson(game + "/current.json");
            const auto verified = readCurrent(root_, serverId_, key, true);
            if (!pointer.contains("files")) {
                pointer["files"] = verified.files;
                writeDurableJson(game + "/current.json", pointer);
            }
        }
    }
}

ServerSaveRevision SaveServerStore::current(const QString &gameKey, bool verifyContents) const {
    if (!verifyContents) {
        // Immutable payloads and atomic current.json publication permit a concurrent
        // HEAD to see the complete old or new revision, including during a PUT.
        return readCurrent(root_, serverId_, gameKey, false, exists(gameRoot(root_, gameKey) + "/.inProgress"));
    }
    noLinks(root_ + "/store.lock");
    QLockFile lock(root_ + "/store.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Save server storage is busy.");
    recoverPending(root_, serverId_, gameKey);
    return readCurrent(root_, serverId_, gameKey, true);
}

ServerSaveRevision SaveServerStore::put(const QString &gameKey, int expectedRevision,
                                        const QString &bundleDirectory, bool reconcile) {
    if (expectedRevision < 0) fail("Expected server revision is invalid.");
    noLinks(root_ + "/store.lock");
    QLockFile lock(root_ + "/store.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Save server storage is busy.");
    recoverPending(root_, serverId_, gameKey);
    const auto previous = readCurrent(root_, serverId_, gameKey, true);
    if (previous.revision != expectedRevision) throw ServerRevisionConflict(previous.revision);
    if (previous.revision == INT_MAX) fail("Server revision limit reached.");
    const auto source = verifyPortableSaveBundle(bundleDirectory);
    if (previous.revision > 0 && !reconcile) {
        const auto old = verifyPortableSaveBundle(previous.bundleDirectory);
        if (source["items"].toArray().size() != old["items"].toArray().size() ||
            source["game_identity"] != old["game_identity"] ||
            source["release_identity"] != old["release_identity"] ||
            source["save_compatibility"] != old["save_compatibility"])
            fail("Server game identity or save mappings changed; explicitly reconcile before uploading.");
        const auto currentItems = source["items"].toArray(), oldItems = old["items"].toArray();
        for (int i = 0; i < currentItems.size(); ++i)
            if (currentItems[i].toObject()["scope"] != oldItems[i].toObject()["scope"] ||
                currentItems[i].toObject()["path"] != oldItems[i].toObject()["path"])
                fail("Server save mappings changed; explicitly reconcile before uploading.");
    }
    const auto game = gameRoot(root_, gameKey);
    requireAvailableSpace(game, static_cast<qint64>(source["bytes"].toDouble()));
    const auto nonce = freshId();
    const auto stage = game + "/.stage-" + nonce;
    const auto final = revisionFolder(game, previous.revision + 1, nonce);
    noLinks(stage); noLinks(final);
    if (exists(stage) || exists(final) || !QDir().mkpath(game + "/revisions"))
        fail("Cannot create server save stage.");
    const auto intent = game + "/.inProgress";
    noLinks(intent);
    if (exists(intent)) fail("A server save publication is already in progress.");
    QJsonObject publication{{"schema", 1}, {"game_key", gameKey}, {"server_id", serverId_},
        {"expected_revision", previous.revision}, {"revision", previous.revision + 1},
        {"snapshot_id", source["id"].toString()}, {"storage_id", nonce}, {"phase", "copying"}};
    writeDurableJson(intent, publication); // establishes ownership before any copy is staged
    try {
        qint64 bytes = 0; int entries = 0;
        copyTree(bundleDirectory, stage, bytes, entries);
        const auto staged = verifyPortableSaveBundle(stage);
        if (staged != source) fail("Save bundle changed while staging the server revision.");
        persistTree(stage); persistParent(stage);
        publication["phase"] = "verified";
        writeDurableJson(intent, publication);
        if (!QDir().rename(stage, final)) fail("Cannot publish verified server save payload.");
        persistParent(final); persistParent(stage);
        writeDurableJson(game + "/current.json", pointerRecord(gameKey, serverId_, previous.revision + 1,
                                                       source["id"].toString(), nonce, source["files"].toInt()));
        const auto published = readCurrent(root_, serverId_, gameKey, true);
        if (published.revision != previous.revision + 1 ||
            published.snapshotId != source["id"].toString() || published.bundleDirectory != final)
            fail("Cannot verify published server save revision.");
        removeDurableFile(intent);
    } catch (...) {
        if (!exists(game + "/.inProgress") && QFileInfo(stage).isDir()) QDir(stage).removeRecursively();
        throw;
    }
    return readCurrent(root_, serverId_, gameKey, true);
}

} // namespace novelty
