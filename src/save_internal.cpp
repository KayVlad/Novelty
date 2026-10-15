#include "filesystem_durability.h"
#include "save_internal.h"
#include "filesystem_checks.h"

namespace novelty::save_detail {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool exists(const QString &path) { return checkedExists(path); }
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validToken(const QString &value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value; }
bool validFingerprint(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool profileGameMatches(const SaveConfig &config, const QString &profileGameId,
                        const QString &authenticatedPeer) {
    if (profileGameId == config.profileGameId) return true;
    if (authenticatedPeer.isEmpty() && !config.serverGameKey.isEmpty()) return true;
    for (const auto &link : config.peerLinks)
        if (link.profileGameId == profileGameId &&
            (authenticatedPeer.isEmpty() || link.fingerprint == authenticatedPeer)) return true;
    return false;
}
bool labelCompatible(const QString &expected, const QString &actual) {
    return expected.isEmpty() || actual.isEmpty() || expected == actual;
}
bool labelsCompatible(const SaveConfig &config, const QJsonObject &manifest) {
    return labelCompatible(config.gameIdentity, manifest["game_identity"].toString()) &&
           labelCompatible(config.releaseIdentity, manifest["release_identity"].toString()) &&
           labelCompatible(config.saveCompatibility, manifest["save_compatibility"].toString());
}
bool mappingsMatch(const SaveConfig &config, const QJsonObject &manifest) {
    const auto items = manifest["items"].toArray();
    if (items.size() != config.mappings.size()) return false;
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["scope"].toString() != config.mappings[i].scope ||
            items[i].toObject()["path"].toString() != config.mappings[i].path) return false;
    return true;
}
QString configPath(const Library &lib, const QString &id) { return lib.gamePath(id, "saves.json"); }
QString journalPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-restore.json"); }
QString pendingPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-pending.json"); }
QString headPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-head.json"); }
QString observationPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-observation.json"); }
QString transferIntentPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-transfer-intent.json"); }
QString transferUndoPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-transfer-undo.json"); }
QString snapshotsRoot(const Library &lib, const QString &id) {
    return QFileInfo(lib.gamePath(id, "saves.json")).absolutePath() + "/snapshots";
}
QString currentHead(const Library &lib, const QString &id) {
    const auto record = readJson(headPath(lib, id));
    if (record.isEmpty()) return {};
    if (record["schema"].toInt() != 1 || !validToken(record["snapshot_id"].toString()))
        fail("Current save snapshot pointer is invalid. Inspect it before syncing.");
    return record["snapshot_id"].toString();
}
void setHead(const Library &lib, const QString &id, const QString &snapshotId) {
    if (!snapshotId.isEmpty() && !validToken(snapshotId)) fail("Invalid current save snapshot ID.");
    if (snapshotId.isEmpty()) {
        if (exists(headPath(lib, id))) removeDurableFile(headPath(lib, id));
        return;
    }
    writeDurableJson(headPath(lib, id), {{"schema", 1}, {"snapshot_id", snapshotId}});
}
SaveLocks::SaveLocks(const Library &lib, const QString &id, int mutationWaitMs)
    : mutation(lib.dataRoot() + "/mutation.lock"), launch(lib.gamePath(id, "launch.lock")) {
    mutation.setStaleLockTime(0); launch.setStaleLockTime(0);
    if (!mutation.tryLock(mutationWaitMs)) fail("Library maintenance is in progress. Retry shortly.");
    if (!QDir().mkpath(QFileInfo(launch.fileName()).absolutePath()) || !launch.tryLock())
        fail("This game is busy. Wait for its current operation before changing saves.");
    if (!runtimeRecoveryIssue(lib, id).isEmpty()) fail("Resolve the game's interrupted launch before changing saves.");
}
} // namespace novelty::save_detail
