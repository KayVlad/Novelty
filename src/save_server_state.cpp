#include "save_server_state.h"
#include "saves.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool validFingerprint(const QJsonObject &value) {
    return value["items"].isArray() && value["files"].isDouble() && value["bytes"].isDouble() &&
           value["game_identity"].isString() && value["release_identity"].isString() &&
           value["save_compatibility"].isString();
}
QString fingerprintHash(const QJsonObject &value) {
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool sameMappings(const QJsonObject &a, const QJsonObject &b) {
    for (const auto &name : {"game_identity", "release_identity", "save_compatibility"})
        if (a[name] != b[name]) return false;
    const auto left = a["items"].toArray(), right = b["items"].toArray();
    if (left.size() != right.size()) return false;
    for (int i = 0; i < left.size(); ++i)
        if (left[i].toObject()["scope"] != right[i].toObject()["scope"] ||
            left[i].toObject()["path"] != right[i].toObject()["path"]) return false;
    return true;
}
}
QString defaultServerGameKey(const Library &library, const QString &gameId) {
    const auto config = loadSaveConfig(library, gameId);
    const auto id = config.serverGameKey.isEmpty() ? config.profileGameId : config.serverGameKey;
    if (id.isEmpty() || id.toUtf8().size() > 256) fail("This game has no valid server save key.");
    return id;
}
AcceptedServerSave loadAcceptedServerSave(const Library &library, const QString &gameId) {
    const auto path = library.gamePath(gameId, "server-accepted.json");
    if (!QFileInfo::exists(path)) return {};
    if (QFileInfo(path).isSymbolicLink() || QFileInfo(path).size() > 1024 * 1024)
        fail("Accepted server save record is invalid.");
    const auto object = readJson(path);
    AcceptedServerSave result;
    result.present = true;
    result.gameKey = object["game_key"].toString();
    result.serverId = object["server_id"].toString();
    result.revision = object["revision"].toInt(-1);
    result.snapshotId = object["snapshot_id"].toString();
    result.liveFingerprint = object["live_fingerprint"].toObject();
    if (object["schema"].toInt() != 1 || object["game_id"].toString() != gameId ||
        result.gameKey.isEmpty() || QUuid(result.serverId).isNull() || result.revision < 0 ||
        (result.revision > 0 && QUuid(result.snapshotId).isNull()) ||
        (result.revision == 0 && !result.snapshotId.isEmpty()) ||
        !validFingerprint(result.liveFingerprint) ||
        object["fingerprint_sha256"].toString() != fingerprintHash(result.liveFingerprint))
        fail("Accepted server save record is invalid.");
    return result;
}
void saveAcceptedServerSave(const Library &library, const QString &gameId, const AcceptedServerSave &state) {
    if (!state.present || state.gameKey.isEmpty() || QUuid(state.serverId).isNull() ||
        state.revision < 0 || (state.revision > 0 && QUuid(state.snapshotId).isNull()) ||
        (state.revision == 0 && !state.snapshotId.isEmpty()) || !validFingerprint(state.liveFingerprint))
        fail("Cannot accept an invalid server save state.");
    writeJson(library.gamePath(gameId, "server-accepted.json"),
              {{"schema", 1}, {"game_id", gameId}, {"game_key", state.gameKey},
               {"server_id", state.serverId}, {"revision", state.revision},
               {"snapshot_id", state.snapshotId}, {"live_fingerprint", state.liveFingerprint},
               {"fingerprint_sha256", fingerprintHash(state.liveFingerprint)}});
}
ServerSaveAction chooseServerSaveAction(const AcceptedServerSave &accepted,
                                        const RemoteSaveRevision &remote,
                                        const QJsonObject &live) {
    if (QUuid(remote.serverId).isNull() || remote.revision < 0 || remote.files < 0 ||
        (remote.revision > 0 && QUuid(remote.snapshotId).isNull()) ||
        (remote.revision == 0 && remote.files != 0) || !validFingerprint(live))
        fail("Cannot compare an invalid server or live save state.");
    if (!accepted.present) {
        if (live["files"].toInt(-1) == 0)
            return remote.revision == 0 ? ServerSaveAction::AcceptEmpty : ServerSaveAction::Download;
        return remote.files == 0 ? ServerSaveAction::Upload : ServerSaveAction::Conflict;
    }
    if (accepted.serverId != remote.serverId || remote.revision < accepted.revision ||
        (remote.revision == accepted.revision && remote.snapshotId != accepted.snapshotId) ||
        !sameMappings(accepted.liveFingerprint, live)) return ServerSaveAction::Conflict;
    const bool changed = accepted.liveFingerprint != live;
    if (remote.revision > accepted.revision)
        return changed ? ServerSaveAction::Conflict : ServerSaveAction::Download;
    return changed ? ServerSaveAction::Upload : ServerSaveAction::Play;
}

} // namespace novelty
