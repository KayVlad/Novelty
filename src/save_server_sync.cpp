#include "save_server_sync.h"
#include "save_server_client.h"
#include "save_server_state.h"
#include "saves.h"
#include <QtCore/qtemporarydir.h>
#include <QtCore/qlockfile.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QJsonObject fingerprint(const QJsonObject &manifest) {
    return {{"items", manifest["items"]}, {"files", manifest["files"]}, {"bytes", manifest["bytes"]},
            {"game_identity", manifest["game_identity"]}, {"release_identity", manifest["release_identity"]},
            {"save_compatibility", manifest["save_compatibility"]}};
}
void accept(const Library &library, const QString &gameId, const QString &key,
            const RemoteSaveRevision &remote, const QJsonObject &live) {
    saveAcceptedServerSave(library, gameId,
                           {true, key, remote.serverId, remote.revision, remote.snapshotId, live});
}
ServerSyncResult operate(Library &library, const QString &gameId, const SaveServerConfig &config,
                         bool beforePlay) {
    if (!config.enabled) return {ServerSyncStatus::Unavailable, {}};
    try {
        const auto key = defaultServerGameKey(library, gameId);
        const auto live = inspectVerifiedLiveSaveState(library, gameId);
        const auto accepted = loadAcceptedServerSave(library, gameId);
        if (accepted.present && accepted.gameKey != key)
            return {ServerSyncStatus::Conflict, "Server game link changed. Review the accepted save state."};
        SaveServerClient client(config.url, config.token);
        const auto remote = client.current(key);
        if (remote.serverId != config.serverId)
            return {ServerSyncStatus::Conflict, "Save server identity changed. Reconcile before syncing."};
        const auto action = chooseServerSaveAction(accepted, remote, live);
        if (action == ServerSaveAction::Conflict) {
            if (!accepted.present && remote.revision > 0 && remote.files == 0 && live["files"].toInt() > 0)
                return {ServerSyncStatus::Conflict,
                        "The server has a committed empty save state. Choose which saves to keep in Manage → Saves."};
            return {ServerSyncStatus::Conflict, "Local saves and the server revision both need review."};
        }
        if (action == ServerSaveAction::Play)
            return {ServerSyncStatus::Synced, {}};
        if (action == ServerSaveAction::AcceptEmpty) {
            accept(library, gameId, key, remote, live);
            return {ServerSyncStatus::Synced, {}};
        }
        if (action == ServerSaveAction::Download) {
            if (!beforePlay)
                return {ServerSyncStatus::Pending, "Newer server saves will be checked before the next Play."};
            QTemporaryDir temporary(library.dataRoot() + "/.server-transfer-XXXXXX");
            if (!temporary.isValid()) fail("Cannot stage server saves.");
            const auto bundle = temporary.path() + "/bundle";
            const auto received = client.download(key, bundle);
            if (received.serverId != remote.serverId || received.revision != remote.revision ||
                received.snapshotId != remote.snapshotId || received.files != remote.files)
                fail("Server revision changed during download. Local saves were preserved.");
            const auto manifest = verifyPortableSaveBundle(bundle);
            importSaveSnapshotDuringLaunch(library, gameId, bundle);
            restoreSaveSnapshotDuringLaunch(library, gameId, received.snapshotId);
            const auto applied = inspectVerifiedLiveSaveState(library, gameId);
            if (applied["items"] != manifest["items"])
                fail("Installed server saves do not match the verified download.");
            accept(library, gameId, key, received, applied);
            return {ServerSyncStatus::Synced, {}};
        }
        const auto snapshot = captureVerifiedSaveStateDuringLaunch(library, gameId, "before server upload");
        const auto bundle = verifiedSaveSnapshotBundle(library, gameId, snapshot.id);
        const auto manifest = verifyPortableSaveBundle(bundle);
        if (fingerprint(manifest) != live)
            fail("Local saves changed while preparing the server upload.");
        const auto uploaded = client.upload(key, remote.revision, bundle);
        if (uploaded.serverId != remote.serverId) fail("Save server identity changed during upload.");
        accept(library, gameId, key, uploaded, live);
        return {ServerSyncStatus::Synced, {}};
    } catch (const RemoteRevisionConflict &) {
        return {ServerSyncStatus::Conflict, "Server saves changed during upload. Both states were preserved."};
    } catch (const std::exception &error) {
        return {ServerSyncStatus::Pending, QString::fromUtf8(error.what())};
    }
}
}
ServerSyncResult syncServerBeforePlay(Library &library, const QString &gameId, const SaveServerConfig &config) {
    return operate(library, gameId, config, true);
}
ServerSyncResult syncServerAfterPlay(Library &library, const QString &gameId, const SaveServerConfig &config) {
    return operate(library, gameId, config, false);
}
ServerSyncResult retryPendingServerSave(Library &library, const QString &gameId,
                                        const SaveServerConfig &config) {
    if (!config.enabled) return {ServerSyncStatus::Unavailable, {}};
    QLockFile mutation(library.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock(2000)) return {ServerSyncStatus::Pending, "Library maintenance is in progress."};
    QLockFile launch(library.gamePath(gameId, "launch.lock")); launch.setStaleLockTime(0);
    if (!launch.tryLock()) return {ServerSyncStatus::Pending, "Game is still running."};
    if (!library.recoveryIssue(gameId).isEmpty() || !saveRestoreIssue(library, gameId).isEmpty())
        return {ServerSyncStatus::Pending, "Resolve game or save recovery before server sync."};
    try { finishPostExitSaveSnapshot(library, library.game(gameId)); }
    catch (const std::exception &error) {
        library.recordStatus(gameId, "Save needs attention", QString::fromUtf8(error.what()));
        return {ServerSyncStatus::Pending, QString::fromUtf8(error.what())};
    }
    const auto result = syncServerBeforePlay(library, gameId, config);
    if (result.status == ServerSyncStatus::Synced)
        library.recordStatus(gameId, "Ready", "Saves synced with the configured server.");
    else if (result.status == ServerSyncStatus::Conflict)
        library.recordStatus(gameId, "Save conflict", result.detail);
    else library.recordStatus(gameId, "Sync pending", result.detail);
    return result;
}
ServerConflictPreview reviewServerSaveConflict(const Library &library, const QString &gameId,
                                                const SaveServerConfig &config) {
    if (!config.enabled) fail("Automatic server sync is disabled.");
    const auto key = defaultServerGameKey(library, gameId);
    const auto live = inspectVerifiedLiveSaveState(library, gameId);
    SaveServerClient client(config.url, config.token);
    const auto remote = client.current(key);
    const auto accepted = loadAcceptedServerSave(library, gameId);
    if ((!accepted.present || accepted.gameKey == key) &&
        chooseServerSaveAction(accepted, remote, live) != ServerSaveAction::Conflict &&
        remote.serverId == config.serverId)
        fail("The server conflict has changed. Retry the ordinary sync check.");
    return {remote, live};
}
void resolveServerSaveConflict(Library &library, const QString &gameId, const SaveServerConfig &config,
                               const ServerConflictPreview &preview, bool chooseLocal) {
    if (!config.enabled) fail("Automatic server sync is disabled.");
    QLockFile mutation(library.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock(2000)) fail("Library maintenance is in progress. Retry shortly.");
    QLockFile launch(library.gamePath(gameId, "launch.lock")); launch.setStaleLockTime(0);
    if (!launch.tryLock()) fail("Wait until the game closes before resolving server saves.");
    if (!library.recoveryIssue(gameId).isEmpty() || !saveRestoreIssue(library, gameId).isEmpty() ||
        !pendingSaveSnapshotIssue(library, gameId).isEmpty())
        fail("Finish game and save recovery before resolving server saves.");
    const auto key = defaultServerGameKey(library, gameId);
    const auto live = inspectVerifiedLiveSaveState(library, gameId);
    if (live != preview.verifiedLive) fail("Local saves changed since the choice was shown. Review them again.");
    SaveServerClient client(config.url, config.token);
    const auto remote = client.current(key);
    if (remote.serverId != preview.remote.serverId || remote.revision != preview.remote.revision ||
        remote.snapshotId != preview.remote.snapshotId || remote.files != preview.remote.files)
        fail("Server saves changed since the choice was shown. Review them again.");
    RemoteSaveRevision accepted;
    QJsonObject applied;
    if (chooseLocal) {
        const auto snapshot = captureVerifiedSaveStateDuringLaunch(library, gameId, "before conflict resolution");
        const auto bundle = verifiedSaveSnapshotBundle(library, gameId, snapshot.id);
        if (fingerprint(verifyPortableSaveBundle(bundle)) != live)
            fail("Local saves changed while preparing the upload. Review them again.");
        accepted = client.upload(key, remote.revision, bundle);
        applied = live;
    } else {
        if (remote.revision == 0)
            fail("This server has no revision to restore. Choose local saves or disable sync and restore an empty local snapshot.");
        QTemporaryDir temporary(library.dataRoot() + "/.server-transfer-XXXXXX");
        if (!temporary.isValid()) fail("Cannot stage server saves.");
        const auto bundle = temporary.path() + "/bundle";
        accepted = client.download(key, bundle);
        if (accepted.serverId != remote.serverId || accepted.revision != remote.revision ||
            accepted.snapshotId != remote.snapshotId || accepted.files != remote.files)
            fail("Server saves changed during download. Review them again.");
        const auto manifest = verifyPortableSaveBundle(bundle);
        importSaveSnapshotDuringLaunch(library, gameId, bundle);
        restoreSaveSnapshotDuringLaunch(library, gameId, accepted.snapshotId);
        applied = inspectVerifiedLiveSaveState(library, gameId);
        if (applied["items"] != manifest["items"])
            fail("Restored server saves do not match the verified download.");
    }
    if (accepted.serverId != remote.serverId) fail("Server identity changed while resolving saves.");
    accept(library, gameId, key, accepted, applied);
    if (config.serverId != accepted.serverId) {
        auto rebound = config;
        rebound.serverId = accepted.serverId;
        saveSaveServerConfig(library, rebound);
    }
    library.recordStatus(gameId, "Ready", "Saves synced with the configured server.");
}
} // namespace novelty
