#include "filesystem_checks.h"
#include "save_server_config.h"
#include "filesystem_durability.h"
#include "save_internal.h"

namespace novelty {
namespace save_detail {
QString stageSuffix(const QString &nonce, const QString &type) { return ".novelty-" + type + "-" + nonce; }
void rollback(const Library &lib, const Game &game, const QJsonObject &journal) {
    const auto nonce = journal["nonce"].toString();
    if (journal["schema"].toInt() != 1 || !validToken(nonce) || !journal["items"].isArray()) fail("Invalid restore journal; inspect saves before playing.");
    const auto phase = journal["phase"].toString();
    if (!phase.isEmpty() && phase != "committed") fail("Unknown restore recovery phase; inspect saves before playing.");
    const bool committed = phase == "committed";
    if (committed) writeDurableJson(journalPath(lib, game.id), journal);
    const auto snapshot = snapshotManifest(lib, game.id, journal["snapshot"].toString());
    const auto backupId = journal["backup"].toString();
    const auto backup = backupId.isEmpty() ? QJsonObject{} : snapshotManifest(lib, game.id, backupId);
    for (const auto &value : journal["items"].toArray()) {
        const auto item = value.toObject();
        const SaveMapping mapping{item["scope"].toString(), item["path"].toString()};
        const auto target = targetPath(lib, game, mapping);
        const auto held = target + stageSuffix(nonce, "held"), staged = target + stageSuffix(nonce, "incoming");
        const auto recoveryStage = target + stageSuffix(nonce, "rollback");
        noLinks(held); noLinks(staged); noLinks(recoveryStage);
        const auto index = item["index"].toInt(-1);
        if (index < 0 || index >= snapshot["items"].toArray().size() ||
            (!backupId.isEmpty() && index >= backup["items"].toArray().size()) ||
            snapshot["items"].toArray()[index].toObject()["scope"] != mapping.scope ||
            snapshot["items"].toArray()[index].toObject()["path"] != mapping.path ||
            (!backupId.isEmpty() && (backup["items"].toArray()[index].toObject()["scope"] != mapping.scope ||
             backup["items"].toArray()[index].toObject()["path"] != mapping.path)))
            fail("Restore journal does not match its snapshots.");
        const auto incomingEntries = snapshot["items"].toArray()[index].toObject()["entries"].toArray();
        const auto originalEntries = backupId.isEmpty() ? item["original_entries"].toArray() :
                                     backup["items"].toArray()[index].toObject()["entries"].toArray();
        if (backupId.isEmpty()) {
            if (!item["original_entries"].isArray()) fail("Restore journal lacks the original empty-save inventory.");
            for (const auto &entry : originalEntries) if (entry.toObject()["type"] == "file")
                fail("Restore journal has unsnapshotted original save files.");
        }
        if (committed) {
            if (inventory(target) != incomingEntries) fail("Restored saves changed before cleanup. Inspect " + target + " before retrying.");
            // Held directories may be partially deleted after the commit point. They are
            // cleanup work now, never a replacement for the verified installed saves.
            if (exists(held)) removeNode(held);
            if (exists(staged)) removeNode(staged);
            persistParent(target);
            continue;
        }
        if (exists(recoveryStage)) {
            if (exists(staged) && !exists(target)) {
                if (inventory(recoveryStage) != originalEntries || !QDir().rename(recoveryStage, target))
                    fail("Cannot finish restoring the original save at " + target);
            } else removeNode(recoveryStage); // A copy interrupted before the original target was moved.
        }
        if (exists(held)) {
            if (inventory(held) != originalEntries) fail("The held original save changed. Inspect " + held + " before retrying.");
            if (exists(target) && inventory(target) != incomingEntries)
                fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
            if (exists(target) && (exists(staged) || !QDir().rename(target, staged)))
                fail("Cannot hold the incoming save during recovery: " + target);
            if (!QDir().rename(held, target)) fail("Cannot restore the original save at " + target);
        } else if (!exists(staged)) {
            // The incoming item was installed, and the held copy may already have been cleaned.
            // Restore from the verified safety snapshot if the target still has the incoming bytes.
            const auto currentEntries = inventory(target);
            if (currentEntries == originalEntries) {
                if (exists(target)) persistTree(target);
                persistParent(target);
                continue;
            } // This item was not changed yet.
            if (currentEntries != incomingEntries)
                fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
            if (!backupId.isEmpty()) {
                const auto original = snapshotsRoot(lib, game.id) + '/' + backupId + "/items/" + QString::number(index);
                if (exists(original)) copyNode(original, recoveryStage);
            } else if (!originalEntries.isEmpty()) {
                for (const auto &entry : originalEntries) {
                    const auto path = entry.toObject()["path"].toString();
                    if (entry.toObject()["type"] != "directory" ||
                        (!path.isEmpty() && cleanMappingPath(path) != path) ||
                        !QDir().mkpath(path.isEmpty() ? recoveryStage : recoveryStage + '/' + path))
                        fail("Cannot recreate the original empty save folder.");
                }
            }
            if (inventory(recoveryStage) != originalEntries) fail("Cannot verify the original save during recovery.");
            if (exists(recoveryStage)) persistTree(recoveryStage);
            persistParent(recoveryStage);
            // Keep the installed copy until the backup is fully staged, so another I/O failure
            // or kill can retry recovery without mistaking a partial copy for player edits.
            if (exists(target) && !QDir().rename(target, staged)) fail("Cannot hold the restored save during recovery.");
            if (exists(recoveryStage) && !QDir().rename(recoveryStage, target)) fail("Cannot install the original save during recovery.");
        } else if (inventory(target) != originalEntries) {
            fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
        }
        if (exists(target)) persistTree(target);
        persistParent(target); // recovered originals must survive before retiring the incoming copy
        if (exists(staged)) removeNode(staged);
        persistParent(target);
    }
    const auto previousHead = journal["previous_head"].toString();
    setHead(lib, game.id, committed ? journal["snapshot"].toString() : previousHead.isEmpty() ? backupId : previousHead);
    removeDurableFile(journalPath(lib, game.id));
}
} // namespace save_detail
using namespace save_detail;

void restoreSaveSnapshotLocked(Library &lib, const QString &gameId, const QString &snapshotId) {
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    const auto manifest = snapshotManifest(lib, gameId, snapshotId);
    if (manifest["schema"].toInt() == 2 &&
        (!profileGameMatches(config, manifest["profile_game_id"].toString()) ||
         !labelsCompatible(config, manifest)))
        fail("This snapshot belongs to a different game edition or save format. No files were changed.");
    const auto items = manifest["items"].toArray();
    if (items.size() != config.mappings.size()) fail("Save mappings changed since this snapshot. Restore the original mappings before continuing.");
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["scope"].toString() != config.mappings[i].scope ||
            items[i].toObject()["path"].toString() != config.mappings[i].path)
            fail("Save mappings changed since this snapshot. Restore the original mappings before continuing.");
    const auto previousHead = currentHead(lib, gameId);
    QString backupId;
    try { backupId = snapshotLocked(lib, game, config, "before restore " + snapshotId).id; }
    catch (const NoSaveData &) { /* Fresh installation; the journal records its empty roots. */ }
    // Backup creation has its own preflight. Reserve the incoming stage on every
    // destination filesystem before making any live-save changes.
    for (const auto &mapping : config.mappings)
        requireAvailableSpace(QFileInfo(targetPath(lib, game, mapping)).absolutePath(),
                              static_cast<qint64>(manifest["bytes"].toDouble()));
    const auto nonce = token();
    QJsonArray journalItems;
    try {
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto staged = target + stageSuffix(nonce, "incoming"), held = target + stageSuffix(nonce, "held");
            if (exists(staged) || exists(held)) fail("A temporary restore path already exists. Inspect it before retrying: " + target);
            journalItems.append(QJsonObject{{"scope", config.mappings[i].scope}, {"path", config.mappings[i].path},
                                            {"index", i}, {"original_present", exists(target)},
                                            {"original_entries", inventory(target)}});
        }
        const QJsonObject journal{{"schema", 1}, {"nonce", nonce}, {"snapshot", snapshotId},
                                  {"backup", backupId}, {"previous_head", previousHead}, {"items", journalItems}};
        // Publish recovery intent before writing any sibling stage. A process kill during copy
        // then leaves a journal that can remove the partial stage without touching originals.
        writeDurableJson(journalPath(lib, gameId), journal);
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto staged = target + stageSuffix(nonce, "incoming");
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) fail("Cannot create save parent folder: " + target);
            const auto source = snapshotsRoot(lib, gameId) + '/' + snapshotId + "/items/" + QString::number(i);
            if (exists(source)) copyNode(source, staged);
            if (inventory(staged) != items[i].toObject()["entries"].toArray()) fail("Cannot verify staged save restore.");
            if (exists(staged)) persistTree(staged);
            persistParent(staged);
        }
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto held = target + stageSuffix(nonce, "held"), staged = target + stageSuffix(nonce, "incoming");
            if (exists(target) && !QDir().rename(target, held)) fail("Cannot hold original save: " + target);
            if (exists(staged) && !QDir().rename(staged, target)) fail("Cannot install restored save: " + target);
            persistParent(target);
        }
        auto committed = journal; committed["phase"] = "committed";
        writeDurableJson(journalPath(lib, gameId), committed);
        setHead(lib, gameId, snapshotId);
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto held = target + stageSuffix(nonce, "held");
            if (exists(held)) removeNode(held);
        }
        for (const auto &item : config.mappings) persistParent(targetPath(lib, game, item));
        removeDurableFile(journalPath(lib, gameId));
    } catch (...) {
        if (exists(journalPath(lib, gameId))) {
            try { rollback(lib, game, readJson(journalPath(lib, gameId))); } catch (...) { /* Preserve journal for recovery. */ }
        } else for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]) + stageSuffix(nonce, "incoming");
            if (exists(target)) removeNode(target);
        }
        throw;
    }
}
void restoreSaveSnapshot(Library &lib, const QString &gameId, const QString &snapshotId) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before restoring another snapshot.");
    restoreSaveSnapshotLocked(lib, gameId, snapshotId);
}
void restoreSaveSnapshotDuringLaunch(Library &lib, const QString &gameId, const QString &snapshotId) {
    if (!saveRestoreIssue(lib, gameId).isEmpty() || !pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish save recovery or the pending post-play snapshot before server restore.");
    restoreSaveSnapshotLocked(lib, gameId, snapshotId);
}
PeerTransferUndo peerTransferUndo(const Library &lib, const QString &gameId) {
    const auto record = readJson(transferUndoPath(lib, gameId));
    if (record.isEmpty()) return {};
    const auto rollbackId = record["rollback_snapshot"].toString();
    const auto installedId = record["installed_snapshot"].toString();
    if (record["schema"].toInt() != 1 || record["game_id"].toString() != gameId ||
        !validToken(rollbackId) || !validToken(installedId))
        fail("The peer transfer Undo record is damaged. Inspect it before replacing saves.");
    snapshotManifest(lib, gameId, rollbackId);
    const auto installed = snapshotManifest(lib, gameId, installedId);
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    return {rollbackId, installedId, !liveMatchesSnapshot(lib, game, config, installed)};
}
void replaceSavesFromPeer(Library &lib, const QString &gameId, const QString &snapshotId,
                          const QString &expectedLocalSnapshotId) {
    SaveLocks locks(lib, gameId);
    if (loadSaveServerConfig(lib).enabled)
        fail("Disable automatic server sync before replacing or undoing peer saves.");
    if (!saveRestoreIssue(lib, gameId).isEmpty() || !pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish save recovery and pending capture before transferring saves.");
    if (snapshotId == currentHead(lib, gameId)) fail("Those peer saves are already installed.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    const auto incoming = snapshotManifest(lib, gameId, snapshotId);
    if (incoming["schema"].toInt() != 2 || !profileGameMatches(config, incoming["profile_game_id"].toString()) ||
        !labelsCompatible(config, incoming) || !mappingsMatch(config, incoming))
        fail("The selected peer save is incompatible with this installation.");
    const auto previous = snapshotLocked(lib, game, config, "before peer transfer", true);
    if (!expectedLocalSnapshotId.isEmpty() && previous.id != expectedLocalSnapshotId)
        fail("Local saves changed since confirmation. Review them and retry the transfer.");
    const auto intentPath = transferIntentPath(lib, gameId);
    writeDurableJson(intentPath, {{"schema", 1}, {"kind", "replace"}, {"game_id", gameId},
                           {"rollback_snapshot", previous.id}, {"installed_snapshot", snapshotId}});
    try {
        restoreSaveSnapshotLocked(lib, gameId, snapshotId);
    } catch (...) {
        if (!exists(journalPath(lib, gameId)) && currentHead(lib, gameId) != snapshotId) QFile::remove(intentPath);
        throw;
    }
    writeDurableJson(transferUndoPath(lib, gameId), {{"schema", 1}, {"game_id", gameId},
        {"rollback_snapshot", previous.id}, {"installed_snapshot", snapshotId}});
    removeDurableFile(intentPath);
}
void undoPeerSaveTransfer(Library &lib, const QString &gameId, bool preserveChanged) {
    SaveLocks locks(lib, gameId);
    if (loadSaveServerConfig(lib).enabled)
        fail("Disable automatic server sync before replacing or undoing peer saves.");
    if (!saveRestoreIssue(lib, gameId).isEmpty() || !pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish save recovery and pending capture before undoing a transfer.");
    const auto undo = peerTransferUndo(lib, gameId);
    if (undo.rollbackSnapshotId.isEmpty()) fail("There is no peer transfer to undo.");
    if (undo.liveChanged && !preserveChanged)
        fail("Saves changed since the transfer. Confirm that current progress should be kept before undoing.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    if (undo.liveChanged) snapshotLocked(lib, game, config, "before peer transfer undo", true);
    const auto intentPath = transferIntentPath(lib, gameId);
    writeDurableJson(intentPath, {{"schema", 1}, {"kind", "undo"}, {"game_id", gameId},
                           {"rollback_snapshot", undo.rollbackSnapshotId},
                           {"installed_snapshot", undo.installedSnapshotId}});
    try {
        restoreSaveSnapshotLocked(lib, gameId, undo.rollbackSnapshotId);
    } catch (...) {
        if (!exists(journalPath(lib, gameId)) && currentHead(lib, gameId) != undo.rollbackSnapshotId)
            QFile::remove(intentPath);
        throw;
    }
    removeDurableFile(transferUndoPath(lib, gameId));
    removeDurableFile(intentPath);
}
QString saveRestoreIssue(const Library &lib, const QString &gameId) {
    if (exists(journalPath(lib, gameId))) return "An interrupted save restore needs recovery before Play.";
    if (exists(transferIntentPath(lib, gameId))) return "An interrupted peer save transfer needs recovery before Play.";
    return {};
}
QStringList recoverSaveRestores(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        if (!exists(journalPath(lib, game.id))) continue;
        if (lib.supervisorActive(game.id)) continue;
        try {
            SaveLocks locks(lib, game.id);
            const auto journal = readJson(journalPath(lib, game.id));
            rollback(lib, game, journal);
            messages.append(journal["phase"] == "committed" ? "Completed save restore cleanup for " + game.name + "." :
                            "Recovered the original saves for " + game.name + " after an interrupted restore.");
        } catch (const std::exception &e) {
            messages.append("Save restore recovery for " + game.name + " needs attention: " + QString::fromUtf8(e.what()));
        }
    }
    return messages;
}
QStringList recoverPeerSaveTransfers(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        const auto intentPath = transferIntentPath(lib, game.id);
        if (!exists(intentPath) || lib.supervisorActive(game.id) || exists(journalPath(lib, game.id))) continue;
        try {
            SaveLocks locks(lib, game.id);
            const auto intent = readJson(intentPath);
            const auto kind = intent["kind"].toString();
            const auto rollbackId = intent["rollback_snapshot"].toString();
            const auto installedId = intent["installed_snapshot"].toString();
            if (intent["schema"].toInt() != 1 || intent["game_id"].toString() != game.id ||
                (kind != "replace" && kind != "undo") || !validToken(rollbackId) || !validToken(installedId))
                fail("Peer transfer intent is damaged.");
            const auto rollbackManifest = snapshotManifest(lib, game.id, rollbackId);
            const auto installedManifest = snapshotManifest(lib, game.id, installedId);
            const auto config = loadSaveConfig(lib, game.id);
            const auto head = currentHead(lib, game.id);
            if (kind == "replace" && head == installedId) {
                writeDurableJson(transferUndoPath(lib, game.id), {{"schema", 1}, {"game_id", game.id},
                    {"rollback_snapshot", rollbackId}, {"installed_snapshot", installedId}});
                messages.append("Completed peer transfer Undo recovery for " + game.name + ".");
            } else if (kind == "undo" && head == rollbackId &&
                       liveMatchesSnapshot(lib, game, config, rollbackManifest)) {
                if (exists(transferUndoPath(lib, game.id)) && !QFile::remove(transferUndoPath(lib, game.id)))
                    fail("Cannot clear completed peer transfer Undo record.");
                messages.append("Completed peer transfer Undo recovery for " + game.name + ".");
            } else {
                const auto &original = kind == "replace" ? rollbackManifest : installedManifest;
                if (kind == "replace" && !liveMatchesSnapshot(lib, game, config, original))
                    fail("Save files changed during peer transfer recovery.");
                // Undo may have captured changed progress before the restore intent. If the
                // rollback state was not installed, the previous Undo point stays available.
                messages.append("Retained the previous saves for " + game.name + " after an interrupted peer transfer.");
            }
            if (!QFile::remove(intentPath)) fail("Cannot clear completed peer transfer intent.");
        } catch (const std::exception &e) {
            messages.append("Peer transfer recovery for " + game.name + " needs attention: " + QString::fromUtf8(e.what()));
        }
    }
    return messages;
}
} // namespace novelty
