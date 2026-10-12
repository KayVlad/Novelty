#include "core.h"
#include "saves.h"
#include "save_server_store.h"
#include "save_bundle_archive.h"
#include "save_server_client.h"
#include "save_server_state.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qtemporarydir.h>
#include <functional>
#include <cstdio>
#include <stdexcept>

using namespace novelty;
namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void touch(const QString &path, const QByteArray &bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture directory");
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                              file.write(bytes) == bytes.size(), "fixture file");
}
void rejects(const std::function<void()> &action) {
    try { action(); } catch (const std::exception &) { return; }
    throw std::runtime_error("operation unexpectedly succeeded");
}
void revisionStore() {
    SaveServerClient local(QUrl("http://127.0.0.1:42428"), "fixture-token");
    SaveServerClient secure(QUrl("https://saves.example.invalid"), "fixture-token");
    rejects([&] { SaveServerClient invalid(QUrl("http://saves.example.invalid"), "fixture-token"); });
    rejects([&] { SaveServerClient invalid(QUrl("https://user:safe@saves.example.invalid"), "fixture-token"); });
    rejects([&] { SaveServerClient invalid(QUrl("https://saves.example.invalid"), "bad\r\nheader"); });
    rejects([&] { local.current("bad\nkey"); });
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary server store");
    const auto libraryRoot = temporary.path() + "/Games", serverRoot = temporary.path() + "/Server";
    touch(libraryRoot + "/Game/game.exe", {});
    Library library(libraryRoot);
    library.import({inspect(libraryRoot + "/Game")}, {});
    const auto game = library.games().first();
    auto config = loadSaveConfig(library, game.id); config.mappings = {{"game", "save"}};
    saveSaveConfig(library, game.id, config);
    const auto bundle = [&](const QString &id) {
        return QFileInfo(library.gamePath(game.id, "saves.json")).absolutePath() + "/snapshots/" + id;
    };
    touch(game.folder + "/save/slot.dat", "first progress");
    const auto first = createSaveSnapshot(library, game.id);
    const auto live = inspectVerifiedLiveSaveState(library, game.id);
    const auto syntheticId = QString("756b539d-1a2b-4ceb-8f16-af8a16639300");
    require(chooseServerSaveAction({}, {syntheticId, 0, {}}, live) == ServerSaveAction::Upload,
            "first populated client conditionally publishes to an empty server");
    require(chooseServerSaveAction({}, {syntheticId, 1, first.id, 1}, live) == ServerSaveAction::Conflict,
            "two populated states need an explicit first-connection choice");
    require(chooseServerSaveAction({}, {syntheticId, 1, first.id, 0}, live) == ServerSaveAction::Upload,
            "populated local saves conditionally publish over a verified empty server state");
    AcceptedServerSave baseline{true, defaultServerGameKey(library, game.id), syntheticId,
                                1, first.id, live};
    saveAcceptedServerSave(library, game.id, baseline);
    require(loadAcceptedServerSave(library, game.id).liveFingerprint == live &&
            chooseServerSaveAction(baseline, {syntheticId, 1, first.id, 1}, live) == ServerSaveAction::Play,
            "accepted state and matching live files permit Play");
    touch(library.gamePath(game.id, "server-accepted.json"), "{}");
    rejects([&] { loadAcceptedServerSave(library, game.id); });
    saveAcceptedServerSave(library, game.id, baseline);
    require(QDir().rename(game.folder, game.folder + "-unavailable"), "hide mapped game root");
    rejects([&] { inspectVerifiedLiveSaveState(library, game.id); });
    require(QDir().rename(game.folder + "-unavailable", game.folder), "restore mapped game root");
    require(chooseServerSaveAction(baseline, {syntheticId, 2, first.id, 1}, live) == ServerSaveAction::Download,
            "newer server with unchanged local files downloads");
    touch(game.folder + "/save/slot.dat", "unpublished progress");
    const auto changed = inspectVerifiedLiveSaveState(library, game.id);
    require(chooseServerSaveAction(baseline, {syntheticId, 1, first.id, 1}, changed) == ServerSaveAction::Upload &&
            chooseServerSaveAction(baseline, {syntheticId, 2, first.id, 1}, changed) == ServerSaveAction::Conflict,
            "changed live files upload only when the server has not advanced");
    require(chooseServerSaveAction(baseline, {QString("b3682114-810f-4d95-a121-754c783ba8ff"), 1, first.id, 1}, live)
                == ServerSaveAction::Conflict,
            "server identity change requires reconciliation");
    touch(game.folder + "/save/slot.dat", "first progress");
    const auto archive = temporary.path() + "/transfer.nvsb";
    const auto extracted = temporary.path() + "/Extracted";
    writePortableSaveArchive(bundle(first.id), archive);
    extractPortableSaveArchive(archive, extracted);
    require(verifyPortableSaveBundle(extracted) == verifyPortableSaveBundle(bundle(first.id)),
            "archive round trip preserves a verified populated save");
    const auto otherRoot = temporary.path() + "/OtherGames";
    touch(otherRoot + "/Game/game.exe", {});
    Library other(otherRoot);
    other.import({inspect(otherRoot + "/Game")}, {});
    const auto otherGame = other.games().first();
    auto otherConfig = loadSaveConfig(other, otherGame.id);
    otherConfig.mappings = {{"game", "save"}};
    saveSaveConfig(other, otherGame.id, otherConfig);
    rejects([&] { importSaveSnapshot(other, otherGame.id, extracted); });
    otherConfig.serverGameKey = "shared-advanced-link";
    saveSaveConfig(other, otherGame.id, otherConfig);
    require(importSaveSnapshot(other, otherGame.id, extracted).snapshotId == first.id,
            "an explicit advanced server link accepts a compatible independent installation");
    restoreSaveSnapshot(other, otherGame.id, first.id);
    QFile restored(otherGame.folder + "/save/slot.dat");
    require(restored.open(QIODevice::ReadOnly) && restored.readAll() == "first progress",
            "an explicitly linked server save restores through the usual journal");
    rejects([&] { extractPortableSaveArchive(archive, extracted); });
    {
        QFile file(archive);
        require(file.open(QIODevice::ReadOnly), "read archive fixture");
        auto data = file.readAll(); file.close();
        data.append('x'); touch(temporary.path() + "/trailing.nvsb", data);
        rejects([&] { extractPortableSaveArchive(temporary.path() + "/trailing.nvsb",
                                            temporary.path() + "/Trailing"); });
        data.chop(2); touch(temporary.path() + "/truncated.nvsb", data);
        rejects([&] { extractPortableSaveArchive(temporary.path() + "/truncated.nvsb",
                                            temporary.path() + "/Truncated"); });
        data.append('y');
        data[data.size() - 1] = char(data.back() ^ 1);
        touch(temporary.path() + "/corrupt.nvsb", data);
        rejects([&] { extractPortableSaveArchive(temporary.path() + "/corrupt.nvsb",
                                            temporary.path() + "/Corrupt"); });
        require(!QFileInfo(temporary.path() + "/Corrupt").exists(),
                "failed extraction removes its private stage");
        const auto metadataLength = (quint32(quint8(data[8])) << 24) | (quint32(quint8(data[9])) << 16) |
                                    (quint32(quint8(data[10])) << 8) | quint32(quint8(data[11]));
        auto metadata = QJsonDocument::fromJson(data.mid(12, metadataLength)).object();
        auto items = metadata["items"].toArray();
        auto item = items[0].toObject();
        auto entries = item["entries"].toArray();
        auto entry = entries[0].toObject();
        entry["path"] = "../../escape";
        entries[0] = entry; item["entries"] = entries; items[0] = item; metadata["items"] = items;
        const auto badMetadata = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        QByteArray hostile = data.left(8);
        for (int shift = 24; shift >= 0; shift -= 8)
            hostile.append(char((quint32(badMetadata.size()) >> shift) & 0xff));
        hostile += badMetadata; hostile += data.mid(12 + metadataLength);
        touch(temporary.path() + "/traversal.nvsb", hostile);
        rejects([&] { extractPortableSaveArchive(temporary.path() + "/traversal.nvsb",
                                            temporary.path() + "/Traversal"); });
        require(!QFileInfo(temporary.path() + "/escape").exists(), "archive path cannot escape stage");
    }
    const auto key = QString("confirmed-game:release-a");
    QString identity;
    {
        SaveServerStore server(serverRoot);
        require(!(QFileInfo(serverRoot).permissions() &
                  (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                   QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)),
                "server save storage is private to its owner");
        identity = server.serverId();
        require(server.current(key).revision == 0, "new server game starts at revision zero");
        const auto accepted = server.put(key, 0, bundle(first.id));
        require(accepted.serverId == identity && accepted.revision == 1 &&
                accepted.snapshotId == first.id &&
                verifyPortableSaveBundle(accepted.bundleDirectory)["id"].toString() == first.id,
                "first conditional upload publishes a complete verified revision");
    }
    SaveServerStore reopened(serverRoot);
    require(reopened.serverId() == identity && reopened.current(key).revision == 1,
            "server identity and committed revision survive restart");
    touch(game.folder + "/save/slot.dat", "second progress");
    const auto second = createSaveSnapshot(library, game.id);
    bool staleRevisionReported = false;
    try { reopened.put(key, 0, bundle(second.id)); }
    catch (const ServerRevisionConflict &conflict) { staleRevisionReported = conflict.currentRevision == 1; }
    require(staleRevisionReported, "stale upload reports the still-current revision");
    require(reopened.current(key).revision == 1 && reopened.current(key).snapshotId == first.id,
            "stale conditional upload leaves the accepted state unchanged");
    touch(bundle(second.id) + "/items/0/slot.dat", "corrupted");
    rejects([&] { reopened.put(key, 1, bundle(second.id)); });
    require(reopened.current(key).revision == 1, "bad payload cannot advance the current revision");
    touch(bundle(second.id) + "/items/0/slot.dat", "second progress");
    const auto gameStorage = QFileInfo(reopened.current(key).bundleDirectory).absolutePath() + "/..";
    require(QDir().mkpath(gameStorage + "/.stage-abandoned"), "fixture interrupted upload stage");
    const auto next = reopened.put(key, 1, bundle(second.id));
    require(next.revision == 2 && next.snapshotId == second.id &&
            reopened.current(key).snapshotId == second.id,
            "an abandoned stage cannot hide or replace a committed revision");
    require(QFileInfo(reopened.current(key).bundleDirectory).isDir(), "current bundle survives old stage");
    require(QFile::remove(game.folder + "/save/slot.dat"), "fixture deletes local save");
    const auto empty = captureVerifiedSaveState(library, game.id, "intentional deletion");
    const auto emptyLive = inspectVerifiedLiveSaveState(library, game.id);
    require(emptyLive["files"].toInt(-1) == 0 &&
            chooseServerSaveAction({}, {syntheticId, 0, {}, 0}, emptyLive) == ServerSaveAction::AcceptEmpty &&
            chooseServerSaveAction({}, {syntheticId, 1, first.id, 1}, emptyLive) == ServerSaveAction::Download,
            "verified empty local saves can accept an empty server or adopt a populated one");
    writePortableSaveArchive(bundle(empty.id), archive);
    extractPortableSaveArchive(archive, temporary.path() + "/Empty");
    require(verifyPortableSaveBundle(temporary.path() + "/Empty")["files"].toInt(-1) == 0,
            "archive round trip preserves a verified empty state");
    const auto deletion = reopened.put(key, 2, bundle(empty.id));
    require(deletion.revision == 3 &&
            verifyPortableSaveBundle(deletion.bundleDirectory)["files"].toInt(-1) == 0,
            "a verified empty state is a real server revision");
    auto changedMapping = loadSaveConfig(library, game.id);
    changedMapping.mappings = {{"game", "other-save"}};
    saveSaveConfig(library, game.id, changedMapping);
    const auto incompatible = captureVerifiedSaveState(library, game.id, "different mapping");
    rejects([&] { reopened.put(key, 3, bundle(incompatible.id)); });
    require(reopened.current(key).revision == 3,
            "mapping mismatch cannot replace an accepted server state");
    require(QFile::remove(gameStorage + "/current.json"), "fixture removes committed pointer");
    rejects([&] { reopened.current(key); });
    rejects([&] { reopened.put(key, 0, bundle(first.id)); });
    require(QFile::remove(serverRoot + "/server.json"), "fixture removes server identity");
    rejects([&] { SaveServerStore lostIdentity(serverRoot); });
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try { revisionStore(); std::puts("PASS: server revision store"); return 0; }
    catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
