#include "core.h"
#include "saves.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qelapsedtimer.h>
#include <functional>
#include <cstdio>
#include <stdexcept>
#include <sys/resource.h>
#include <csignal>

using namespace novelty;
namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void touch(const QString &path, const QByteArray &bytes = {}) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture mkdir");
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture write");
}
QByteArray contents(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "fixture read"); return file.readAll();
}
void rejects(const std::function<void()> &action) {
    try { action(); } catch (const std::exception &) { return; }
    throw std::runtime_error("operation unexpectedly succeeded");
}
struct Fixture {
    QTemporaryDir directory;
    Library library;
    Game game;
    static QString checkedRoot(const QTemporaryDir &directory) {
        require(directory.isValid(), "Cannot create disposable audit library"); return directory.path();
    }
    Fixture() : library(checkedRoot(directory)) {
        touch(directory.path() + "/Game/game.exe");
        library.import({inspect(directory.path() + "/Game")}, {});
        game = library.games().first();
        auto config = loadSaveConfig(library, game.id); config.mappings = {{"game", "save"}};
        saveSaveConfig(library, game.id, config);
    }
    QString bundle(const QString &id) const {
        return QFileInfo(library.gamePath(game.id, "saves.json")).absolutePath() + "/snapshots/" + id;
    }
};
void mappingChanges() {
    Fixture source, target;
    touch(source.game.folder + "/save/slot.dat", "old mapping");
    const auto old = createSaveSnapshot(source.library, source.game.id);
    auto config = loadSaveConfig(source.library, source.game.id);
    auto remote = config; remote.profileId = target.library.profileId();
    saveSaveConfig(target.library, target.game.id, remote);
    importSaveSnapshot(target.library, target.game.id, source.bundle(old.id));
    restoreSaveSnapshot(target.library, target.game.id, old.id);
    config.mappings = {{"game", "UserData"}};
    saveSaveConfig(source.library, source.game.id, config);
    touch(source.game.folder + "/UserData/slot.dat", "new mapping");
    const auto fresh = createSaveSnapshot(source.library, source.game.id);
    require(readJson(source.bundle(fresh.id) + "/manifest.json")["parent_ids"].toArray().isEmpty(),
            "mapping change must start compatible ancestry");
    // Simulate a snapshot written by the prior build, which linked incompatible mappings.
    auto legacy = readJson(source.bundle(fresh.id) + "/manifest.json");
    legacy["parent_ids"] = QJsonArray{old.id};
    writeJson(source.bundle(fresh.id) + "/manifest.json", legacy);
    require(saveSnapshotLineage(source.library, source.game.id) == QStringList{fresh.id},
            "existing mixed-mapping ancestry is bounded without rewriting old snapshots");
    remote.mappings = config.mappings;
    saveSaveConfig(target.library, target.game.id, remote);
    for (const auto &id : saveSnapshotLineage(source.library, source.game.id))
        importSaveSnapshot(target.library, target.game.id, source.bundle(id));
    restoreSaveSnapshot(target.library, target.game.id, fresh.id);
    require(contents(target.game.folder + "/UserData/slot.dat") == "new mapping" &&
            contents(target.game.folder + "/save/slot.dat") == "old mapping", "mapping restore touches only the chosen location");
    rejects([&] { restoreSaveSnapshot(target.library, target.game.id, old.id); });
    remote.mappings = {{"game", "save"}}; saveSaveConfig(target.library, target.game.id, remote);
    restoreSaveSnapshot(target.library, target.game.id, old.id);
    require(listSaveSnapshots(target.library, target.game.id).size() >= 2, "old mapping history remains accessible");
}
void settingsRecovery() {
    Fixture f;
    const auto beforeGame = f.library.game(f.game.id);
    const auto beforeConfig = readJson(f.library.gamePath(f.game.id, "saves.json"));
    auto afterGame = beforeGame; afterGame.name = "Updated"; afterGame.source = "manual";
    auto afterConfig = beforeConfig;
    afterConfig["mappings"] = QJsonArray{QJsonObject{{"scope", "game"}, {"path", "UserData"}}};
    const QJsonObject journal{{"schema", 1}, {"before_game", beforeGame.json()}, {"after_game", afterGame.json()},
        {"before_config", beforeConfig}, {"after_config", afterConfig}, {"had_config", true}};
    const auto journalPath = f.library.gamePath(f.game.id, "settings-change.json");
    const auto configPath = f.library.gamePath(f.game.id, "saves.json");
    writeJson(configPath, afterConfig); writeJson(journalPath, journal);
    require(!f.library.recoveryIssue(f.game.id).isEmpty(), "interrupted settings must block Play");
    f.library.recoverSettingsChanges();
    require(readJson(configPath) == beforeConfig && !QFileInfo::exists(journalPath),
            "settings recovery rolls JSON back when SQLite did not commit");
    f.library.save(afterGame);
    writeJson(configPath, afterConfig); writeJson(journalPath, journal);
    f.library.recoverSettingsChanges();
    require(readJson(configPath) == afterConfig && !QFileInfo::exists(journalPath),
            "settings recovery retains JSON after SQLite committed");
    auto edited = loadSaveConfig(f.library, f.game.id); edited.mappings = {{"game", "save"}};
    auto invalid = afterGame; invalid.name.clear();
    rejects([&] { f.library.save(invalid, &edited); });
    require(readJson(configPath) == afterConfig && f.library.game(f.game.id).name == "Updated",
            "invalid game settings cannot persist a new mapping");
    require(QFile::setPermissions(configPath, QFile::ReadOwner), "fixture read-only saves config");
    auto rejected = afterGame; rejected.name = "Cannot write mapping";
    rejects([&] { f.library.save(rejected, &edited); });
    require(QFile::setPermissions(configPath, QFile::ReadOwner | QFile::WriteOwner), "restore config permissions");
    require(readJson(configPath) == afterConfig && f.library.game(f.game.id).name == "Updated" &&
            !QFileInfo::exists(journalPath), "failed JSON publication rolls back SQLite and clears recovery intent");
    // A failed JSON write must roll the database back, including a changed name.
    require(QFile::rename(configPath, configPath + ".kept") && QDir().mkdir(configPath), "fixture unreadable config");
    auto renamed = afterGame; renamed.name = "Should not persist";
    rejects([&] { f.library.save(renamed, &edited); });
    require(f.library.game(f.game.id).name == "Updated", "save settings I/O failure rolls back SQLite");
}
void boundedBackup() {
    Fixture f;
    const auto path = f.game.folder + "/save/assets.bin";
    touch(path);
    QFile huge(path); require(huge.open(QIODevice::WriteOnly) && huge.resize(4LL * 1024 * 1024 * 1024 + 1), "fixture sparse asset");
    huge.close();
    setSaveDiscoveryState(f.library, f.game.id, "searching");
    beginSaveObservation(f.library, f.game);
    require(loadSaveConfig(f.library, f.game.id).discoverySource == "heuristic",
            "conventional folder discovery can select an asset-sized directory");
    QElapsedTimer elapsed; elapsed.start();
    rejects([&] { createSaveSnapshot(f.library, f.game.id); });
    require(elapsed.elapsed() < 2000 && listSaveSnapshots(f.library, f.game.id).isEmpty(),
            "large mistaken save locations are bounded before hashing or copying");
    const auto runner = f.directory.path() + "/runner", fake = f.directory.path() + "/umu";
    touch(runner + "/proton");
    touch(fake, "#!/bin/sh\nmkdir -p \"$WINEPREFIX\"\ntouch \"$WINEPREFIX/system.reg\"\nexit 0\n");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto game = f.library.game(f.game.id); game.runner = runner; f.library.save(game);
    f.library.setSettings({{"umu", fake}});
    require(supervise(f.library, game.id, false) == 0 && f.library.status(f.library.game(game.id)) == "Save needs attention" &&
            !f.library.busy(game.id) && !pendingSaveSnapshotIssue(f.library, game.id).isEmpty(),
            "failed post-play backups stay visible while offline Play remains available");
    touch(path, "legitimate small progress");
    recoverPendingSaveSnapshots(f.library);
    require(pendingSaveSnapshotIssue(f.library, game.id).isEmpty() &&
            f.library.status(f.library.game(game.id)) == "Ready", "Rescan clears the warning after a successful retry");
}
void unreadableSaveDirectory() {
    Fixture f;
    touch(f.game.folder + "/save/readable.dat", "progress");
    const auto hidden = f.game.folder + "/save/private";
    touch(hidden + "/slot.dat", "must not silently disappear");
    require(QFile::setPermissions(hidden, QFile::WriteOwner | QFile::ExeOwner), "fixture unreadable save directory");
    bool failed = false;
    try { createSaveSnapshot(f.library, f.game.id); } catch (const std::exception &) { failed = true; }
    QFile::setPermissions(hidden, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    require(failed && listSaveSnapshots(f.library, f.game.id).isEmpty(), "unreadable save subdirectories must not become empty successful backups");
}
void legacySnapshots() {
    Fixture f;
    const QStringList names{"A.dat", "a.dat", "Z.dat", "é.dat", "10.dat", "2.dat"};
    for (const auto &name : names) touch(f.game.folder + "/save/" + name, name.toUtf8());
    const auto snapshot = createSaveSnapshot(f.library, f.game.id);
    const auto manifestPath = f.bundle(snapshot.id) + "/manifest.json";
    auto manifest = readJson(manifestPath);
    const auto entries = manifest["items"].toArray().first().toObject()["entries"].toArray();
    const auto previousOrder = QDir(f.game.folder + "/save").entryList(QDir::Files, QDir::Name);
    for (int i = 0; i < previousOrder.size(); ++i)
        require(entries[i + 1].toObject()["path"] == previousOrder[i], "inventory ordering must remain compatible with existing snapshots");
    manifest["schema"] = 1; manifest.remove("parent_ids"); writeJson(manifestPath, manifest);
    touch(f.game.folder + "/save/A.dat", "later progress");
    restoreSaveSnapshot(f.library, f.game.id, snapshot.id);
    require(contents(f.game.folder + "/save/A.dat") == "A.dat" && listSaveSnapshots(f.library, f.game.id).size() == 2,
            "schema-1 snapshots remain restorable with a verified backup of current progress");
    require(readJson(manifestPath)["schema"] == 1, "legacy history is not rewritten");
}
void metadataQueries() {
    Fixture f;
    touch(f.game.folder + "/save/slot.dat", "progress");
    const auto snapshot = createSaveSnapshot(f.library, f.game.id);
    const auto stored = f.bundle(snapshot.id) + "/items/0/slot.dat";
    require(QFile::setPermissions(stored, QFile::WriteOwner), "fixture unreadable snapshot data");
    require(saveSnapshotLineage(f.library, f.game.id) == QStringList{snapshot.id},
            "listing lineage must not read historical save payloads on the UI thread");
    rejects([&] { verifiedSaveSnapshotBundle(f.library, f.game.id, snapshot.id); });
    QFile::setPermissions(stored, QFile::ReadOwner | QFile::WriteOwner);
    require(contents(f.game.folder + "/save/slot.dat") == "progress", "unreadable snapshots cannot change current saves");
}
void recoveryCopyFailure() {
    Fixture f;
    const auto path = f.game.folder + "/save/slot.dat";
    touch(path, QByteArray(16384, 'o'));
    const auto original = createSaveSnapshot(f.library, f.game.id);
    touch(path, QByteArray(16384, 'i'));
    const auto incoming = createSaveSnapshot(f.library, f.game.id);
    const auto nonce = incoming.id;
    writeJson(f.library.gamePath(f.game.id, "save-restore.json"), {{"schema", 1}, {"nonce", nonce},
        {"snapshot", incoming.id}, {"backup", original.id}, {"previous_head", original.id},
        {"items", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "save"}, {"index", 0}, {"original_present", true}}}}});
    rlimit previous{}; require(getrlimit(RLIMIT_FSIZE, &previous) == 0, "fixture file-size limit");
    const auto oldSignal = std::signal(SIGXFSZ, SIG_IGN);
    rlimit limit = previous; limit.rlim_cur = 4096;
    require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "fixture enables failed writes");
    recoverSaveRestores(f.library);
    require(setrlimit(RLIMIT_FSIZE, &previous) == 0, "fixture restores file-size limit");
    std::signal(SIGXFSZ, oldSignal);
    require(QFileInfo::exists(f.library.gamePath(f.game.id, "save-restore.json")), "I/O fault must leave a recovery journal");
    require(contents(path) == QByteArray(16384, 'i'), "failed rollback copy must preserve installed saves");
    recoverSaveRestores(f.library);
    require(contents(path) == QByteArray(16384, 'o') && f.library.recoveryIssue(f.game.id).isEmpty(),
            "rollback can retry after an I/O failure without manual file repair");
}
void recoveryCleanupFailure() {
    Fixture f;
    const auto target = f.game.folder + "/save";
    touch(target + "/slot.dat", "original");
    const auto original = createSaveSnapshot(f.library, f.game.id);
    touch(target + "/slot.dat", "incoming");
    touch(target + "/a.dat", "incoming file removed first");
    touch(target + "/locked/slot.dat", "incoming file that cannot be removed");
    const auto incoming = createSaveSnapshot(f.library, f.game.id);
    const auto held = target + ".novelty-held-" + incoming.id;
    const auto staged = target + ".novelty-incoming-" + incoming.id;
    touch(held + "/slot.dat", "original");
    writeJson(f.library.gamePath(f.game.id, "save-restore.json"), {{"schema", 1}, {"nonce", incoming.id},
        {"snapshot", incoming.id}, {"backup", original.id}, {"previous_head", original.id},
        {"items", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "save"}, {"index", 0}, {"original_present", true}}}}});
    require(QFile::setPermissions(target + "/locked", QFile::ReadOwner | QFile::ExeOwner), "fixture prevents cleanup");
    recoverSaveRestores(f.library);
    const bool restored = QFileInfo::exists(target + "/slot.dat") && contents(target + "/slot.dat") == "original";
    for (const auto &path : {target + "/locked", staged + "/locked"})
        if (QFileInfo::exists(path)) QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    recoverSaveRestores(f.library);
    require(restored && f.library.recoveryIssue(f.game.id).isEmpty() && contents(target + "/slot.dat") == "original",
            "partial incoming cleanup must not prevent restoring held originals or retrying recovery");
}
void committedRestoreCleanup() {
    Fixture f;
    const auto target = f.game.folder + "/save";
    touch(target + "/slot.dat", "restore this progress");
    const auto chosen = createSaveSnapshot(f.library, f.game.id);
    touch(target + "/slot.dat", "newer original progress");
    touch(target + "/locked/slot.dat", "original folder that cannot be cleaned");
    require(QFile::setPermissions(target + "/locked", QFile::ReadOwner | QFile::ExeOwner), "fixture prevents held cleanup");
    rejects([&] { restoreSaveSnapshot(f.library, f.game.id, chosen.id); });
    const auto journal = readJson(f.library.gamePath(f.game.id, "save-restore.json"));
    const auto held = target + ".novelty-held-" + journal["nonce"].toString();
    if (QFileInfo::exists(held + "/locked")) QFile::setPermissions(held + "/locked", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    require(journal["phase"] == "committed", "completed restore needs a durable cleanup phase before deleting originals");
    recoverSaveRestores(f.library);
    require(f.library.recoveryIssue(f.game.id).isEmpty() && contents(target + "/slot.dat") == "restore this progress" &&
            appliedSaveSnapshotId(f.library, f.game.id) == chosen.id && listSaveSnapshots(f.library, f.game.id).size() == 2,
            "cleanup recovery retains the committed restore, its head, and the full safety backup");
}
void unappliedImport() {
    Fixture source, target;
    touch(source.game.folder + "/save/slot.dat", "remote progress");
    const auto remote = createSaveSnapshot(source.library, source.game.id);
    auto config = loadSaveConfig(source.library, source.game.id); config.profileId = target.library.profileId();
    saveSaveConfig(target.library, target.game.id, config);
    importSaveSnapshot(target.library, target.game.id, source.bundle(remote.id));
    touch(target.game.folder + "/save/slot.dat", "independent local progress");
    const auto local = createSaveSnapshot(target.library, target.game.id);
    require(readJson(target.bundle(local.id) + "/manifest.json")["parent_ids"].toArray().isEmpty(),
            "an imported but unapplied snapshot must not become the local ancestor");
}
void verifiedEmptyState() {
    Fixture f;
    const auto target = f.game.folder + "/save";
    touch(target + "/slot.dat", "progress to restore");
    const auto populated = createSaveSnapshot(f.library, f.game.id);
    require(QFile::remove(target + "/slot.dat"), "fixture removes save file");
    const auto empty = captureVerifiedSaveState(f.library, f.game.id, "explicit empty state");
    require(empty.files == 0 && empty.id != populated.id &&
            readJson(f.bundle(empty.id) + "/manifest.json")["files"].toInt(-1) == 0,
            "a verified deletion has its own durable empty snapshot");
    touch(target + "/slot.dat", "later local progress");
    restoreSaveSnapshot(f.library, f.game.id, empty.id);
    require(!QFileInfo::exists(target + "/slot.dat") && appliedSaveSnapshotId(f.library, f.game.id) == empty.id,
            "an empty state restores as a real deletion");
    restoreSaveSnapshot(f.library, f.game.id, populated.id);
    require(contents(target + "/slot.dat") == "progress to restore",
            "older populated snapshots remain restorable after an empty state");

    auto config = loadSaveConfig(f.library, f.game.id);
    config.mappings = {{"prefix", "drive_c/save"}};
    saveSaveConfig(f.library, f.game.id, config);
    rejects([&] { captureVerifiedSaveState(f.library, f.game.id, "missing prefix"); });
    config.mappings = {{"game", "save"}};
    saveSaveConfig(f.library, f.game.id, config);
    require(QDir(f.game.folder).rename("save", "save.offline"), "fixture moves save root");
    // An absent mapped path is valid only while the owning game folder remains available.
    require(captureVerifiedSaveState(f.library, f.game.id, "absent mapped path").files == 0,
            "missing save leaf under an available game root can be an empty state");
    require(QDir().rename(f.game.folder, f.game.folder + ".offline"), "fixture disconnects game folder");
    rejects([&] { captureVerifiedSaveState(f.library, f.game.id, "missing game folder"); });
}
void manualTransferUndoCase() {
    Fixture source, target;
    const auto sourcePath = source.game.folder + "/save/slot.dat";
    const auto targetPath = target.game.folder + "/save/slot.dat";
    touch(sourcePath, "peer progress");
    const auto peer = createSaveSnapshot(source.library, source.game.id);
    auto config = loadSaveConfig(source.library, source.game.id);
    config.profileId = target.library.profileId();
    saveSaveConfig(target.library, target.game.id, config);
    importSaveSnapshot(target.library, target.game.id, source.bundle(peer.id));
    touch(targetPath, "local progress");
    replaceSavesFromPeer(target.library, target.game.id, peer.id);
    require(contents(targetPath) == "peer progress", "manual peer replacement installs the chosen state");
    {
        Library reopened(target.directory.path());
        const auto undo = peerTransferUndo(reopened, target.game.id);
        require(!undo.rollbackSnapshotId.isEmpty() && undo.installedSnapshotId == peer.id && !undo.liveChanged,
                "the one-transfer Undo point survives closing and reopening the library");
    }
    touch(targetPath, "progress after transfer");
    require(peerTransferUndo(target.library, target.game.id).liveChanged,
            "Undo detects live progress after the transfer");
    rejects([&] { undoPeerSaveTransfer(target.library, target.game.id, false); });
    require(contents(targetPath) == "progress after transfer", "unconfirmed Undo preserves changed progress");
    undoPeerSaveTransfer(target.library, target.game.id, true);
    require(contents(targetPath) == "local progress" &&
            peerTransferUndo(target.library, target.game.id).rollbackSnapshotId.isEmpty(),
            "confirmed Undo restores prior progress and consumes the dedicated Undo point");
    bool preserved = false;
    for (const auto &snapshot : listSaveSnapshots(target.library, target.game.id))
        if (snapshot.reason == "before peer transfer undo") preserved = true;
    require(preserved, "changed progress remains available in general snapshot history after Undo");

    require(QFile::remove(sourcePath), "fixture clears source progress");
    const auto emptyPeer = captureVerifiedSaveState(source.library, source.game.id, "source deleted saves");
    importSaveSnapshot(target.library, target.game.id, source.bundle(emptyPeer.id));
    replaceSavesFromPeer(target.library, target.game.id, emptyPeer.id);
    require(!QFileInfo::exists(targetPath), "an explicit peer transfer can install a verified empty state");
    undoPeerSaveTransfer(target.library, target.game.id, false);
    require(contents(targetPath) == "local progress", "Undo recovers populated saves after an empty peer transfer");

    Fixture fresh;
    config.profileId = fresh.library.profileId();
    saveSaveConfig(fresh.library, fresh.game.id, config);
    importSaveSnapshot(fresh.library, fresh.game.id, source.bundle(peer.id));
    replaceSavesFromPeer(fresh.library, fresh.game.id, peer.id);
    require(peerTransferUndo(fresh.library, fresh.game.id).rollbackSnapshotId.size() > 0,
            "an initially empty installation has a durable transfer Undo point");
    undoPeerSaveTransfer(fresh.library, fresh.game.id, false);
    require(!QFileInfo::exists(fresh.game.folder + "/save/slot.dat"),
            "Undo returns a fresh installation to its verified empty state");
}
void peerTransferRecovery() {
    Fixture source, target;
    touch(source.game.folder + "/save/slot.dat", "peer one");
    const auto first = createSaveSnapshot(source.library, source.game.id);
    touch(source.game.folder + "/save/slot.dat", "peer two");
    const auto second = createSaveSnapshot(source.library, source.game.id);
    auto config = loadSaveConfig(source.library, source.game.id);
    config.profileId = target.library.profileId();
    saveSaveConfig(target.library, target.game.id, config);
    importSaveSnapshot(target.library, target.game.id, source.bundle(first.id));
    importSaveSnapshot(target.library, target.game.id, source.bundle(second.id));
    touch(target.game.folder + "/save/slot.dat", "local original");
    const auto confirmed = captureVerifiedSaveState(target.library, target.game.id, "preview");
    touch(target.game.folder + "/save/slot.dat", "changed after preview");
    rejects([&] { replaceSavesFromPeer(target.library, target.game.id, first.id, confirmed.id); });
    require(contents(target.game.folder + "/save/slot.dat") == "changed after preview" &&
            !QFileInfo::exists(target.library.gamePath(target.game.id, "save-transfer-undo.json")),
            "a stale confirmation never replaces newer local progress or publishes Undo");
    replaceSavesFromPeer(target.library, target.game.id, first.id);
    const auto oldUndo = peerTransferUndo(target.library, target.game.id);
    const auto intentPath = target.library.gamePath(target.game.id, "save-transfer-intent.json");
    writeJson(intentPath, {{"schema", 1}, {"kind", "replace"}, {"game_id", target.game.id},
                           {"rollback_snapshot", first.id}, {"installed_snapshot", second.id}});
    recoverPeerSaveTransfers(target.library);
    require(!QFileInfo::exists(intentPath) &&
            peerTransferUndo(target.library, target.game.id).rollbackSnapshotId == oldUndo.rollbackSnapshotId &&
            contents(target.game.folder + "/save/slot.dat") == "peer one",
            "pre-commit interruption keeps the previous dedicated Undo point");
    restoreSaveSnapshot(target.library, target.game.id, second.id);
    writeJson(intentPath, {{"schema", 1}, {"kind", "replace"}, {"game_id", target.game.id},
                           {"rollback_snapshot", first.id}, {"installed_snapshot", second.id}});
    recoverPeerSaveTransfers(target.library);
    require(!QFileInfo::exists(intentPath) &&
            peerTransferUndo(target.library, target.game.id).rollbackSnapshotId == first.id &&
            contents(target.game.folder + "/save/slot.dat") == "peer two",
            "post-commit interruption publishes the new Undo point after restart recovery");
    replaceSavesFromPeer(target.library, target.game.id, first.id);
    require(peerTransferUndo(target.library, target.game.id).rollbackSnapshotId == second.id,
            "a later successful transfer replaces only the dedicated one-transfer Undo point");
    undoPeerSaveTransfer(target.library, target.game.id, false);
    require(contents(target.game.folder + "/save/slot.dat") == "peer two",
            "Undo returns to the state directly before the most recent transfer");
}
void failedRelink() {
    Fixture f;
    const auto original = f.library.game(f.game.id).json();
    const auto destination = f.directory.path() + "/Renamed";
    require(QDir().rename(f.game.folder, destination), "fixture rename");
    const auto plan = f.library.planRelink(f.game.id, destination);
    touch(f.library.gamePath(f.game.id, "saves.json"), "broken JSON");
    rejects([&] { f.library.relink(plan); });
    require(f.library.game(f.game.id).json() == original, "failed alias update must not commit a relink");
}
}
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    for (const auto &[name, test] : std::initializer_list<std::pair<const char *, std::function<void()>>>{
             {"mapping changes", mappingChanges}, {"unapplied imports", unappliedImport}, {"failed relink", failedRelink},
             {"settings recovery", settingsRecovery}, {"restore recovery copy failure", recoveryCopyFailure},
             {"restore recovery cleanup failure", recoveryCleanupFailure},
             {"committed restore cleanup", committedRestoreCleanup},
             {"bounded backups and visible failures", boundedBackup}, {"unreadable save directory", unreadableSaveDirectory},
             {"legacy snapshot compatibility", legacySnapshots}, {"metadata-only history queries", metadataQueries},
             {"verified empty state", verifiedEmptyState}, {"peer transfer undo", manualTransferUndoCase},
             {"peer transfer recovery", peerTransferRecovery}}) {
        try { test(); std::printf("PASS: %s\n", name); }
        catch (const std::exception &e) { ++failures; std::fprintf(stderr, "FAIL: %s: %s\n", name, e.what()); }
    }
    return failures ? 1 : 0;
}
