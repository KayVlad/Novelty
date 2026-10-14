#include "core.h"
#include "setup.h"
#include "profiles.h"
#include "recipes.h"
#include "saves.h"
#include "steam_shortcut.h"
#include "devices.h"
#include "pairing.h"
#include "peer_tls.h"
#include "sync_queue.h"
#include "lan_games.h"
#include "runtime_migration.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qset.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qthread.h>
#include <QtCore/qlockfile.h>
#include <QtCore/quuid.h>
#include <QtSql/qsqlquery.h>
#include <QtNetwork/qsslcertificate.h>
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <sys/stat.h>

using namespace novelty;
namespace {
int assertions = 0;
void require(bool condition, const char *what) { ++assertions; if (!condition) throw std::runtime_error(what); }
void rejects(const std::function<void()> &fn, const char *what) {
    bool threw = false; try { fn(); } catch (const std::exception &) { threw = true; } require(threw, what);
}
void touch(const QString &path, const QByteArray &data = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); require(f.open(QIODevice::WriteOnly), "fixture open"); f.write(data);
}
QByteArray contents(const QString &path);
void testLanGameManifest() {
    const QJsonObject descriptor{{"profile_game_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name", "Fixture"}, {"engine", "Ren'Py"}, {"executable", "bin/game.exe"},
        {"cwd", "bin"}, {"locale", ""}, {"game_identity", "fixture:game"},
        {"release_identity", "build-1"}, {"save_compatibility", "format-1"},
        {"mappings", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "saves"}}}},
        {"arguments", QJsonArray{}}, {"metadata", QJsonObject{}}};
    const QList<LanGameExchange::Entry> valid{{"bin", true, 0, {}},
        {"bin/game.exe", false, 4, QString(64, QLatin1Char('a'))}};
    validateLanGameManifest(descriptor, valid, 4);
    ++assertions;
    auto broken = valid;
    broken[1].path = "../outside.exe";
    rejects([&] { validateLanGameManifest(descriptor, broken, 4); }, "LAN manifest rejects path traversal");
    broken = valid; broken[1].path = "bin/game.novelty-part";
    rejects([&] { validateLanGameManifest(descriptor, broken, 4); }, "LAN manifest rejects internal partial-file names");
    broken = valid; broken.removeFirst();
    rejects([&] { validateLanGameManifest(descriptor, broken, 4); }, "LAN manifest requires declared parent directory");
    broken = valid; broken.append(valid.last());
    rejects([&] { validateLanGameManifest(descriptor, broken, 8); }, "LAN manifest rejects duplicate files");
    broken = valid; broken[1].sha256 = "invalid";
    rejects([&] { validateLanGameManifest(descriptor, broken, 4); }, "LAN manifest rejects unverified file");
    rejects([&] { validateLanGameManifest(descriptor, valid, 5); }, "LAN manifest rejects byte mismatch");
    broken = valid; broken.append({"saves", true, 0, {}});
    broken.append({"saves/slot.sav", false, 4, QString(64, QLatin1Char('a'))});
    rejects([&] { validateLanGameManifest(descriptor, broken, 8); }, "LAN game manifest cannot deliver mapped saves as installation files");
    auto unsafeDescriptor = descriptor;
    unsafeDescriptor["mappings"] = QJsonArray{QJsonObject{{"scope", "game"}, {"path", "../escape"}}};
    rejects([&] { validateLanGameManifest(unsafeDescriptor, valid, 4); }, "LAN install rejects unsafe save mapping before import");
    unsafeDescriptor = descriptor; unsafeDescriptor["executable"] = "missing.exe";
    rejects([&] { validateLanGameManifest(unsafeDescriptor, valid, 4); }, "LAN install requires declared executable");

    QTemporaryDir transferRoot;
    require(transferRoot.isValid(), "LAN transfer fixture root exists");
    touch(transferRoot.path() + "/Game/game.exe", "exe");
    touch(transferRoot.path() + "/Game/assets/background.bin", "asset");
    touch(transferRoot.path() + "/Game/UserData/slot.sav", "progress");
    Library transfer(transferRoot.path()); transfer.import({inspect(transferRoot.path() + "/Game")}, {});
    const auto transferGame = transfer.games().first();
    const auto paths = [](const auto &manifest) {
        QSet<QString> names;
        for (const auto &entry : manifest.first) names.insert(entry.path);
        return names;
    };
    require(paths(previewLanGameManifest(transfer, transferGame.id)).contains("UserData/slot.sav"),
            "unconfirmed folder names are not silently omitted from game transfer");
    QDir().mkpath(transferGame.folder + "/saves");
    require(unresolvedGameSaveLocations(transfer, transferGame.id).contains("UserData") &&
            unresolvedGameSaveLocations(transfer, transferGame.id).contains("saves"),
            "ambiguous game-local save folders require a source-side choice before LAN transfer");
    require(QDir(transferGame.folder + "/saves").removeRecursively(), "clear ambiguous transfer fixture");
    auto transferConfig = loadSaveConfig(transfer, transferGame.id);
    transferConfig.mappings = {{"game", "UserData"}};
    saveSaveConfig(transfer, transferGame.id, transferConfig);
    const auto protectedManifest = paths(previewLanGameManifest(transfer, transferGame.id));
    require(protectedManifest.contains("game.exe") && protectedManifest.contains("assets/background.bin") &&
            !protectedManifest.contains("UserData") && !protectedManifest.contains("UserData/slot.sav"),
            "confirmed game-local saves are excluded while game assets remain transferable");
    transferConfig = loadSaveConfig(transfer, transferGame.id);
    transferConfig.discoveryState = "searching"; // A supported older configuration can retain mappings while searching.
    saveSaveConfig(transfer, transferGame.id, transferConfig);
    require(!paths(previewLanGameManifest(transfer, transferGame.id)).contains("UserData/slot.sav"),
            "existing mappings remain protected regardless of discovery status");
    require(captureAvailableSaveSnapshot(transfer, transferGame.id, "before game transfer") &&
            listSaveSnapshots(transfer, transferGame.id).size() == 1,
            "peer game transfer captures existing progress before excluding its save folder");
    const auto transferHead = appliedSaveSnapshotId(transfer, transferGame.id);
    require(captureAvailableSaveSnapshot(transfer, transferGame.id, "before game transfer") &&
            appliedSaveSnapshotId(transfer, transferGame.id) == transferHead &&
            listSaveSnapshots(transfer, transferGame.id).size() == 1,
            "retrying a peer game transfer reuses unchanged verified progress");
    require(QFile::remove(transferGame.folder + "/UserData/slot.sav") &&
            !captureAvailableSaveSnapshot(transfer, transferGame.id, "before game transfer") &&
            appliedSaveSnapshotId(transfer, transferGame.id) == transferHead,
            "an empty save folder does not publish an empty transfer snapshot or erase history");
}
void testSetupRecipePrecedence() {
    const QList<SetupRule> rules{{{}, {}, "Ren'Py", "generic-proton-v1"},
                                 {"fixture:story", "build-1", {}, "renpy-bundled-v1"}};
    const auto exact = recommendSetupProfile("Ren'Py", "fixture:story", "build-1", rules);
    require(exact.profileId == "renpy-bundled-v1" && exact.source.contains("game"),
            "exact game and release recipe precedes engine recipe");
    const auto engine = recommendSetupProfile("Ren'Py", "fixture:story", "build-2", rules);
    require(engine.profileId == "generic-proton-v1" && engine.source.contains("engine"),
            "different release falls back to verified engine recipe");
    require(recommendSetupProfile("Generic").profileId == "generic-game-v1" &&
            recommendSetupProfile("Ren'Py").profileId == "generic-game-v1",
            "unverified engines do not select a compiled recipe");
    rejects([&] { recommendSetupProfile("Ren'Py", {}, {}, {{"fixture:story", {}, {}, "renpy-bundled-v1"}}); },
            "incomplete game recipe is rejected");
}
void testEditableRecipes(const QString &base) {
    const auto root = base + "/recipe-library";
    const auto game = root + "/Yosuga no Sora";
    touch(game + "/Folder/Story.exe", "exe");
    touch(game + "/Folder/renpy/__init__.py", "renpy");
    QDir().mkpath(game + "/Folder/game");
    Library library(root);
    const QStringList executables{game + "/Folder/Story.exe"};
    auto selected = selectRecipe(root, game, executables, "generic-vn-v1");
    require(!selected.ambiguous() && selected.selected().id == "renpy-bundled-v1",
            "nested Ren'Py structure matches an engine recipe");
    touch(game + "/Folder/game/saves/slot.sav", "progress");
    library.import({inspect(game, root)}, {});
    const auto nestedGame = library.games().first();
    const auto suggested = suggestedSaveMappings(library, nestedGame.id);
    require(std::any_of(suggested.begin(), suggested.end(), [](const SaveMapping &mapping) {
        return mapping.scope == "game" && mapping.path == "Folder/game/saves";
    }),
            "nested Ren'Py saves remain relative to the imported game folder");
    const auto specific = [](const QString &id, const QJsonArray &conditions) {
        return QJsonDocument(QJsonObject{{"schema", 1}, {"id", id}, {"name", "Story settings"},
            {"match", QJsonObject{{"all", conditions}}},
            {"settings", QJsonObject{{"executable", "Folder/Story.exe"}, {"runner", "proton-experimental"},
                {"saves", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "Folder/game/saves"}}}}}}}).toJson();
    };
    const QJsonArray exact{QJsonObject{{"file", "Folder/Story.exe"}}, QJsonObject{{"file_glob", "Folder/*.dat"}},
        QJsonObject{{"file", "marker.txt"}}};
    touch(game + "/Folder/assets.dat", "archive");
    saveRecipe(root, "games", specific("story", exact));
    selected = selectRecipe(root, game, executables, "generic-vn-v1");
    require(selected.selected().id == "renpy-bundled-v1", "every game recipe condition is required");
    touch(game + "/marker.txt");
    selected = selectRecipe(root, game, executables, "generic-vn-v1");
    require(!selected.ambiguous() && selected.selected().id == "story", "specific game recipe precedes engine");
    Game configured; configured.folder = game;
    applyRecipeToGame(configured, selected.selected());
    require(configured.executable == game + "/Folder/Story.exe" && configured.runnerSelector == "proton-experimental" &&
            configured.recipeSaves.size() == 1, "recipe settings use paths relative to the top-level game folder");
    saveRecipe(root, "games", specific("story-alternative", exact));
    selected = selectRecipe(root, game, executables, "generic-vn-v1");
    require(selected.ambiguous() && selected.matches.size() == 2, "equal-tier complete matches require a choice");
    QFile::remove(game + "/marker.txt");
    QFile::remove(game + "/Folder/renpy/__init__.py");
    selected = selectRecipe(root, game, executables, "generic-vn-v1");
    require(selected.selected().id == "generic-vn-v1", "selected default is the final fallback");
    rejects([&] { saveRecipe(root, "games", specific("bad", QJsonArray{QJsonObject{{"file", "../escape"}}})); },
            "recipe paths cannot escape the game folder");
}
void testLegacyRuntimeMigration(const QString &base) {
    const auto root = base + "/legacy-runtime-library";
    const auto source = base + "/legacy-umu";
    require(QDir().mkpath(root) && QDir().mkpath(source + "/steamrt3/bin"), "runtime migration fixture folders");
    touch(source + "/steamrt3/.installed.ok", "complete");
    touch(source + "/steamrt3/toolmanifest.vdf", "manifest");
    touch(source + "/steamrt3/bin/launch", "runtime payload");
    Library lib(root);
    {
        const auto name = "runtime-migration-fixture";
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(lib.dataRoot() + "/library.sqlite");
        require(db.open(), "legacy runtime fixture database opens");
        {
            QSqlQuery query(db);
            require(query.exec("DELETE FROM settings WHERE key='runtime_storage'"), "fixture switches to legacy runtime mode");
        }
        db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name);
    }
    require(!lib.managedRuntimeStorage(), "fixture library starts in legacy runtime mode");
    const auto destination = lib.dataRoot() + "/runtime";
    require(QDir().mkpath(destination), "create an unverified managed runtime collision");
    rejects([&] { migrateLegacyUmuRuntime(lib, source); }, "migration refuses unknown existing runtime");
    require(!lib.managedRuntimeStorage(), "collision leaves legacy mode unchanged");
    require(QDir(destination).removeRecursively(), "remove fixture collision");
    std::stop_source stopped; stopped.request_stop();
    rejects([&] { migrateLegacyUmuRuntime(lib, source, stopped.get_token()); }, "cancelled migration refuses cutover");
    require(!lib.managedRuntimeStorage() && !QFileInfo(destination).exists(), "cancel preserves legacy mode and source");
    migrateLegacyUmuRuntime(lib, source);
    require(lib.managedRuntimeStorage() && contents(destination + "/umu/steamrt3/bin/launch") == "runtime payload" &&
            contents(source + "/steamrt3/bin/launch") == "runtime payload", "verified migration retains original runtime");
}
QByteArray contents(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "fixture read"); return file.readAll();
}
void testDeviceIdentity(const QString &base) {
    const auto original = base + "/Device identity A", moved = base + "/Device identity moved";
    const auto second = base + "/Device identity B";
    require(QDir().mkpath(original) && QDir().mkpath(second), "device libraries created");
    QString profile, fingerprint;
    {
        Library lib(original);
        profile = lib.profileId();
        const auto first = ensureDeviceIdentity(lib);
        fingerprint = first.fingerprint;
        require(fingerprint.size() == 64 && first.certificatePath.startsWith(lib.dataRoot()) &&
                first.privateKeyPath.startsWith(lib.dataRoot()), "device identity stays within the profile library");
        const auto qtCertificates = QSslCertificate::fromData(contents(first.certificatePath));
        require(qtCertificates.size() == 1 && !qtCertificates.first().isNull() &&
                QString::fromLatin1(qtCertificates.first().digest(QCryptographicHash::Sha256).toHex()) == fingerprint,
                "Qt TLS loads the device certificate and agrees on its fingerprint");
        QSslSocket socket;
        configureDeviceTls(socket, first);
        require(socket.peerVerifyMode() == QSslSocket::VerifyPeer &&
                socket.protocol() == QSsl::TlsV1_2OrLater &&
                peerCertificateFingerprint(socket.localCertificate()) == fingerprint,
                "local TLS socket loads the P-256 identity and requires peer verification");
        const auto certificate = qtCertificates.first();
        const auto verificationErrors = QSslCertificate::verify({certificate}, "novelty.local");
        require(verificationErrors.size() == 1 &&
                verificationErrors.first().error() == QSslError::SelfSignedCertificate,
                "generated leaf has only the expected TLS self-signed error");
        const QList<QSslError> selfSigned{QSslError(QSslError::SelfSignedCertificate, certificate)};
        require(expectedPeerTlsErrors(certificate, selfSigned) &&
                expectedPeerTlsErrors(certificate, selfSigned, fingerprint),
                "only the presented self-signed certificate is an expected pairing error");
        require(!expectedPeerTlsErrors(certificate, selfSigned, QString(64, QLatin1Char('0'))) &&
                !expectedPeerTlsErrors(certificate, {QSslError(QSslError::HostNameMismatch, certificate)}) &&
                !expectedPeerTlsErrors(QSslCertificate(), selfSigned),
                "wrong pin, name mismatch and missing certificate fail closed");
        rejects([&] { authorizedPeerFingerprint(lib, socket); },
                "unencrypted connection never authorizes application data");
        struct stat keyStat {};
        require(::stat(first.privateKeyPath.toLocal8Bit().constData(), &keyStat) == 0 &&
                (keyStat.st_mode & 0777) == 0600, "private key is owner-only");
        require(ensureDeviceIdentity(lib).fingerprint == fingerprint, "device identity is stable on repeat use");
    }
    require(QDir().rename(original, moved), "device library relocates");
    {
        Library lib(moved);
        require(lib.profileId() == profile && ensureDeviceIdentity(lib).fingerprint == fingerprint,
                "profile and device identity survive library relocation");
        Library other(second);
        const auto otherFingerprint = ensureDeviceIdentity(other).fingerprint;
        require(other.profileId() != profile && otherFingerprint != fingerprint,
                "separate libraries have separate device identities");
        require(trustedPeers(lib).isEmpty() && trustedPeers(other).isEmpty(), "trust starts empty in each profile");
        rejects([&] { approvePairedPeer(lib, fingerprint, "Self"); }, "cannot trust the local identity as a peer");
        rejects([&] { approvePairedPeer(lib, "not a fingerprint", "Other"); }, "malformed peer fingerprint rejected");
        rejects([&] { approvePairedPeer(lib, otherFingerprint, "Bad\nName"); }, "control characters rejected in peer labels");
        approvePairedPeer(lib, otherFingerprint, "Desktop");
        require(isTrustedPeer(lib, otherFingerprint) && !isTrustedPeer(other, fingerprint) &&
                trustedPeers(lib).first().name == "Desktop", "approval is pinned to only one profile");
        rememberPeerEndpoint(lib, otherFingerprint, "192.0.2.42", 38373);
        require(trustedPeers(lib).first().address == "192.0.2.42" &&
                trustedPeers(lib).first().port == 38373,
                "last LAN endpoint persists without changing the peer fingerprint");
        rejects([&] { rememberPeerEndpoint(other, fingerprint, "192.0.2.42", 38373); },
                "unpaired profile cannot remember a peer endpoint");
        rejects([&] { rememberPeerEndpoint(lib, otherFingerprint, "not an IP", 38373); },
                "invalid peer endpoint rejected");
        const auto approvedAt = trustedPeers(lib).first().approvedAt;
        approvePairedPeer(lib, otherFingerprint, "Renamed desktop");
        require(trustedPeers(lib).size() == 1 && trustedPeers(lib).first().approvedAt == approvedAt,
                "repeat approval updates the label without duplicating or resetting trust");
        const auto trustPath = lib.dataRoot() + "/devices/" + lib.profileId() + "/peers.json";
        struct stat peerStat {};
        require(::stat(trustPath.toLocal8Bit().constData(), &peerStat) == 0 &&
                (peerStat.st_mode & 0777) == 0600, "trusted-peer list is owner-only");
        revokePeer(lib, otherFingerprint);
        require(!isTrustedPeer(lib, otherFingerprint) && trustedPeers(lib).isEmpty(), "revocation removes access immediately");
        touch(trustPath, "broken trust file");
        rejects([&] { isTrustedPeer(lib, otherFingerprint); }, "damaged trust file fails closed");
        require(contents(trustPath) == "broken trust file", "damaged trust file is not silently replaced");
        const auto identity = ensureDeviceIdentity(lib);
        require(QFile::remove(identity.privateKeyPath), "fixture removes private key");
        rejects([&] { ensureDeviceIdentity(lib); }, "missing private key fails closed");
        require(QFileInfo::exists(identity.certificatePath) && !QFileInfo::exists(identity.privateKeyPath),
                "incomplete identity is not silently regenerated");
        touch(identity.privateKeyPath, "invalid key");
        QFile::setPermissions(identity.privateKeyPath, QFile::ReadOwner | QFile::WriteOwner);
        rejects([&] { ensureDeviceIdentity(lib); }, "corrupt private key fails closed");
        QFile::setPermissions(identity.privateKeyPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);
        rejects([&] { ensureDeviceIdentity(lib); }, "exposed private key fails closed");
    }
}
void testMovedLibrary(const QString &base) {
    const auto oldRoot = base + "/Moved library old";
    const auto newRoot = base + "/Moved library new";
    touch(oldRoot + "/Story/play.exe", "game");
    QString id, profile;
    {
        Library library(oldRoot);
        library.import(discover(oldRoot), {});
        auto game = library.games().first();
        id = game.id; profile = library.profileId();
        game.name = "Edited title";
        game.runner = oldRoot + "/.novelty/runners/GE-Test";
        touch(game.runner + "/proton", "runner");
        library.save(game);
        library.setSettings({{"runner", game.runner}, {"umu", "umu-run"}});
        touch(library.prefix(id) + "/save.dat", "retained progress");
    }
    require(QDir().rename(oldRoot, newRoot), "move populated Games library");
    {
        Library library(newRoot);
        const auto games = library.games();
        require(games.size() == 1 && games.first().id == id && games.first().name == "Edited title",
                "moved library retains its original game and installation identity");
        require(games.first().folder == newRoot + "/Story" &&
                games.first().executable == newRoot + "/Story/play.exe" &&
                games.first().workingDirectory == newRoot + "/Story" &&
                games.first().runner == newRoot + "/.novelty/runners/GE-Test" &&
                library.settings()["runner"] == games.first().runner &&
                library.settings()["umu"] == "umu-run",
                "moved library rebases managed paths while retaining command names");
        require(contents(library.prefix(id) + "/save.dat") == "retained progress" &&
                library.profileId() == profile && library.import(discover(newRoot), {}) == 0,
                "moved library retains saves/profile and does not discover duplicate games");
    }
    Library reopened(newRoot);
    require(reopened.games().size() == 1 && reopened.game(id).folder == newRoot + "/Story",
            "moved library remains stable after reopening");
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "relative-path-fixture");
        db.setDatabaseName(newRoot + "/.novelty/library.sqlite");
        require(db.open(), "inspect moved library storage");
        QSqlQuery query(db);
        require(query.exec("SELECT folder,config FROM games") && query.next(), "inspect stored game paths");
        const auto data = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
        require(query.value(0).toString() == "Story" && data["folder"] == "Story" &&
                data["executable"] == "Story/play.exe" &&
                data["runner"] == ".novelty/runners/GE-Test",
                "database stores library-owned game paths relative to Games");
        query.finish();
        require(query.exec("SELECT value FROM settings WHERE key='app'") && query.next(),
                "inspect stored library defaults");
        const auto defaults = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
        require(defaults["runner"] == ".novelty/runners/GE-Test" && defaults["umu"] == "umu-run",
                "database stores managed defaults relatively and keeps command names intact");
        query.finish();
        db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase("relative-path-fixture");
    }

    const auto legacyRoot = base + "/Legacy move old";
    const auto recoveredRoot = base + "/Legacy move new";
    touch(legacyRoot + "/Classic/game.exe");
    Game original;
    {
        Library library(legacyRoot);
        library.import(discover(legacyRoot), {});
        original = library.games().first();
    }
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-move-fixture");
        db.setDatabaseName(legacyRoot + "/.novelty/library.sqlite");
        require(db.open(), "open legacy moved library fixture");
        QSqlQuery query(db);
        require(query.exec("DELETE FROM settings WHERE key='path_format'"), "restore legacy absolute-path format");
        query.prepare("UPDATE games SET folder=?, config=? WHERE id=?");
        query.addBindValue(original.folder);
        query.addBindValue(QString::fromUtf8(QJsonDocument(original.json()).toJson(QJsonDocument::Compact)));
        query.addBindValue(original.id);
        require(query.exec(), "restore legacy absolute game record");
        db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase("legacy-move-fixture");
    }
    require(QDir().rename(legacyRoot, recoveredRoot), "move legacy Games folder");
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "duplicate-move-fixture");
        db.setDatabaseName(recoveredRoot + "/.novelty/library.sqlite");
        require(db.open(), "open moved legacy library before repair");
        Game duplicate = original;
        duplicate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        duplicate.folder = recoveredRoot + "/Classic";
        duplicate.executable = duplicate.folder + "/game.exe";
        duplicate.workingDirectory = duplicate.folder;
        duplicate.source = "discovered";
        QSqlQuery query(db);
        query.prepare("INSERT INTO games(id,folder,config) VALUES(?,?,?)");
        query.addBindValue(duplicate.id); query.addBindValue(duplicate.folder);
        query.addBindValue(QString::fromUtf8(QJsonDocument(duplicate.json()).toJson(QJsonDocument::Compact)));
        require(query.exec(), "simulate duplicate discovered after library move");
        db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase("duplicate-move-fixture");
    }
    Library recovered(recoveredRoot);
    require(recovered.games().size() == 1 && recovered.game(original.id).folder == recoveredRoot + "/Classic" &&
            recovered.import(discover(recoveredRoot), {}) == 0,
            "legacy copied library rebases its original record and removes an unused duplicate");
}
void testNestedGameDiscovery(const QString &base) {
    const auto root = base + "/Nested game discovery";
    const auto title = root + "/Title";
    const auto executable = title + "/Game/Release/bin/Play.exe";
    touch(executable, "game");
    touch(title + "/OST/player.exe", "soundtrack utility");
    touch(title + "/OST/track.flac", "music");
    touch(title + "/Game/renpy/__init__.py");
    touch(title + "/Game/game/options.rpy");
    touch(root + "/.novelty/tools/Managed.exe", "managed tool");
    touch(title + "/.novelty/tools/Hidden.exe", "managed tool");
    const auto found = discover(root);
    require(found.size() == 1 && found.first().folder == title && found.first().name == "Title" &&
            found.first().executables == QStringList{executable} && found.first().engine == "Ren'Py",
            "nested Game executable and engine belong to top-level Title; OST utility is secondary");
    const auto scanned = scanDiscovery(root, {});
    require(scanned.candidates.size() == 1 && scanned.candidates.first().folder == title &&
            !scanned.folderIdentities.contains(root + "/.novelty") &&
            !scanned.watchDirectories.contains(root + "/.novelty") &&
            !scanned.watchDirectories.contains(title + "/.novelty"),
            "background discovery treats a deep nested executable as one Title game and skips .novelty");
    Library library(root);
    require(library.import(found, {}) == 1, "nested game imports once");
    const auto game = library.games().first();
    require(game.folder == title && game.name == "Title" && game.executable == executable &&
            game.workingDirectory == QFileInfo(executable).absolutePath() && game.engine == "Ren'Py" &&
            library.import(discover(root), {}) == 0,
            "nested game keeps the top-level folder and correct launch directory without duplicates");
}
void testNativeGameDiscovery(const QString &base) {
    const auto root = base + "/Native game discovery";
    const auto title = root + "/Yosuga no Sora";
    const auto executable = title + "/ヨスガノソラRE/yosuga.x86_64";
    const auto marker = title + "/launched.txt";
    touch(executable, "#!/bin/sh\nprintf '%s' \"$PWD\" > ../launched.txt\n");
    require(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "native fixture is executable");
    touch(title + "/OST/player.exe", "soundtrack utility");
    touch(root + "/.novelty/tools/Managed.x86_64", "managed tool");
    const auto found = discover(root);
    require(found.size() == 1 && found.first().name == "Yosuga no Sora" && found.first().folder == title &&
            found.first().native && found.first().executables == QStringList{executable},
            "nested Linux launcher identifies its top-level game ahead of an OST utility");
    const auto scanned = scanDiscovery(root, {});
    require(scanned.candidates.size() == 1 && scanned.candidates.first().native &&
            scanned.candidates.first().folder == title && !scanned.folderIdentities.contains(root + "/.novelty"),
            "background discovery finds the native game and excludes Novelty state");
    Library library(root);
    require(library.import(found, {}) == 1 && library.import(found, {}) == 0,
            "native game imports once");
    const auto game = library.games().first();
    require(game.backend == "native" && game.executable == executable &&
            game.workingDirectory == QFileInfo(executable).absolutePath() && library.status(game) == "Ready",
            "native game keeps its executable, working directory, and ready status");
    const auto command = launchCommand(library, game);
    require(command.program == executable && command.arguments.isEmpty() &&
            command.cwd == QFileInfo(executable).absolutePath(),
            "native launcher runs directly without Proton");
    require(supervise(library, game.id, false) == 0, "native Play completes without Proton setup");
    QFile launched(marker);
    require(launched.open(QIODevice::ReadOnly) && launched.readAll() == command.cwd.toUtf8(),
            "native Play uses the launcher's directory");
    const auto source = base + "/Native launcher without extension";
    const auto chosen = source + "/start";
    touch(chosen, "#!/bin/sh\nexit 0\n");
    require(QFile::setPermissions(chosen, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "extensionless Linux fixture is executable");
    touch(source + "/play.exe", "Windows version");
    touch(source + "/lib/libhelper.so", "\177ELF");
    require(nativeExecutable(chosen) && !nativeExecutable(source + "/lib/libhelper.so") &&
            !nativeExecutable(source + "/play.exe"),
            "native selection recognizes scripts and excludes non-executable files");
    rejects([&] { library.addFolder(source, source + "/play.exe"); },
            "explicit native option rejects a Windows executable");
    const auto added = library.addFolder(source, chosen).game;
    require(added.backend == "native" && added.executable == root + "/Native launcher without extension/start" &&
            added.workingDirectory == added.folder && nativeExecutable(added.executable),
            "explicit native option moves and selects an extensionless Linux launcher");
    require(launchCommand(library, added).program == added.executable,
            "explicit native choice runs directly even beside a Windows executable");
}
void testPairingProtocol() {
    const auto sessionId = newDiscoverySessionId();
    const auto announcement = encodeDiscoveryAnnouncement({sessionId, 51472});
    DiscoveryAnnouncement discovered;
    require(announcement.size() == 22 && decodeDiscoveryAnnouncement(announcement, discovered) &&
            discovered.sessionId == sessionId && discovered.port == 51472,
            "discovery announces only a random session and listener port");
    require(!decodeDiscoveryAnnouncement(announcement.left(21), discovered) &&
            !decodeDiscoveryAnnouncement(announcement + 'x', discovered),
            "discovery rejects truncated and extended datagrams");
    auto invalidAnnouncement = announcement; invalidAnnouncement[3] = '2';
    require(!decodeDiscoveryAnnouncement(invalidAnnouncement, discovered), "discovery rejects unknown version");
    rejects([&] { encodeDiscoveryAnnouncement({sessionId, 0}); }, "zero listener port rejected");
    rejects([&] { encodeDiscoveryAnnouncement({sessionId.left(8), 51472}); }, "short session ID rejected");
    const QString first(64, QLatin1Char('a')), second(64, QLatin1Char('b'));
    const auto firstNonce = newPairingNonce(), secondNonce = newPairingNonce();
    require(firstNonce.size() == 32 && secondNonce.size() == 32 && firstNonce != secondNonce,
            "pairing uses distinct 256-bit random nonces");
    const auto phrase = pairingPhrase(first, firstNonce, second, secondNonce);
    require(phrase == pairingPhrase(second, secondNonce, first, firstNonce) &&
            phrase.size() == 24 && phrase.count(QLatin1Char(' ')) == 4,
            "both peers derive the same grouped verification phrase");
    auto changed = secondNonce; changed[0] = char(changed[0] ^ 1);
    require(phrase != pairingPhrase(first, firstNonce, second, changed),
            "verification phrase binds both fresh nonces");
    require(phrase != pairingPhrase(first, firstNonce, QString(64, QLatin1Char('c')), secondNonce),
            "verification phrase binds both certificate fingerprints");
    rejects([&] { pairingPhrase(first, firstNonce, first, secondNonce); }, "self-pairing rejected");
    rejects([&] { pairingPhrase(first.toUpper(), firstNonce, second, secondNonce); },
            "noncanonical fingerprint rejected");
    rejects([&] { pairingPhrase(first, firstNonce.left(16), second, secondNonce); },
            "short nonce rejected");

    QByteArray buffer, payload("untouched");
    const auto firstFrame = encodeControlFrame("hello"), secondFrame = encodeControlFrame("world");
    buffer = firstFrame.left(4);
    require(!takeControlFrame(buffer, payload) && payload == "untouched", "partial frame header waits");
    buffer += firstFrame.mid(4, 5);
    require(!takeControlFrame(buffer, payload), "partial frame payload waits");
    buffer += firstFrame.mid(9) + secondFrame;
    require(takeControlFrame(buffer, payload) && payload == "hello" &&
            takeControlFrame(buffer, payload) && payload == "world" && buffer.isEmpty(),
            "framing preserves concatenated messages");
    rejects([&] { encodeControlFrame(QByteArray(maxControlPayload + 1, 'x')); },
            "oversized outgoing frame rejected");
    buffer = QByteArray::fromHex("4e560100010001");
    rejects([&] { takeControlFrame(buffer, payload); }, "oversized incoming frame rejected before payload");
    buffer = firstFrame; buffer[2] = 2;
    rejects([&] { takeControlFrame(buffer, payload); }, "unknown frame version rejected");
    buffer = QByteArray(2 * (7 + maxControlPayload) + 1, 'x');
    rejects([&] { takeControlFrame(buffer, payload); }, "excess buffered input rejected");
}
void testSyncQueue(const QString &base) {
    const auto root = base + "/Pending sync", peerRoot = base + "/Pending sync peer";
    touch(root + "/Game/game.exe"); touch(peerRoot + "/Peer/game.exe");
    Library lib(root), peer(peerRoot);
    lib.import({inspect(root + "/Game")}, {});
    const auto game = lib.games().first();
    auto config = loadSaveConfig(lib, game.id);
    config.mappings = {{"game", "saves"}};
    saveSaveConfig(lib, game.id, config);
    touch(game.folder + "/saves/slot.sav", "first");
    const auto first = createSaveSnapshot(lib, game.id);
    const auto fingerprint = ensureDeviceIdentity(peer).fingerprint;
    approvePairedPeer(lib, fingerprint, "Peer");
    queuePeerSync(lib, game.id, first.id);
    queuePeerSync(lib, game.id, first.id);
    require(pendingPeerSync(lib).size() == 1 && pendingPeerSync(lib).first().snapshotId == first.id,
            "repeat pending save work stays durable and idempotent");
    touch(game.folder + "/saves/slot.sav", "second");
    const auto second = createSaveSnapshot(lib, game.id);
    require(pendingPeerSync(lib).size() == 1 && pendingPeerSync(lib).first().snapshotId == first.id,
            "new local captures do not schedule automatic peer transfers");
    queuePeerSync(lib, game.id, second.id);
    acknowledgePeerSync(lib, fingerprint, game.id, first.id);
    require(pendingPeerSync(lib).size() == 1 && pendingPeerSync(lib).first().snapshotId == second.id,
            "late acknowledgement cannot clear newer pending progress");
    acknowledgePeerSync(lib, fingerprint, game.id, second.id);
    require(pendingPeerSync(lib).isEmpty(), "matching acknowledgement clears pending progress");
    rejects([&] { queuePeerSync(lib, game.id, QUuid::createUuid().toString(QUuid::WithoutBraces)); },
            "missing snapshot cannot be queued for a peer");
    touch(lib.dataRoot() + "/sync/queue.json", "damaged");
    rejects([&] { pendingPeerSync(lib); }, "damaged pending peer work fails closed");
}
void testSaveSnapshots(const QString &base) {
    const auto root = base + "/Save snapshots";
    touch(root + "/Game/game.exe");
    Library lib(root); lib.import({inspect(root + "/Game")}, {});
    const auto game = lib.games().first();
    const auto gameSave = game.folder + "/game/saves/slot1.dat";
    const auto prefixSave = lib.prefix(game.id) + "/drive_c/users/steamuser/AppData/Roaming/NoveltyTest/progress.dat";
    touch(gameSave, "first game save"); touch(prefixSave, "first prefix save");
    SaveConfig config; config.gameIdentity = "vndb:v123"; config.releaseIdentity = "test-build-1";
    config.saveCompatibility = "test-format-1";
    config.mappings = {{"game", "game/saves"}, {"prefix", "drive_c/users/steamuser/AppData/Roaming/NoveltyTest/progress.dat"}};
    saveSaveConfig(lib, game.id, config);
    require(loadSaveConfig(lib, game.id).mappings.size() == 2 &&
            loadSaveConfig(lib, game.id).saveCompatibility == "test-format-1", "save mapping and identity persist");
    const auto first = createSaveSnapshot(lib, game.id);
    require(first.files == 2 && listSaveSnapshots(lib, game.id).size() == 1, "two mapped locations snapshot together");
    const auto firstManifest = readJson(QFileInfo(lib.gamePath(game.id, "saves.json")).absolutePath() +
                                        "/snapshots/" + first.id + "/manifest.json");
    require(firstManifest["release_identity"] == "test-build-1" && firstManifest["profile_id"] == lib.profileId(),
            "snapshot records the edition and local profile provenance");
    touch(gameSave, "newer game save"); touch(prefixSave, "newer prefix save");
    restoreSaveSnapshot(lib, game.id, first.id);
    require(contents(gameSave) == "first game save" && contents(prefixSave) == "first prefix save" &&
            listSaveSnapshots(lib, game.id).size() == 2, "restore verifies and retains replaced progress as a snapshot");
    auto overlapping = config; overlapping.mappings.append(SaveMapping{"game", "game/saves/slot1.dat"});
    rejects([&] { saveSaveConfig(lib, game.id, overlapping); }, "overlapping save locations rejected");
    overlapping = config; overlapping.mappings[0].path = "../outside";
    rejects([&] { saveSaveConfig(lib, game.id, overlapping); }, "escaping save mapping rejected");
    const auto link = game.folder + "/game/link";
    require(QFile::link(base, link), "fixture symlink");
    overlapping = config; overlapping.mappings[0].path = "game/link/other";
    rejects([&] { saveSaveConfig(lib, game.id, overlapping); }, "symlinked save path rejected");
    QLockFile running(lib.gamePath(game.id, "launch.lock")); running.setStaleLockTime(0);
    require(running.tryLock(), "fixture game lock");
    rejects([&] { createSaveSnapshot(lib, game.id); }, "running game cannot be snapshotted"); running.unlock();
    touch(gameSave, "unsaved newer progress");
    const auto beforeRecovery = createSaveSnapshot(lib, game.id);
    const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto held = game.folder + "/game/saves.novelty-held-" + nonce;
    require(QDir().rename(game.folder + "/game/saves", held), "fixture interrupted restore hold");
    QDir().mkpath(game.folder + "/game/saves"); touch(gameSave, "first game save");
    QJsonArray items;
    for (int i = 0; i < config.mappings.size(); ++i)
        items.append(QJsonObject{{"scope", config.mappings[i].scope}, {"path", config.mappings[i].path},
                                 {"index", i}, {"original_present", true}});
    writeJson(lib.gamePath(game.id, "save-restore.json"), {{"schema", 1}, {"nonce", nonce},
        {"snapshot", first.id}, {"backup", beforeRecovery.id}, {"items", items}});
    require(!lib.recoveryIssue(game.id).isEmpty(), "pending restore blocks Play");
    require(recoverSaveRestores(lib).size() == 1 && contents(gameSave) == "unsaved newer progress" &&
            !QFileInfo::exists(lib.gamePath(game.id, "save-restore.json")), "interrupted restore rolls back to original saves");
    const auto runner = base + "/Save runner", fake = base + "/save-umu";
    touch(runner + "/proton");
    touch(fake, "#!/bin/sh\nif [ \"$2\" = \"/c\" ]; then mkdir -p \"$WINEPREFIX\"; touch \"$WINEPREFIX/system.reg\"; exit 0; fi\nprintf 'played game save' > \"$(dirname \"$1\")/game/saves/slot1.dat\"\nprintf 'played prefix save' > \"$WINEPREFIX/drive_c/users/steamuser/AppData/Roaming/NoveltyTest/progress.dat\"\n");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto configured = lib.game(game.id); configured.runner = runner; lib.save(configured); lib.setSettings({{"umu", fake}});
    require(supervise(lib, game.id, false) == 0 && contents(gameSave) == "played game save" &&
            listSaveSnapshots(lib, game.id).size() == 4 &&
            !QFileInfo::exists(lib.gamePath(game.id, "save-pending.json")), "ordinary Play snapshots mapped saves after exit");
    configured = lib.game(game.id); queuePostExitSaveSnapshot(lib, configured);
    touch(gameSave, "progress after interrupted supervisor");
    require(lib.recoveryIssue(game.id).isEmpty() && !pendingSaveSnapshotIssue(lib, game.id).isEmpty(),
            "pending post-exit work remains visible without blocking offline Play");
    require(recoverPendingSaveSnapshots(lib).size() == 1 &&
            !QFileInfo::exists(lib.gamePath(game.id, "save-pending.json")) &&
            listSaveSnapshots(lib, game.id).size() == 5, "pending post-exit work survives and completes on recovery");
    auto optional = config; optional.mappings.append(SaveMapping{"game", "game/optional.dat"});
    saveSaveConfig(lib, game.id, optional);
    const auto withoutOptional = createSaveSnapshot(lib, game.id);
    touch(game.folder + "/game/optional.dat", "later optional progress");
    restoreSaveSnapshot(lib, game.id, withoutOptional.id);
    require(!QFileInfo::exists(game.folder + "/game/optional.dat"), "restore handles a save path absent in the snapshot");
    const auto savedNow = createSaveSnapshot(lib, game.id);
    const auto partialNonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto partial = game.folder + "/game/saves.novelty-incoming-" + partialNonce;
    touch(partial + "/partial.dat", "interrupted staged copy");
    QJsonArray partialItems;
    for (int i = 0; i < optional.mappings.size(); ++i)
        partialItems.append(QJsonObject{{"scope", optional.mappings[i].scope}, {"path", optional.mappings[i].path},
                                      {"index", i}, {"original_present", i < 2}});
    writeJson(lib.gamePath(game.id, "save-restore.json"), {{"schema", 1}, {"nonce", partialNonce},
        {"snapshot", savedNow.id}, {"backup", savedNow.id}, {"items", partialItems}});
    const auto recoveredPartial = recoverSaveRestores(lib);
    if (recoveredPartial.size() != 1 || QFileInfo::exists(partial) || contents(gameSave) != "progress after interrupted supervisor")
        throw std::runtime_error(("interrupted staged copy is removed without changing original saves: " + recoveredPartial.join(" | ")).toStdString());
    touch(game.folder + "/renpy/__init__.py");
    configured = lib.game(game.id); configured.saveStrategy = "renpy"; lib.save(configured);
    const auto suggestions = suggestedSaveMappings(lib, game.id);
    require(!suggestions.isEmpty() && suggestions.first().scope == "game" && suggestions.first().path == "game/saves",
            "populated Ren'Py save folder can be suggested for confirmation");
}
void testSaveDiscovery(const QString &base) {
    const auto root = base + "/Save discovery";
    touch(root + "/Game/game.exe");
    Library lib(root); lib.import({inspect(root + "/Game")}, {});
    const auto game = lib.games().first();
    const auto simplePath = saveMappingFromPath(lib, game, "UserData");
    require(simplePath.scope == "game" && simplePath.path == "UserData" &&
            saveMappingFromPath(lib, game, "/Game/UserData").path == "UserData" &&
            saveMappingFromPath(lib, game, game.folder + "/UserData").path == "UserData",
            "one path field accepts game-relative, displayed and browsed game locations");
    const auto roaming = saveMappingFromPath(lib, game,
        saveMappingRoot(lib, game, "appdata") + "/Vendor/Save");
    const auto local = saveMappingFromPath(lib, game,
        lib.prefix(game.id) + "/drive_c/users/steamuser/AppData/Local/Vendor/Save");
    require(roaming.scope == "appdata" && roaming.path == "Vendor/Save" &&
            local.scope == "prefix" && local.path.endsWith("AppData/Local/Vendor/Save") &&
            saveMappingFromPath(lib, game, "AppData:/Vendor/Save").scope == "appdata",
            "a browsed prefix path retains its portable save scope without a dropdown");
    rejects([&] { saveMappingFromPath(lib, game, "/tmp/outside-save"); },
            "manual save path cannot point outside the game or its prefix");
    rejects([&] { saveMappingFromPath(lib, game, "/Game/../outside-save"); },
            "displayed game path cannot escape the game folder");
    beginSaveObservation(lib, game);
    require(loadSaveConfig(lib, game.id).discoveryState == "searching" &&
            !QFileInfo::exists(lib.gamePath(game.id, "save-observation.json")),
            "no save folder leaves discovery ready for the next Play without an inventory");
    QDir().mkpath(game.folder + "/UserData");
    finishSaveObservation(lib, game);
    auto found = loadSaveConfig(lib, game.id);
    require(found.discoveryState == "identified" && found.discoverySource == "heuristic" &&
            found.mappings.size() == 1 && found.mappings.first().path == "UserData",
            "empty conventional UserData folder is detected after Play");
    queuePostExitSaveSnapshot(lib, game);
    finishPostExitSaveSnapshot(lib, game);
    require(listSaveSnapshots(lib, game.id).size() == 1 && listSaveSnapshots(lib, game.id).first().files == 0,
            "post-Play capture records a verified empty save state");
    touch(game.folder + "/UserData/slot1.sav", "first launch progress");
    const auto first = createSaveSnapshot(lib, game.id);
    require(createSaveSnapshot(lib, game.id).id == first.id && listSaveSnapshots(lib, game.id).size() == 2,
            "unchanged save creates no duplicate snapshot");
    auto otherEdition = loadSaveConfig(lib, game.id); otherEdition.releaseIdentity = "different-edition";
    saveSaveConfig(lib, game.id, otherEdition);
    require(createSaveSnapshot(lib, game.id).id != first.id,
            "identical bytes under a different edition do not reuse an incompatible snapshot");
    otherEdition.releaseIdentity.clear(); saveSaveConfig(lib, game.id, otherEdition);
    require(QDir(game.folder + "/UserData").removeRecursively(), "clear saves for fresh-install restore fixture");
    const auto count = listSaveSnapshots(lib, game.id).size();
    restoreSaveSnapshot(lib, game.id, first.id);
    require(contents(game.folder + "/UserData/slot1.sav") == "first launch progress" &&
            listSaveSnapshots(lib, game.id).size() == count,
            "fresh empty save root restores without publishing an empty backup");
    setSaveDiscoveryState(lib, game.id, "searching");
    require(loadSaveConfig(lib, game.id).discoveryState == "searching", "Rescan persists searching state");
    setSaveDiscoveryState(lib, game.id, "unidentified");
    require(loadSaveConfig(lib, game.id).discoveryState == "unidentified", "explicit stop persists unidentified state");
    beginSaveObservation(lib, game);
    require(loadSaveConfig(lib, game.id).discoveryState == "identified" &&
            loadSaveConfig(lib, game.id).mappings.first().path == "UserData",
            "legacy stopped discovery resumes the bounded detector");

    const auto recipeRoot = base + "/Save recipe";
    touch(recipeRoot + "/Game/game.exe");
    Library recipes(recipeRoot); recipes.import({inspect(recipeRoot + "/Game")}, {});
    const auto recipeGame = recipes.games().first();
    SaveConfig config; config.gameIdentity = "vndb:v123"; config.releaseIdentity = "edition-a";
    saveSaveConfig(recipes, recipeGame.id, config);
    auto recipe = saveRecipePath(recipes, loadSaveConfig(recipes, recipeGame.id));
    writeJson(recipe, {{"schema", 1}, {"game_identity", "vndb:v123"}, {"edition_identity", "edition-b"},
                       {"save_roots", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "saves"}}}}});
    rejects([&] { beginSaveObservation(recipes, recipeGame); }, "wrong-edition save recipe rejected");
    require(loadSaveConfig(recipes, recipeGame.id).discoveryState == "searching", "wrong recipe does not identify saves");
    writeJson(recipe, {{"schema", 1}, {"game_identity", "vndb:v123"}, {"edition_identity", "edition-a"},
                       {"save_roots", QJsonArray{QJsonObject{{"scope", "game"}, {"path", "saves"}}}}});
    beginSaveObservation(recipes, recipeGame);
    require(loadSaveConfig(recipes, recipeGame.id).discoveryState == "identified" &&
            loadSaveConfig(recipes, recipeGame.id).discoverySource == "recipe" &&
            listSaveSnapshots(recipes, recipeGame.id).isEmpty(), "exact recipe identifies empty known root without a snapshot");
    queuePostExitSaveSnapshot(recipes, recipeGame);
    finishPostExitSaveSnapshot(recipes, recipeGame);
    require(!QFileInfo::exists(recipes.gamePath(recipeGame.id, "save-pending.json")) &&
            listSaveSnapshots(recipes, recipeGame.id).size() == 1 &&
            listSaveSnapshots(recipes, recipeGame.id).first().files == 0,
            "empty known root clears pending capture with a verified empty snapshot");

    const auto legacyRoot = base + "/Save schema migration";
    touch(legacyRoot + "/Game/game.exe");
    Library legacy(legacyRoot); legacy.import({inspect(legacyRoot + "/Game")}, {});
    const auto legacyGame = legacy.games().first();
    writeJson(legacy.gamePath(legacyGame.id, "saves.json"), {{"schema", 1},
        {"mappings", QJsonArray{QJsonObject{{"scope", "prefix"},
           {"path", "drive_c/users/steamuser/AppData/Roaming/Example/progress.sav"}}}}});
    const auto migrated = loadSaveConfig(legacy, legacyGame.id);
    require(migrated.discoveryState == "identified" && migrated.mappings.first().scope == "appdata" &&
            migrated.mappings.first().path == "Example/progress.sav", "legacy prefix mapping becomes portable AppData root");

    const auto ambiguousRoot = base + "/Ambiguous saves";
    touch(ambiguousRoot + "/Game/game.exe");
    Library ambiguous(ambiguousRoot); ambiguous.import({inspect(ambiguousRoot + "/Game")}, {});
    const auto ambiguousGame = ambiguous.games().first();
    QDir().mkpath(ambiguousGame.folder + "/saves");
    QDir().mkpath(ambiguousGame.folder + "/savedata");
    beginSaveObservation(ambiguous, ambiguousGame);
    const auto uncertain = loadSaveConfig(ambiguous, ambiguousGame.id);
    require(uncertain.discoveryState == "searching" && uncertain.mappings.isEmpty(),
            "two conventional save folders require a manual choice");
    const auto ambiguousCandidates = saveDiscoveryCandidates(ambiguous, ambiguousGame.id);
    require(ambiguousCandidates.size() == 2 && ambiguousCandidates.first().path == "saves" &&
            ambiguousCandidates.last().path == "savedata",
            "both conventional save folders remain available for manual selection");
    auto manual = uncertain; manual.mappings = {{"game", "savedata"}};
    saveSaveConfig(ambiguous, ambiguousGame.id, manual);
    require(loadSaveConfig(ambiguous, ambiguousGame.id).discoveryState == "identified" &&
            loadSaveConfig(ambiguous, ambiguousGame.id).discoverySource == "manual", "manual confirmation overrides ambiguity");

    const auto titleRoot = base + "/Title saves";
    touch(titleRoot + "/Amairo Chocolata/game.exe");
    Library titles(titleRoot); titles.import({inspect(titleRoot + "/Amairo Chocolata")}, {});
    const auto titleGame = titles.games().first();
    QDir().mkpath(titles.prefix(titleGame.id) + "/drive_c/users/steamuser/AppData/Roaming/Publisher/Amairo Chocolata");
    beginSaveObservation(titles, titleGame);
    const auto titleConfig = loadSaveConfig(titles, titleGame.id);
    require(titleConfig.discoveryState == "identified" && titleConfig.mappings.size() == 1 &&
            titleConfig.mappings.first().scope == "appdata" &&
            titleConfig.mappings.first().path == "Publisher/Amairo Chocolata",
            "title-named AppData folder is detected within bounded Wine locations");

    const auto unityRoot = base + "/Unity candidate folders";
    touch(unityRoot + "/Aokana/Aokana.exe");
    touch(unityRoot + "/Aokana/SaveData/AoKana001.dat", "saved progress");
    Library unity(unityRoot); unity.import({inspect(unityRoot + "/Aokana")}, {});
    const auto unityGame = unity.games().first();
    const auto prefixTitle = unity.prefix(unityGame.id) +
        "/drive_c/users/steamuser/AppData/LocalLow/NekoNyanSoft/Aokana";
    touch(prefixTitle + "/output_log.txt", "Unity diagnostics");
    beginSaveObservation(unity, unityGame);
    const auto unityConfig = loadSaveConfig(unity, unityGame.id);
    const auto unityCandidates = saveDiscoveryCandidates(unity, unityGame.id);
    require(unityConfig.discoveryState == "searching" && unityConfig.mappings.isEmpty() &&
            unityCandidates.size() == 2 && unityCandidates.first().scope == "game" &&
            unityCandidates.first().path == "SaveData" && unityCandidates.last().scope == "prefix" &&
            unityCandidates.last().path.endsWith("/NekoNyanSoft/Aokana"),
            "Aokana-style local saves and title-matched Wine folder are both offered for selection");

    const auto adjacentRoot = base + "/Executable adjacent saves";
    touch(adjacentRoot + "/Game/bin/game.exe");
    QDir().mkpath(adjacentRoot + "/Game/bin/UserData");
    Library adjacent(adjacentRoot); adjacent.import({inspect(adjacentRoot + "/Game")}, {});
    const auto adjacentGame = adjacent.games().first();
    beginSaveObservation(adjacent, adjacentGame);
    require(loadSaveConfig(adjacent, adjacentGame.id).mappings.first().path == "bin/UserData",
            "save directory beside selected executable is detected");

    const auto unusualRoot = base + "/Unusual saves";
    touch(unusualRoot + "/Game/game.exe");
    Library unusual(unusualRoot); unusual.import({inspect(unusualRoot + "/Game")}, {});
    const auto unusualGame = unusual.games().first();
    touch(unusualGame.folder + "/Data/Progress/user0.bin", "game folder progress");
    touch(unusual.prefix(unusualGame.id) + "/drive_c/users/steamuser/AppData/Local/Vendor/OtherTitle/state.bin",
          "unrelated prefix data");
    beginSaveObservation(unusual, unusualGame);
    finishSaveObservation(unusual, unusualGame);
    require(loadSaveConfig(unusual, unusualGame.id).discoveryState == "searching" &&
            loadSaveConfig(unusual, unusualGame.id).mappings.isEmpty(),
            "unmatched paths remain unconfigured for manual selection");

    const auto priorRoot = base + "/Legacy observation";
    touch(priorRoot + "/Game/game.exe");
    Library prior(priorRoot); prior.import({inspect(priorRoot + "/Game")}, {});
    const auto priorGame = prior.games().first();
    writeJson(prior.gamePath(priorGame.id, "save-observation.json"), {{"schema", 1}, {"game_id", priorGame.id}});
    QDir().mkpath(priorGame.folder + "/UserData");
    require(recoverSaveObservations(prior).size() == 1 &&
            !QFileInfo::exists(prior.gamePath(priorGame.id, "save-observation.json")) &&
            loadSaveConfig(prior, priorGame.id).mappings.first().path == "UserData",
            "legacy observation metadata is cleared after bounded discovery");

    const auto renpyRoot = base + "/Engine saves";
    touch(renpyRoot + "/Game/game.exe");
    touch(renpyRoot + "/Game/renpy/__init__.py");
    touch(renpyRoot + "/Game/game/options.rpy", "define config.save_directory = 'Example-123'\n");
    Library renpy(renpyRoot); renpy.import({inspect(renpyRoot + "/Game")}, {});
    auto renpyGame = renpy.games().first(); renpyGame.saveStrategy = "renpy"; renpy.save(renpyGame);
    beginSaveObservation(renpy, renpyGame);
    const auto engine = loadSaveConfig(renpy, renpyGame.id);
    require(engine.discoveryState == "identified" && engine.discoverySource == "engine" && engine.mappings.size() == 2 &&
            engine.mappings.first().scope == "appdata" && engine.mappings.first().path == "RenPy/Example-123",
            "verified Ren'Py literal resolves portable prefix and game paths before any save exists");

    const auto sourceRoot = base + "/Portable source", targetRoot = base + "/Portable destination";
    touch(sourceRoot + "/Game/game.exe"); touch(targetRoot + "/Different folder/game.exe");
    Library source(sourceRoot); source.import({inspect(sourceRoot + "/Game")}, {});
    Library destination(targetRoot); destination.import({inspect(targetRoot + "/Different folder")}, {});
    const auto sourceGame = source.games().first(), targetGame = destination.games().first();
    auto portable = loadSaveConfig(source, sourceGame.id);
    portable.gameIdentity = "vndb:v456"; portable.releaseIdentity = "edition-1";
    portable.saveCompatibility = "format-1"; portable.mappings = {{"appdata", "Example/progress.sav"}};
    saveSaveConfig(source, sourceGame.id, portable);
    touch(saveMappingRoot(source, sourceGame, "appdata") + "/Example/progress.sav", "portable progress");
    const auto portableSnapshot = createSaveSnapshot(source, sourceGame.id);
    require(readJson(source.gamePath(sourceGame.id, "save-head.json"))["snapshot_id"] == portableSnapshot.id,
            "local capture records the currently applied snapshot");
    auto targetConfig = portable;
    targetConfig.profileId = destination.profileId();
    targetConfig.profileGameId = loadSaveConfig(destination, targetGame.id).profileGameId;
    saveSaveConfig(destination, targetGame.id, targetConfig);
    const auto sourceSnapshotDir = QFileInfo(source.gamePath(sourceGame.id, "saves.json")).absolutePath() + "/snapshots/" + portableSnapshot.id;
    rejects([&] { importSaveSnapshot(destination, targetGame.id, sourceSnapshotDir); },
            "independent existing-install game IDs require an explicit peer link");
    const auto sourceFingerprint = ensureDeviceIdentity(source).fingerprint;
    approvePairedPeer(destination, sourceFingerprint, "Source Deck");
    linkPeerSaveGame(destination, targetGame.id, sourceFingerprint, portable.profileGameId);
    require(loadSaveConfig(destination, targetGame.id).peerLinks.size() == 1,
            "confirmed peer game link persists without replacing local game identity");
    const auto incoming = importSaveSnapshot(destination, targetGame.id, sourceSnapshotDir, sourceFingerprint);
    require(incoming.added && incoming.relation == "available" && incoming.snapshotId == portableSnapshot.id,
            "verified incoming snapshot is available without changing current saves");
    require(importedSaveCandidates(destination, targetGame.id).size() == 1 &&
            importedSaveCandidates(destination, targetGame.id).first().relation == "available",
            "new peer save is visible before first Play");
    require(adoptInitialPeerSave(destination, targetGame.id) &&
            !adoptInitialPeerSave(destination, targetGame.id),
            "an empty fresh installation applies its one verified peer head once");
    require(importedSaveCandidates(destination, targetGame.id).isEmpty(),
            "applied peer save no longer appears as pending");
    require(contents(saveMappingRoot(destination, targetGame, "appdata") + "/Example/progress.sav") == "portable progress" &&
            targetGame.folder != sourceGame.folder && destination.prefix(targetGame.id) != source.prefix(sourceGame.id),
            "verified snapshot restores under a different install folder and prefix");
    require(importSaveSnapshot(destination, targetGame.id, sourceSnapshotDir, sourceFingerprint).relation == "current",
            "repeated import recognizes the current snapshot without duplication");
    const auto guardedRoot = base + "/Local progress before peer sync";
    touch(guardedRoot + "/Game/game.exe");
    Library guarded(guardedRoot); guarded.import({inspect(guardedRoot + "/Game")}, {});
    const auto guardedGame = guarded.games().first();
    auto guardedConfig = portable;
    guardedConfig.profileId = guarded.profileId();
    guardedConfig.profileGameId = loadSaveConfig(guarded, guardedGame.id).profileGameId;
    saveSaveConfig(guarded, guardedGame.id, guardedConfig);
    approvePairedPeer(guarded, sourceFingerprint, "Source Deck");
    linkPeerSaveGame(guarded, guardedGame.id, sourceFingerprint, portable.profileGameId);
    touch(saveMappingRoot(guarded, guardedGame, "appdata") + "/Example/progress.sav", "local progress");
    importSaveSnapshot(guarded, guardedGame.id, sourceSnapshotDir, sourceFingerprint);
    require(!adoptInitialPeerSave(guarded, guardedGame.id) &&
            contents(saveMappingRoot(guarded, guardedGame, "appdata") + "/Example/progress.sav") == "local progress" &&
            !appliedSaveSnapshotId(guarded, guardedGame.id).isEmpty() &&
            importedSaveCandidates(guarded, guardedGame.id).first().relation == "conflict",
            "initial peer adoption preserves and snapshots unexpected local progress");
    touch(saveMappingRoot(source, sourceGame, "appdata") + "/Example/progress.sav", "source branch");
    const auto sourceBranch = createSaveSnapshot(source, sourceGame.id);
    const auto sourceBranchManifest = readJson(QFileInfo(source.gamePath(sourceGame.id, "saves.json")).absolutePath() +
                                             "/snapshots/" + sourceBranch.id + "/manifest.json");
    require(sourceBranchManifest["parent_ids"].toArray().first() == portableSnapshot.id,
            "new save state records the applied parent snapshot");
    touch(saveMappingRoot(destination, targetGame, "appdata") + "/Example/progress.sav", "destination branch");
    const auto localBranch = createSaveSnapshot(destination, targetGame.id);
    const auto sourceBranchDir = QFileInfo(source.gamePath(sourceGame.id, "saves.json")).absolutePath() + "/snapshots/" + sourceBranch.id;
    const auto damagedBundle = base + "/Unsafe bundle";
    QDir().mkpath(damagedBundle + "/items");
    touch(damagedBundle + "/manifest.json", contents(sourceBranchDir + "/manifest.json"));
    touch(damagedBundle + "/unexpected.txt", "not part of snapshot");
    rejects([&] { importSaveSnapshot(destination, targetGame.id, damagedBundle, sourceFingerprint); },
            "incoming bundle with undeclared files is rejected");
    require(importSaveSnapshot(destination, targetGame.id, sourceBranchDir, sourceFingerprint).relation == "conflict" &&
            contents(saveMappingRoot(destination, targetGame, "appdata") + "/Example/progress.sav") == "destination branch" &&
            listSaveSnapshots(destination, targetGame.id).size() == 3,
            "divergent imported branch is preserved without overwriting playable local progress");
    require(!importedSaveCandidates(destination, targetGame.id).isEmpty() &&
            importedSaveCandidates(destination, targetGame.id).first().relation == "conflict",
            "conflicting peer progress stays visible without changing local saves");
    restoreSaveSnapshot(destination, targetGame.id, sourceBranch.id);
    require(contents(saveMappingRoot(destination, targetGame, "appdata") + "/Example/progress.sav") == "source branch" &&
            importSaveSnapshot(destination, targetGame.id, sourceBranchDir, sourceFingerprint).relation == "current" &&
            listSaveSnapshots(destination, targetGame.id).size() == 3,
            "manual conflict choice restores incoming branch and retains local branch");
    auto wrongEdition = loadSaveConfig(destination, targetGame.id);
    wrongEdition.releaseIdentity = "other build"; saveSaveConfig(destination, targetGame.id, wrongEdition);
    rejects([&] { importSaveSnapshot(destination, targetGame.id, sourceBranchDir, sourceFingerprint); },
            "incoming snapshot from another edition is refused before touching saves");

    const auto simpleSourceRoot = base + "/Simple peer source", simpleTargetRoot = base + "/Simple peer target";
    touch(simpleSourceRoot + "/Same Name/game.exe");
    touch(simpleTargetRoot + "/Same Name/game.exe");
    Library simpleSource(simpleSourceRoot), simpleTarget(simpleTargetRoot);
    simpleSource.import({inspect(simpleSourceRoot + "/Same Name")}, {});
    simpleTarget.import({inspect(simpleTargetRoot + "/Same Name")}, {});
    const auto sourceSimpleGame = simpleSource.games().first(), targetSimpleGame = simpleTarget.games().first();
    auto sourceSimpleConfig = loadSaveConfig(simpleSource, sourceSimpleGame.id);
    sourceSimpleConfig.mappings = {{"game", "UserData"}};
    saveSaveConfig(simpleSource, sourceSimpleGame.id, sourceSimpleConfig);
    auto targetSimpleConfig = loadSaveConfig(simpleTarget, targetSimpleGame.id);
    targetSimpleConfig.mappings = {{"game", "UserData"}};
    saveSaveConfig(simpleTarget, targetSimpleGame.id, targetSimpleConfig);
    touch(sourceSimpleGame.folder + "/UserData/slot.sav", "simple peer progress");
    const auto simpleSnapshot = createSaveSnapshot(simpleSource, sourceSimpleGame.id);
    const auto simpleBundle = QFileInfo(simpleSource.gamePath(sourceSimpleGame.id, "saves.json")).absolutePath() +
                              "/snapshots/" + simpleSnapshot.id;
    const auto simpleFingerprint = ensureDeviceIdentity(simpleSource).fingerprint;
    approvePairedPeer(simpleTarget, simpleFingerprint, "Simple source");
    linkPeerSaveGame(simpleTarget, targetSimpleGame.id, simpleFingerprint, sourceSimpleConfig.profileGameId);
    require(importSaveSnapshot(simpleTarget, targetSimpleGame.id, simpleBundle, simpleFingerprint).added,
            "linked games exchange verified saves without identity, build or compatibility labels");
    restoreSaveSnapshot(simpleTarget, targetSimpleGame.id, simpleSnapshot.id);
    require(contents(targetSimpleGame.folder + "/UserData/slot.sav") == "simple peer progress",
            "unlabeled peer snapshot restores through confirmed portable mapping");
    const auto renamed = simpleSourceRoot + "/Renamed folder";
    require(QDir().rename(sourceSimpleGame.folder, renamed), "rename source game folder");
    simpleSource.relink(simpleSource.planRelink(sourceSimpleGame.id, renamed));
    require(loadSaveConfig(simpleSource, sourceSimpleGame.id).folderAliases.contains("Same Name") &&
            simpleSource.game(sourceSimpleGame.id).id == sourceSimpleGame.id &&
            !listSaveSnapshots(simpleSource, sourceSimpleGame.id).isEmpty(),
            "folder rename retains stable ID, previous name and snapshot history");
}
void testSteamShortcut(const QString &base) {
    const auto root = base + "/Steam shortcut game";
    touch(root + "/Game/game.exe");
    Library lib(root); lib.import({inspect(root + "/Game")}, {});
    const auto game = lib.games().first();
    const auto account = base + "/Steam account/config/shortcuts.vdf";
    QDir().mkpath(QFileInfo(account).absolutePath());
    const auto executable = QCoreApplication::applicationFilePath();
    const auto added = addSteamShortcut(lib, game, account, executable);
    require(added.added && added.appId != 0 && contents(account).contains("--novelty-shortcut"),
            "Steam shortcut invokes Novelty's managed launch");
    const auto original = contents(account);
    const auto again = addSteamShortcut(lib, game, account, executable);
    require(!again.added && again.appId == added.appId && contents(account) == original,
            "repeated Add to Steam does not duplicate or rewrite the shortcut");
    touch(root + "/Another/other.exe"); lib.import({inspect(root + "/Another")}, {});
    const auto other = lib.games().first().id == game.id ? lib.games().last() : lib.games().first();
    require(addSteamShortcut(lib, other, account, executable).added &&
            contents(account).startsWith(original.left(11)) && contents(account).contains(game.name.toUtf8()),
            "adding another game preserves the prior shortcut");
    const auto movedExecutable = base + "/Moved Novelty/novelty";
    touch(movedExecutable, "fake executable");
    QFile::setPermissions(movedExecutable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto updated = addSteamShortcut(lib, game, account, movedExecutable);
    require(updated.added && updated.appId == added.appId && contents(account).contains(movedExecutable.toUtf8()) &&
            !addSteamShortcut(lib, game, account, movedExecutable).added,
            "repeat Add updates a moved app path without changing the shortcut ID");
    const auto damaged = base + "/Damaged Steam/config/shortcuts.vdf";
    touch(damaged, "not binary VDF");
    rejects([&] { addSteamShortcut(lib, game, damaged, executable); }, "unrecognized Steam format fails without editing it");
    require(contents(damaged) == "not binary VDF", "damaged Steam shortcut file is unchanged");
    const auto sample = qEnvironmentVariable("NOVELTY_STEAM_FIXTURE");
    if (!sample.isEmpty()) {
        const auto originalSample = contents(sample);
        const auto copied = base + "/Copied Steam/config/shortcuts.vdf";
        touch(copied, originalSample);
        require(addSteamShortcut(lib, game, copied, executable).added &&
                contents(sample) == originalSample && contents(copied).size() > originalSample.size(),
                "real Steam-format fixture accepts a new shortcut while source remains untouched");
    }
}
void testFolderImport(const QString &base) {
    const auto root = base + "/Move library"; QDir().mkpath(root);
    const auto source = base + "/Unsorted 日本語"; touch(source + "/game.exe", "game");
    Library lib(root); lib.setSettings({{"runner", base + "/runner"}});
    const auto moved = lib.addFolder(source).game;
    require(moved.folder == root + "/Unsorted 日本語" && moved.executable == moved.folder + "/game.exe" &&
            QFileInfo::exists(moved.executable) && !QFileInfo::exists(source), "external game folder moves and scans inside Games");
    require(lib.addFolder(moved.folder).game.id == moved.id && lib.games().size() == 1, "direct child rescans without duplicating or moving");
    const auto conflict = base + "/Other/Unsorted 日本語"; touch(conflict + "/other.exe");
    rejects([&] { lib.addFolder(conflict); }, "destination name conflict is rejected");
    require(QFileInfo::exists(conflict + "/other.exe") && QFileInfo::exists(moved.executable), "name conflict keeps both folders intact");
    const auto ambiguousSource = base + "/Two candidates";
    touch(ambiguousSource + "/main.exe"); touch(ambiguousSource + "/Extras/alternate.EXE");
    auto ambiguous = lib.addFolder(ambiguousSource).game;
    require(ambiguous.executable.isEmpty() && ambiguous.id == lib.game(ambiguous.id).id,
            "multiple executables leave a stable entry awaiting selection");
    rejects([&] { lib.selectExecutable(ambiguous.id, moved.executable); }, "selection cannot escape the game folder");
    rejects([&] { lib.selectExecutable(ambiguous.id, ambiguous.folder + "/missing.exe"); }, "missing executable rejected");
    touch(ambiguous.folder + "/readme.txt");
    rejects([&] { lib.selectExecutable(ambiguous.id, ambiguous.folder + "/readme.txt"); }, "non-executable selection rejected");
    const auto chosen = lib.selectExecutable(ambiguous.id, ambiguous.folder + "/Extras/alternate.EXE");
    require(chosen.id == ambiguous.id && chosen.executable == ambiguous.folder + "/Extras/alternate.EXE" &&
            chosen.workingDirectory == ambiguous.folder + "/Extras" && !lib.game(chosen.id).executable.isEmpty(),
            "manual executable selection keeps ID and nested working directory");
    rejects([&] { lib.selectExecutable(ambiguous.id, ambiguous.folder + "/main.exe"); },
            "second onboarding selection cannot replace an already configured game");
    rejects([&] { lib.addFolder(root); }, "Games root cannot be selected as a game");
    touch(moved.folder + "/nested/patch.exe");
    rejects([&] { lib.addFolder(moved.folder + "/nested"); }, "nested registered folder cannot be pulled into Games");
    const auto legacy = base + "/Legacy game"; touch(legacy + "/old.exe");
    lib.import({inspect(legacy)}, {}); auto old = lib.games().last();
    for (const auto &g : lib.games()) if (g.folder == legacy) old = g;
    old.locale = "ja_JP.UTF-8"; lib.save(old);
    const auto relinked = lib.addFolder(legacy).game;
    require(relinked.id == old.id && relinked.locale == old.locale && relinked.folder == root + "/Legacy game" &&
            relinked.executable == relinked.folder + "/old.exe" && !QFileInfo::exists(legacy),
            "previously registered external folder keeps its ID and configuration when moved");
    const auto removed = base + "/Conflict source"; touch(removed + "/game.exe");
    touch(root + "/Conflict source/other.exe");
    rejects([&] { lib.addFolder(removed); }, "existing unregistered destination blocks move");
    require(QFileInfo::exists(removed + "/game.exe"), "failed move preserves source");
    QTemporaryDir alternate("/tmp/novelty-folder-import-XXXXXX");
    struct stat alternateStat{}, rootStat{};
    if (alternate.isValid() && QFileInfo(alternate.path()).isWritable() &&
        stat(QFile::encodeName(alternate.path()).constData(), &alternateStat) == 0 &&
        stat(QFile::encodeName(root).constData(), &rootStat) == 0 && alternateStat.st_dev != rootStat.st_dev) {
        rejects([&] { lib.addFolder("/tmp"); }, "mounted drive root is not treated as a game folder");
        const auto remote = alternate.path() + "/SD game"; touch(remote + "/game.exe", "cross-device");
        const auto copied = lib.addFolder(remote).game;
        require(copied.folder == root + "/SD game" && QFileInfo::exists(copied.executable) && !QFileInfo::exists(remote),
                "cross-filesystem folder copy verifies and retires source");
    }
    const auto pendingSource = base + "/Import original"; touch(pendingSource + "/game.exe", "keep");
    const auto pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto pendingStage = root + "/.novelty-importing-" + pendingId;
    const auto pendingJournal = lib.dataRoot() + "/imports/" + pendingId + ".json";
    touch(pendingStage + "/partial.bin", "incomplete");
    auto pending = QJsonObject{{"schema", 1}, {"source", pendingSource},
        {"source_identity", folderIdentity(pendingSource)}, {"destination", root + "/Import original"},
        {"stage", pendingStage}, {"stage_identity", folderIdentity(pendingStage)}};
    writeJson(pendingJournal, pending);
    require(lib.recoverFolderImports().size() == 1 && !QFileInfo::exists(pendingStage) &&
        !QFileInfo::exists(pendingJournal) && QFileInfo::exists(pendingSource + "/game.exe"),
        "known incomplete import copy is removed while source survives");
    touch(pendingStage + "/partial.bin", "changed");
    pending["stage_identity"] = "different-directory"; writeJson(pendingJournal, pending);
    require(!lib.recoverFolderImports().isEmpty() && QFileInfo::exists(pendingStage + "/partial.bin") &&
        QFileInfo::exists(pendingJournal), "changed import stage is retained for inspection");
    pending["stage_identity"] = folderIdentity(pendingStage); writeJson(pendingJournal, pending);
    touch(root + "/Import original/replacement.exe");
    require(!lib.recoverFolderImports().isEmpty() && QFileInfo::exists(pendingStage + "/partial.bin") &&
        QFileInfo::exists(root + "/Import original/replacement.exe"),
        "destination conflict retains both copies for inspection");
    QDir(root + "/Import original").removeRecursively();
    QFile::remove(pendingJournal); QDir(pendingStage).removeRecursively();
    const auto untracked = root + "/.novelty-importing-legacy"; touch(untracked + "/game.exe");
    require(lib.recoverFolderImports().isEmpty() && QFileInfo::exists(untracked + "/game.exe"),
        "untracked older import copy is never removed broadly");
}
void testOneLaunchDiagnostics(const QString &base) {
    const auto root = base + "/Diagnostics"; touch(root + "/Game/game.exe");
    const auto runner = base + "/diagnostic-runner"; touch(runner + "/proton");
    const auto fake = base + "/diagnostic-umu";
    touch(fake, R"SH(#!/bin/sh
if [ "$2" = "/c" ]; then mkdir -p "$WINEPREFIX/drive_c"; touch "$WINEPREFIX/system.reg"; exit 0; fi
printf '%s\n' "$WINEDEBUG" > "$WINEPREFIX/observed-debug"
printf 'Synthetic Wine loader output\n'
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); g.environment["WINEDEBUG"] = "saved-preset";
    g.environment["SECRET_TOKEN"] = "never-export-this-value";
    g.arguments = {"--secret=also-never-export"}; lib.save(g); g = lib.game(g.id);
    const auto original = g.json();
    require(supervise(lib, g.id, false, {}, {}, false, true) == 0, "one-time debug launch works");
    QFile observed(lib.prefix(g.id) + "/observed-debug");
    require(observed.open(QIODevice::ReadOnly) && observed.readAll() == "+seh,+loaddll\n", "debug trace overrides only this invocation"); observed.close();
    QFile log(lib.gamePath(g.id, "launch.log"));
    require(log.open(QIODevice::ReadOnly) && log.readAll().contains("One-launch Wine debug: +seh,+loaddll"), "diagnostic log identifies debug capture"); log.close();
    const auto report = diagnosticReport(lib, g.id);
    const auto payload = QJsonDocument(report).toJson();
    require(report["schema"] == 1 && report["game"].toObject()["id"] == g.id && report["launch_log"].toString().contains("Synthetic Wine loader output"), "diagnostic report captures saved state and current log");
    require(report["game"].toObject()["environment_names"].toArray().contains("SECRET_TOKEN") &&
            !payload.contains("never-export-this-value") && !payload.contains("also-never-export"), "diagnostic report omits custom environment values and arguments");
    require(supervise(lib, g.id, false) == 0 && lib.game(g.id).json() == original, "ordinary Play resumes saved configuration");
    require(observed.open(QIODevice::ReadOnly) && observed.readAll() == "saved-preset\n", "debug override did not persist into normal launch"); observed.close();
    const auto afterNormal = diagnosticReport(lib, g.id);
    require(!afterNormal["launch_log"].toString().contains("One-launch Wine debug") &&
            afterNormal["launch_log_previous"].toString().contains("One-launch Wine debug"),
            "normal launch retains bounded debug history for export");
    require(supervise(lib, g.id, false, {}, {}, true, true) == 1, "invalid installer/debug combination rejected");
    writeJson(lib.gamePath(g.id, "status.json"), {{"state", "Ready"}});
    touch(lib.gamePath(g.id, "status.json"), "broken status");
    require(diagnosticReport(lib, g.id)["status"].toObject().contains("read_error"), "damaged status remains exportable");
}
void testGameInstallation(const QString &base) {
    const auto root = base + "/Install library"; QDir().mkpath(root);
    const auto runner = base + "/installer-runner"; touch(runner + "/proton");
    const auto source = base + "/Installer media 日本語/setup.exe"; touch(source); touch(QFileInfo(source).absolutePath() + "/data.bin", "media");
    const auto fake = base + "/installer-umu";
    touch(fake, R"SH(#!/bin/sh
if [ "$2" = "/c" ]; then mkdir -p "$WINEPREFIX/drive_c"; touch "$WINEPREFIX/system.reg"; exit 0; fi
printf '%s\n' "$1" "$PWD" >> "$WINEPREFIX/installer-invocations"
printf '%s\n' "$HOST_LC_ALL" "$LANG" > "$WINEPREFIX/installer-locale"
[ -f data.bin ] || exit 42
[ "$INSTALL_MODE" = fail ] && exit 7
[ "$INSTALL_MODE" = empty ] && exit 0
mkdir -p "$WINEPREFIX/dosdevices/n:/Novel/renpy" "$WINEPREFIX/dosdevices/n:/Novel/game"
touch "$WINEPREFIX/dosdevices/n:/Novel/Story.exe" "$WINEPREFIX/dosdevices/n:/Novel/renpy/__init__.py"
[ "$INSTALL_MODE" = multiple ] && touch "$WINEPREFIX/dosdevices/n:/Novel/Alt.exe"
exit 0
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}, {"backend", "steam"}});
    for (const auto &name : {QString("../escape"), QString(".novelty"), QString("CON"), QString("Bad?Name"), QString()})
        rejects([&] { lib.createInstallation(name, source, runner); }, "invalid installation name rejected");
    rejects([&] { lib.createInstallation("Missing media", "/missing.exe", runner); }, "missing installer rejected before creating folder");
    rejects([&] { lib.createInstallation("Missing runner", source, "/missing"); }, "missing runner rejected before creating folder");
    require(lib.games().isEmpty() && !QFileInfo::exists(root + "/Missing media"), "invalid installer has no game side effects");
    auto g = lib.createInstallation("New story", source, runner);
    require(g.installPending && g.executable.isEmpty() && g.backend == "umu" && lib.status(g) == "Installation pending", "new installer entry is pending and always uses umu");
    require(lib.game(g.id).installer == source && lib.game(g.id).installPending, "pending installation survives serialization");
    rejects([&] { lib.createInstallation(g.name, source, runner); }, "existing installation never overwritten");
    rejects([&] { launchCommand(lib, g); }, "pending installation cannot accidentally play installer");
    rejects([&] { lib.finishInstallation(g.id); }, "detection requires operation ownership");
    QLockFile lock(lib.gamePath(g.id, "launch.lock")); QDir().mkpath(QFileInfo(lock.fileName()).absolutePath());
    require(lock.tryLock(), "hold installer operation lock");
    rejects([&] { lib.detectInstallation(g.id); }, "detection cannot race an installer"); lock.unlock();
    require(supervise(lib, g.id, false, {}, {}, true) == 0, "supervised installer completes");
    const auto installed = lib.game(g.id);
    require(!installed.installPending && installed.installer.isEmpty() && installed.id == g.id && installed.executable == g.folder + "/Novel/Story.exe", "detection selects installed game retaining ID");
    require(installed.engine == "Ren'Py" && installed.setupProfile == "renpy-bundled-v1", "nested installed engine selects profile");
    require(canonicalPath(lib.prefix(g.id) + "/dosdevices/n:") == g.folder && setupComplete(lib, installed), "mapped destination and prepared prefix retained");
    QFile invocations(lib.prefix(g.id) + "/installer-invocations");
    require(invocations.open(QIODevice::ReadOnly) && invocations.readAll() == (source + '\n' + QFileInfo(source).absolutePath() + '\n').toUtf8(), "installer keeps external media working directory");
    require(readJson(lib.gamePath(g.id, "extra-launch.json"))["kind"] == "installer", "installer operation audited");
    auto japanese = lib.createInstallation("Japanese text", source, runner, "ja_JP.UTF-8");
    require(japanese.locale == "ja_JP.UTF-8" && launchCommand(lib, japanese, true).environment.value("HOST_LC_ALL") == "ja_JP.UTF-8" &&
            launchCommand(lib, japanese, true).environment.value("LANG") == "ja_JP.UTF-8", "Japanese installer selection sets both Proton and process locale");
    require(supervise(lib, japanese.id, false, {}, {}, true) == 0, "Japanese installer operation completes");
    QFile localeRecord(lib.prefix(japanese.id) + "/installer-locale");
    require(localeRecord.open(QIODevice::ReadOnly) && localeRecord.readAll() == "ja_JP.UTF-8\nja_JP.UTF-8\n", "installer receives Japanese locale in its real command environment"); localeRecord.close();
    require(lib.game(japanese.id).locale == "ja_JP.UTF-8", "installed game keeps selected Japanese locale");
    require(supervise(lib, g.id, false, {}, {}, true) == 1, "completed installer cannot be silently rerun");
    auto failed = lib.createInstallation("Retry", source, runner); failed.environment["INSTALL_MODE"] = "fail"; lib.save(failed);
    require(supervise(lib, failed.id, false, {}, {}, true) == 1 && lib.game(failed.id).installPending, "failed installer remains recoverable");
    touch(lib.prefix(failed.id) + "/keep-save", "save"); failed.environment["INSTALL_MODE"] = "empty"; lib.save(failed);
    require(supervise(lib, failed.id, false, {}, {}, true) == 0 && lib.game(failed.id).installPending, "empty successful install stays pending");
    require(lib.status(lib.game(failed.id)) == "Installation pending", "empty installer gives actionable pending status");
    const auto completed = readJson(lib.gamePath(failed.id, "extra-launch.json"));
    touch(failed.folder + "/Recovered.exe");
    require(!lib.recoverInstallations().isEmpty() && !lib.game(failed.id).installPending && QFileInfo::exists(lib.prefix(failed.id) + "/keep-save"), "completed installer recovery detects without resetting prefix");
    require(readJson(lib.gamePath(failed.id, "extra-launch.json")) == completed, "recovery never reexecutes installer");
    auto manual = lib.createInstallation("Manual files", source, runner);
    touch(manual.folder + "/manual.EXE"); touch(lib.prefix(manual.id) + "/keep-save", "save");
    const auto chosenAfterInstall = lib.selectExecutable(manual.id, manual.folder + "/manual.EXE");
    require(!chosenAfterInstall.installPending && chosenAfterInstall.installer.isEmpty() &&
            chosenAfterInstall.executable == manual.folder + "/manual.EXE" &&
            QFileInfo::exists(lib.prefix(manual.id) + "/keep-save"),
            "explicit executable selection completes pending install without losing prefix saves");
    auto multiple = lib.createInstallation("Ambiguous", source, runner); multiple.environment["INSTALL_MODE"] = "multiple"; lib.save(multiple);
    require(supervise(lib, multiple.id, false, {}, {}, true) == 0, "multi-executable installer completes");
    require(!lib.game(multiple.id).installPending && lib.game(multiple.id).executable.isEmpty(), "ambiguous installed executables require user choice");
    auto conflict = lib.createInstallation("Mapping conflict", source, runner);
    QDir().mkpath(lib.prefix(conflict.id) + "/dosdevices"); QFile::link(root, lib.prefix(conflict.id) + "/dosdevices/n:");
    require(supervise(lib, conflict.id, false, {}, {}, true) == 1 && canonicalPath(lib.prefix(conflict.id) + "/dosdevices/n:") == root, "installer refuses to overwrite conflicting drive mapping");
    touch(root + "/Media/setup.exe"); lib.import({inspect(root + "/Media")}, runner);
    Game media; for (const auto &entry : lib.games()) if (entry.folder == root + "/Media") media = entry;
    lib.createInstallation("Uses media", media.folder + "/setup.exe", runner);
    rejects([&] { lib.planRemoval(media.id); }, "pending installer media protected from another game's removal");
}
void testSchemaAndManagedRunners(const QString &base) {
    const auto root = base + "/Schema upgrade"; touch(root + "/Game/game.exe"); QDir().mkpath(root + "/.novelty");
    Game g; g.id = QUuid::createUuid().toString(QUuid::WithoutBraces); g.name = "Retained title";
    g.folder = root + "/Game"; g.executable = g.folder + "/game.exe"; g.workingDirectory = g.folder;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "old-schema-fixture"); db.setDatabaseName(root + "/.novelty/library.sqlite");
        require(db.open(), "open schema 1 fixture");
        {
            QSqlQuery q(db);
            require(q.exec("CREATE TABLE games (id TEXT PRIMARY KEY, folder TEXT UNIQUE, config TEXT)"), "old games schema");
            require(q.exec("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT)"), "old settings schema");
            require(q.exec("PRAGMA user_version=1"), "set old schema version");
            q.prepare("INSERT INTO games VALUES(?,?,?)"); q.addBindValue(g.id); q.addBindValue(g.folder);
            q.addBindValue(QString::fromUtf8(QJsonDocument(g.json()).toJson())); require(q.exec(), "old game persisted");
        }
        db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase("old-schema-fixture");
    }
    Library lib(root);
    require(lib.game(g.id).json() == g.json(), "schema migration preserves game configuration");
    require(lib.recoverRemovals().isEmpty(), "new recovery table is empty after migration");
    const auto managed = root + "/.novelty/runners/GE-Proton11-7";
    const auto incomplete = root + "/.novelty/runners/.install-GE-Proton11-7";
    touch(managed + "/proton"); touch(incomplete + "/proton");
    const auto runners = installedRunners(root);
    require(runners.contains(canonicalPath(managed)), "managed GE build appears in runner picker source");
    require(!runners.contains(canonicalPath(incomplete)), "incomplete hidden download stays out of runner picker");
    g.runner = managed; lib.save(g); lib.setSettings({{"umu", "umu-run"}});
    const auto pinned = managedUmuPath(root), old = lib.dataRoot() + "/tools/umu-run";
    touch(pinned, "#!/bin/sh\n"); QFile::setPermissions(pinned, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto oldPath = qgetenv("PATH"); qputenv("PATH", "/nonexistent-novelty-fixture");
    const auto pinnedCommand = launchCommand(lib, g);
    require(!lib.managedRuntimeStorage() &&
            pinnedCommand.environment.value("UMU_FOLDERS_PATH") == QProcessEnvironment::systemEnvironment().value("UMU_FOLDERS_PATH"),
            "existing libraries keep their prior umu runtime location");
    auto redirected = g; redirected.environment["UMU_FOLDERS_PATH"] = base + "/outside";
    require(launchCommand(lib, redirected).environment.value("UMU_FOLDERS_PATH") == base + "/outside",
            "legacy runtime overrides remain editable");
    touch(old, "#!/bin/sh\n"); QFile::setPermissions(old, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto legacyCommand = launchCommand(lib, g); qputenv("PATH", oldPath);
    require(pinnedCommand.program == pinned, "pinned umu fills a missing default executable");
    require(legacyCommand.program == old, "adding a pinned tool preserves existing legacy default selection");
    const auto freshRoot = base + "/Fresh managed runtime", freshRunner = base + "/Fresh runner";
    touch(freshRoot + "/Game/game.exe"); touch(freshRunner + "/proton");
    Library fresh(freshRoot); fresh.setSettings({{"umu", pinned}}); fresh.import({inspect(freshRoot + "/Game")}, freshRunner);
    auto freshGame = fresh.games().first();
    const auto freshCommand = launchCommand(fresh, freshGame);
    require(fresh.managedRuntimeStorage() &&
            freshCommand.environment.value("UMU_FOLDERS_PATH") == fresh.dataRoot() + "/runtime" &&
            freshCommand.environment.value("XDG_CACHE_HOME") == fresh.dataRoot() + "/cache" &&
            freshCommand.environment.value("TMPDIR") == fresh.dataRoot() + "/cache/tmp" &&
            QFileInfo(fresh.dataRoot() + "/cache/tmp").isDir(),
            "new libraries keep umu's runtime and cache inside Games");
    const auto previousLib = qgetenv("LD_LIBRARY_PATH"), previousOriginal = qgetenv("NOVELTY_ORIGINAL_LD_LIBRARY_PATH");
    const auto previousMarker = qgetenv("NOVELTY_BUNDLED_LIB_DIR"), previousPlugins = qgetenv("QT_PLUGIN_PATH");
    const auto previousOriginalPlugins = qgetenv("NOVELTY_ORIGINAL_QT_PLUGIN_PATH");
    const bool hadLib = qEnvironmentVariableIsSet("LD_LIBRARY_PATH"), hadOriginal = qEnvironmentVariableIsSet("NOVELTY_ORIGINAL_LD_LIBRARY_PATH");
    const bool hadMarker = qEnvironmentVariableIsSet("NOVELTY_BUNDLED_LIB_DIR"), hadPlugins = qEnvironmentVariableIsSet("QT_PLUGIN_PATH");
    const bool hadOriginalPlugins = qEnvironmentVariableIsSet("NOVELTY_ORIGINAL_QT_PLUGIN_PATH");
    qputenv("LD_LIBRARY_PATH", "/fake-bundle/lib"); qputenv("NOVELTY_ORIGINAL_LD_LIBRARY_PATH", "/original/lib");
    qputenv("NOVELTY_BUNDLED_LIB_DIR", "/fake-bundle/lib"); qputenv("QT_PLUGIN_PATH", "/fake-bundle/plugins");
    qputenv("NOVELTY_ORIGINAL_QT_PLUGIN_PATH", "/original/plugins");
    const auto external = launchCommand(fresh, freshGame).environment;
    require(external.value("LD_LIBRARY_PATH") == "/original/lib" && external.value("QT_PLUGIN_PATH") == "/original/plugins" &&
            !external.contains("NOVELTY_BUNDLED_LIB_DIR") && !external.contains("NOVELTY_ORIGINAL_LD_LIBRARY_PATH") &&
            !external.contains("NOVELTY_ORIGINAL_QT_PLUGIN_PATH"),
            "packaged Qt libraries and plugins do not leak into Proton");
    if (hadLib) qputenv("LD_LIBRARY_PATH", previousLib); else qunsetenv("LD_LIBRARY_PATH");
    if (hadOriginal) qputenv("NOVELTY_ORIGINAL_LD_LIBRARY_PATH", previousOriginal); else qunsetenv("NOVELTY_ORIGINAL_LD_LIBRARY_PATH");
    if (hadMarker) qputenv("NOVELTY_BUNDLED_LIB_DIR", previousMarker); else qunsetenv("NOVELTY_BUNDLED_LIB_DIR");
    if (hadPlugins) qputenv("QT_PLUGIN_PATH", previousPlugins); else qunsetenv("QT_PLUGIN_PATH");
    if (hadOriginalPlugins) qputenv("NOVELTY_ORIGINAL_QT_PLUGIN_PATH", previousOriginalPlugins); else qunsetenv("NOVELTY_ORIGINAL_QT_PLUGIN_PATH");
    freshGame.environment["UMU_FOLDERS_PATH"] = base + "/outside";
    rejects([&] { launchCommand(fresh, freshGame); }, "managed runtime cannot be redirected by a game override");
}
void testRuntimeBarrier(const QString &base, const QString &runner, const QString &fake) {
    const auto root = base + "/Runtime barrier"; touch(root + "/Game/game.exe");
    const auto destination = base + "/Runtime relink"; touch(destination + "/game.exe");
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    const auto g = lib.games().first();
    const auto removal = lib.planRemoval(g.id);
    const auto relink = lib.planRelink(g.id, destination);
    const auto path = lib.gamePath(g.id, "runtime.json");
    QFile bootFile("/proc/sys/kernel/random/boot_id"); require(bootFile.open(QIODevice::ReadOnly), "read fixture boot identity");
    const auto boot = QString::fromUtf8(bootFile.readAll()).trimmed();
    writeJson(path, {{"schema", 1}, {"boot_id", boot}, {"supervisor_pid", 99999999}});
    require(lib.busy(g.id) && !lib.supervisorActive(g.id), "lost supervisor barrier differs from active lock");
    require(lib.status(g) == "Recovery required" && lib.recoveryIssue(g.id).contains("Restart"), "recovery status explains next step");
    rejects([&] { lib.save(g); }, "crash barrier blocks edits");
    rejects([&] { lib.planRelink(g.id, destination); }, "crash barrier blocks relink preview");
    rejects([&] { lib.relink(relink); }, "crash barrier blocks previously planned relink");
    rejects([&] { lib.planRemoval(g.id); }, "crash barrier blocks removal preview");
    rejects([&] { lib.remove(removal); }, "crash barrier blocks previously planned removal");
    require(supervise(lib, g.id, false) == 2 && supervise(lib, g.id, true) == 2, "crash barrier blocks play and prepare");
    {
        Library reopened(root);
        require(reopened.busy(g.id) && reopened.status(g) == "Recovery required", "reopening cannot discard recovery barrier");
    }
    touch(path, "damaged");
    require(lib.busy(g.id) && lib.recoveryIssue(g.id).contains("damaged"), "damaged recovery record fails closed");
    writeJson(path, {{"schema", 1}, {"boot_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    require(!lib.busy(g.id), "previous boot cannot retain live descendant processes");
    require(supervise(lib, g.id, true) == 0 && !QFileInfo::exists(path), "new preparation clears previous boot record");
    require(QFileInfo::exists(g.executable) && lib.game(g.id).folder == g.folder, "blocked operations preserve original installation");
}
void testExtraLaunch(const QString &base) {
    const auto root = base + "/Extra launch", runner = base + "/Extra runner", fake = base + "/extra-umu";
    const auto patch = base + "/Patches 日本語/patch.EXE";
    touch(root + "/Game/game.exe"); touch(runner + "/proton"); touch(patch);
    touch(fake, R"SH(#!/bin/sh
if [ "$2" = "/c" ]; then
    mkdir -p "$WINEPREFIX"; touch "$WINEPREFIX/system.reg"
else
    printf '%s\n' "$PWD" "$@" > "$WINEPREFIX/invocation"
    exit "${EXTRA_EXIT:-0}"
fi
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); g.arguments = {"game-only"}; g.locale = "ja_JP.UTF-8"; lib.save(g); g = lib.game(g.id);
    const auto saved = g.json(); const auto prefix = lib.prefix(g.id), recordPath = lib.gamePath(g.id, "extra-launch.json");
    touch(prefix + "/save.dat", "keep save");
    ExtraLaunch extra{patch, {}, {"argument with spaces", "$(do-not-execute)", "--option=value", "", "日本語"}};
    const auto cmd = extraLaunchCommand(lib, g, extra);
    require(cmd.cwd == QFileInfo(patch).absolutePath() && cmd.arguments.size() == 6 && cmd.arguments[4].isEmpty(), "extra executable uses its own cwd and literal arguments including empty values");
    require(cmd.environment.value("WINEPREFIX") == prefix && cmd.environment.value("PROTONPATH") == runner && cmd.environment.value("HOST_LC_ALL") == g.locale && cmd.environment.value("LANG") == g.locale, "extra executable keeps saved prefix, runner and locale");
    require(supervise(lib, g.id, false, {}, extra) == 0 && setupComplete(lib, g), "extra executable prepares first and completes");
    auto record = readJson(recordPath);
    require(record["state"] == "complete" && record["executable"] == patch && record["cwd"] == cmd.cwd && record["exit_code"] == 0, "extra operation has durable completion and launch context");
    require(lib.game(g.id).json() == saved, "extra executable does not replace saved game configuration");
    QFile invocation(prefix + "/invocation"); require(invocation.open(QIODevice::ReadOnly), "read extra invocation");
    require(invocation.readAll() == (cmd.cwd + '\n' + cmd.arguments.join('\n') + '\n').toUtf8(), "extra argument bytes reach backend unchanged"); invocation.close();
    require(supervise(lib, g.id, false) == 0 && readJson(recordPath) == record, "ordinary game launch does not rerun or replace the extra operation");
    require(invocation.open(QIODevice::ReadOnly) && invocation.readAll() == (g.workingDirectory + '\n' + g.executable + "\ngame-only\n").toUtf8(), "ordinary launch still uses the game executable/arguments"); invocation.close();
    extra.workingDirectory = g.folder;
    require(extraLaunchCommand(lib, g, extra).cwd == g.folder, "extra executable accepts an explicit cwd");
    for (const auto &badPath : {QString("relative.exe"), QString("/missing/patch.exe"), g.folder}) {
        auto bad = extra; bad.executable = badPath; rejects([&] { extraLaunchCommand(lib, g, bad); }, "invalid extra executable rejected");
    }
    auto bad = extra; bad.workingDirectory = "/missing/directory";
    rejects([&] { extraLaunchCommand(lib, g, bad); }, "missing extra cwd rejected");
    bad = extra; bad.arguments = {QString(QChar::Null)};
    rejects([&] { extraLaunchCommand(lib, g, bad); }, "null extra argument rejected");
    bad.arguments = {QString(65537, 'x')}; rejects([&] { extraLaunchCommand(lib, g, bad); }, "oversized extra arguments rejected");
    auto steam = g; steam.backend = "steam";
    rejects([&] { extraLaunchCommand(lib, steam, extra); }, "Steam extra launch rejected");
    QLockFile lock(lib.gamePath(g.id, "launch.lock")); require(lock.tryLock(), "hold extra prefix lock");
    require(supervise(lib, g.id, false, {}, extra) == 2 && readJson(recordPath) == record, "busy extra launch preserves prior operation"); lock.unlock();
    g.environment["EXTRA_EXIT"] = "19"; lib.save(g);
    require(supervise(lib, g.id, false, {}, extra) == 1, "failed extra executable reports failure");
    require(readJson(recordPath)["state"] == "failed" && readJson(recordPath)["exit_code"] == 19 && readJson(recordPath + ".previous") == record, "failed operation retains exit code and previous operation");
    // A live operation may use a patch stored in another game's folder or prefix.
    touch(root + "/Other/game.exe"); lib.import(discover(root), runner);
    Game other; for (const auto &entry : lib.games()) if (entry.folder == root + "/Other") other = entry;
    const auto removalBeforeLaunch = lib.planRemoval(other.id);
    require(!other.id.isEmpty() && lock.tryLock(), "hold active extra operation fixture");
    for (const auto &directory : {other.folder, lib.prefix(other.id), QFileInfo(lib.gamePath(other.id, "launch.lock")).absolutePath()}) {
        auto active = record; active["state"] = "running"; active["executable"] = directory + "/patch.exe"; active["cwd"] = directory;
        writeJson(recordPath, active);
        rejects([&] { lib.planRemoval(other.id); }, "removal protects files referenced by active extra operation");
        rejects([&] { lib.remove(removalBeforeLaunch); }, "stale confirmed removal cannot delete a newly referenced extra path");
    }
    auto corrupt = record; corrupt["state"] = "unknown"; writeJson(recordPath, corrupt);
    rejects([&] { lib.planRemoval(other.id); }, "unreadable active operation state blocks removal");
    lock.unlock(); lib.planRemoval(other.id);
    QFile save(prefix + "/save.dat"); require(save.open(QIODevice::ReadOnly) && save.readAll() == "keep save", "extra orchestration preserves existing save bytes");
}
void testPrefixTools(const QString &base) {
    const auto root = base + "/Prefix tools", runner = base + "/Tool runner", fake = base + "/tool-umu";
    touch(root + "/Game/game.exe"); touch(root + "/Game/winecfg.exe", "must not run"); touch(runner + "/proton");
    touch(fake, R"SH(#!/bin/sh
if [ "$2" = "/c" ]; then
    mkdir -p "$WINEPREFIX/drive_c/windows/system32"
    touch "$WINEPREFIX/system.reg" "$WINEPREFIX/drive_c/windows/system32/winecfg.exe" "$WINEPREFIX/drive_c/windows/regedit.exe"
else
    printf '%s\n' "$@" > "$WINEPREFIX/tool-invocation"
    exit "${TOOL_EXIT:-0}"
fi
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); g.executable = g.folder + "/game.exe"; g.arguments = {"game-only"};
    g.locale = "ja_JP.UTF-8"; g.environment["CUSTOM"] = "kept"; lib.save(g); g = lib.game(g.id);
    const auto saved = g.json(); const auto prefix = lib.prefix(g.id); touch(prefix + "/save.dat", "keep save");
    for (const auto &tool : {QString("winecfg"), QString("regedit")}) {
        const auto cmd = prefixToolCommand(lib, g, tool);
        const auto executable = prefix + "/drive_c/windows/" + (tool == "winecfg" ? "system32/winecfg.exe" : "regedit.exe");
        require(cmd.arguments == QStringList{executable}, "prefix tool uses its own executable without game arguments");
        require(cmd.program == fake && cmd.environment.value("PROTONPATH") == runner &&
                cmd.environment.value("WINEPREFIX") == prefix && cmd.environment.value("HOST_LC_ALL") == g.locale &&
                cmd.environment.value("CUSTOM") == "kept", "prefix tools retain selected backend, runner, prefix and overrides");
        require(supervise(lib, g.id, false, tool) == 0 && setupComplete(lib, g), "tool prepares its prefix before opening");
        QFile invocation(prefix + "/tool-invocation");
        require(invocation.open(QIODevice::ReadOnly) && invocation.readAll() == (executable + '\n').toUtf8(), "supervised tool receives the exact prefix utility path");
        require(lib.game(g.id).json() == saved, "tool opening leaves game configuration unchanged");
    }
    rejects([&] { prefixToolCommand(lib, g, "../../other"); }, "unknown prefix tools refused");
    auto steam = g; steam.backend = "steam";
    rejects([&] { prefixToolCommand(lib, steam, "winecfg"); }, "Steam prefix tools refused");
    QLockFile lock(lib.gamePath(g.id, "launch.lock")); require(lock.tryLock(), "lock tool fixture");
    require(supervise(lib, g.id, false, "winecfg") == 2, "tool cannot bypass a running game's lock"); lock.unlock();
    QFile::remove(prefix + "/drive_c/windows/regedit.exe"); QFile::remove(prefix + "/tool-invocation");
    require(supervise(lib, g.id, false, "regedit") == 1 && !QFileInfo::exists(prefix + "/tool-invocation"), "missing utility does not launch a different executable");
    g.environment["TOOL_EXIT"] = "17"; lib.save(g);
    require(supervise(lib, g.id, false, "winecfg") == 1 &&
        readJson(lib.gamePath(g.id, "status.json"))["message"].toString().contains("winecfg exited with code 17"), "tool failures retain actionable logs");
    QFile save(prefix + "/save.dat"); require(save.open(QIODevice::ReadOnly) && save.readAll() == "keep save", "tool orchestration keeps existing saves");
}
void testSetupRecovery(const QString &base) {
    const auto root = base + "/Setup recovery";
    const auto runner = base + "/Recovery runner";
    const auto fake = base + "/recovery-umu";
    touch(root + "/Game/game.exe"); touch(runner + "/proton"); touch(runner + "/version", "one");
    touch(fake, R"SH(#!/bin/sh
if [ "$2" = "/c" ]; then
    mkdir -p "$WINEPREFIX"
    echo attempt >> "$WINEPREFIX/attempts"
    touch "$WINEPREFIX/system.reg"
    case "$(cat "$WINEPREFIX/mode" 2>/dev/null)" in
        fail) exit 23 ;;
        crash) kill -TERM $$ ;;
        missing) rm "$WINEPREFIX/system.reg" ;;
        runner-change) echo changed > "$PROTONPATH/version" ;;
    esac
else
    touch "$WINEPREFIX/launched"
fi
exit 0
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); const auto id = g.id; const auto prefix = lib.prefix(id);
    const auto statePath = lib.gamePath(id, "setup.json"), recipePath = lib.gamePath(id, "recipe.lock.json");
    auto attempts = [&] { QFile file(prefix + "/attempts"); return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0; };
    auto savesKept = [&] { QFile file(prefix + "/save.dat"); require(file.open(QIODevice::ReadOnly) && file.readAll() == "keep my save", "setup keeps existing save bytes"); };
    // A legacy registry/snapshot without successful completion must be verified in place.
    touch(prefix + "/system.reg"); touch(prefix + "/save.dat", "keep my save");
    writeJson(recipePath, {{"schema", 1}, {"preset", "generic-proton-v1"}});
    require(lib.status(g) == "Needs setup", "legacy prefix is not assumed ready");
    require(supervise(lib, id, false) == 0 && attempts() == 1, "legacy prefix gets preparation before launch");
    require(setupComplete(lib, g) && lib.status(g) == "Ready", "explicit setup completion is ready"); savesKept();
    auto recipe = readJson(recipePath);
    auto legacy = g.json(); legacy.remove("setup_profile"); legacy["engine"] = "Ren'Py";
    const auto oldGame = Game::fromJson(legacy);
    require(oldGame.setupProfile == "generic-proton-v1" && setupComplete(lib, oldGame), "legacy RenPy records retain valid generic preparation");
    g.setupProfile = "renpy-bundled-v1"; lib.save(g);
    require(lib.game(id).setupProfile == g.setupProfile, "profile pin persists");
    require(supervise(lib, id, false) == 0 && attempts() == 1 && readJson(recipePath) == recipe,
        "equivalent profile switch preserves preparation and does not rerun setup");
    g.setupProfile = "generic-proton-v1"; lib.save(g);
    auto unsupported = g; unsupported.setupProfile = "future-v99";
    require(!setupComplete(lib, unsupported), "unknown profile cannot reuse a receipt");
    rejects([&] { launchCommand(lib, unsupported); }, "unknown profile cannot launch");
    rejects([&] { lib.save(unsupported); }, "unknown profile cannot be silently saved as generic");
    bool invoked = false; const auto priorState = readJson(statePath);
    rejects([&] { preparePrefix(lib, unsupported, [&] { invoked = true; return 0; }); }, "unknown preparation rejected");
    require(!invoked && readJson(statePath) == priorState && attempts() == 1, "unknown profile leaves receipt and backend untouched");
    require(Game::fromJson(unsupported.json()).setupProfile == "future-v99", "unknown saved profile remains visible for repair");
    legacy["setup_profile"] = QJsonValue::Null;
    require(!findSetupProfile(Game::fromJson(legacy).setupProfile), "malformed profile is not silently converted to default");
    savesKept();
    g.name = "Changed title"; g.arguments = {"new argument"}; lib.save(g);
    require(supervise(lib, id, false) == 0 && attempts() == 1, "ordinary launch edits do not repeat setup");
    require(readJson(recipePath) == recipe, "ordinary launch preserves setup snapshot");
    require(supervise(lib, id, true) == 0 && attempts() == 2, "explicit Prepare reruns completed setup in place");
    require(readJson(recipePath)["attempt"] != recipe["attempt"], "explicit repair records a new completion"); savesKept();
    recipe = readJson(recipePath);
    touch(prefix + "/mode", "fail"); QFile::remove(prefix + "/launched");
    require(supervise(lib, id, true) == 1, "nonzero preparation fails after registry appears");
    require(QFileInfo::exists(prefix + "/system.reg") && !setupComplete(lib, g), "existing registry cannot mask failed setup");
    require(readJson(statePath)["state"] == "failed" && lib.status(g) == "Needs attention", "failure step persisted");
    require(readJson(lib.gamePath(id, "status.json"))["message"].toString().contains("23"), "failure exposes backend exit code");
    require(readJson(recipePath) == recipe, "failed repair keeps last successful recipe snapshot");
    auto count = attempts();
    require(supervise(lib, id, false) == 1 && attempts() == count + 1 && !QFileInfo::exists(prefix + "/launched"), "Play retries failed preparation and blocks game on failure");
    touch(prefix + "/mode");
    require(supervise(lib, id, false) == 0 && setupComplete(lib, g), "Play recovers failed setup in place"); savesKept();
    touch(prefix + "/mode", "crash");
    require(supervise(lib, id, true) == 1 && !setupComplete(lib, g), "crashed backend never marks setup complete");
    touch(prefix + "/mode", "missing");
    require(supervise(lib, id, true) == 1 && !setupComplete(lib, g), "zero exit without registry fails validation");
    touch(prefix + "/mode");
    require(supervise(lib, id, false) == 0, "retry repairs missing registry"); savesKept();
    // Reproduce durable state at interruption after registry creation, then reopen the library.
    auto state = readJson(statePath); state["state"] = "running"; state["step"] = "prefix-init";
    writeJson(statePath, state); writeJson(lib.gamePath(id, "status.json"), {{"state", "Preparing"}});
    count = attempts();
    {
        Library reopened(root);
        require(reopened.status(g) == "Interrupted", "interrupted setup survives restart");
        require(supervise(reopened, id, false) == 0 && attempts() == count + 1, "retry reinitialises partial prefix despite registry");
    }
    savesKept();
    state = readJson(statePath); state["attempt"] = "different-attempt"; writeJson(statePath, state);
    require(!setupComplete(lib, g) && supervise(lib, id, false) == 0, "mismatched completion records require setup");
    touch(statePath, "broken JSON");
    require(lib.status(g) == "Needs setup" && supervise(lib, id, false) == 0, "corrupt completion receipt can be repaired");
    touch(runner + "/version", "two"); lib.save(g); g = lib.game(id);
    require(lib.status(g) == "Needs setup", "accepted runner change invalidates preparation");
    require(supervise(lib, id, false) == 0 && readJson(recipePath)["inputs"].toObject()["runner_version"] == "two", "runner change prepares and records new version");
    g.locale = "ja_JP.UTF-8"; lib.save(g);
    require(!setupComplete(lib, g) && supervise(lib, id, false) == 0, "locale change receives setup verification");
    g.environment["CUSTOM"] = "changed"; lib.save(g);
    require(!setupComplete(lib, g) && supervise(lib, id, false) == 0, "environment change receives setup verification");
    touch(prefix + "/mode", "runner-change");
    require(supervise(lib, id, true) == 1 && !setupComplete(lib, g), "runner changed during preparation cannot receive completion");
    touch(prefix + "/mode"); lib.save(g); g = lib.game(id);
    require(supervise(lib, id, false) == 0, "reviewed runner can recover mid-setup change");
    const auto held = prefix + "-original";
    require(QDir().rename(prefix, held), "hold completed prefix fixture"); touch(prefix + "/system.reg");
    if (!folderIdentity(prefix).isEmpty()) require(!setupComplete(lib, g), "replacement prefix is not covered by old completion");
    require(supervise(lib, id, false) == 0, "replacement prefix can be prepared");
    QFile save(held + "/save.dat"); require(save.open(QIODevice::ReadOnly) && save.readAll() == "keep my save", "held original prefix saves untouched");
}
void testRecipeWinetricks(const QString &base) {
    const auto root = base + "/Recipe Winetricks";
    const auto runner = base + "/GE-Proton11-7";
    const auto newerRunner = base + "/GE-Proton11-8";
    const auto fake = base + "/winetricks-umu";
    touch(root + "/Game/game.exe"); touch(runner + "/proton"); touch(runner + "/version", "11-7");
    touch(newerRunner + "/proton"); touch(newerRunner + "/version", "11-8");
    touch(runner + "/protonfixes/winetricks", "#!/bin/sh\nexit 0\n");
    touch(newerRunner + "/protonfixes/winetricks", "#!/bin/sh\nexit 0\n");
    QFile::setPermissions(runner + "/protonfixes/winetricks", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(newerRunner + "/protonfixes/winetricks", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    touch(fake, R"SH(#!/bin/sh
if [ "$1" = "winetricks" ]; then
    if [ -e "$WINEPREFIX/fail-$2" ]; then echo 'download failed' >&2; exit 23; fi
    echo "$2:$PROTONPATH:$WINEPREFIX" >> "$WINEPREFIX/winetricks-calls"
    exit 0
fi
if [ "$2" = "/c" ]; then mkdir -p "$WINEPREFIX"; touch "$WINEPREFIX/system.reg"; exit 0; fi
touch "$WINEPREFIX/played"; exit 0
)SH");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"umu", fake}, {"runner", runner}});
    const auto recipes = loadRecipes(root);
    for (const auto &recipe : recipes) if (recipe.tier == "general")
        require(recipe.settings["winetricks"].toArray().isEmpty() &&
                recipe.settings["runner"] == "GE-Proton11-7", "general recipes start clean with the tested GE pin");
    lib.import(discover(root), runner);
    auto g = lib.games().first(); const auto id = g.id; const auto prefix = lib.prefix(id);
    require(g.runner == runner && g.runnerSelector == "GE-Proton11-7",
            "a valid library runner takes precedence over the general recipe recommendation");
    require(pendingWinetricks(lib, g).isEmpty() && supervise(lib, id, false) == 0 &&
            !QFileInfo::exists(prefix + "/winetricks-calls"), "clean general game never runs Winetricks");
    auto legacyReceipt = readJson(lib.gamePath(id, "recipe.lock.json"));
    legacyReceipt.remove("winetricks_requested");
    writeJson(lib.gamePath(id, "recipe.lock.json"), legacyReceipt);
    require(setupComplete(lib, g) && pendingWinetricks(lib, g).isEmpty(),
            "existing schema 2 receipts without component fields remain valid");
    lib.setSettings({{"umu", fake}, {"default_recipe", "generic-vn-v1"}});
    lib.applyRecipe(id, "generic-vn-v1"); g = lib.game(id); g.runner = runner; lib.save(g); g = lib.game(id);
    require(g.winetricks.isEmpty() && supervise(lib, id, true) == 0 &&
            !QFileInfo::exists(prefix + "/winetricks-calls"), "clean General VN Prepare has no component install");
    touch(prefix + "/save.dat", "keep save");
    const auto parent = QJsonObject{{"schema", 1}, {"id", "media-base"}, {"name", "Media base"},
        {"inherits", "generic-game-v1"}, {"match", QJsonObject{}},
        {"settings", QJsonObject{{"winetricks", QJsonArray{"quartz", "wmp11", "quartz"}},
            {"environment", QJsonObject{{"TZ", "Asia/Tokyo"}}}}}};
    saveRecipe(root, "default", QJsonDocument(parent).toJson());
    const auto writeGame = [&](const QJsonArray &verbs) {
        saveRecipe(root, "games", QJsonDocument(QJsonObject{{"schema", 1}, {"id", "media-game"},
            {"name", "Media game"}, {"inherits", "media-base"},
            {"match", QJsonObject{{"all", QJsonArray{QJsonObject{{"file", "game.exe"}}}}}},
            {"settings", QJsonObject{{"winetricks", verbs},
                {"environment", QJsonObject{{"CUSTOM", "value"}}}}}}).toJson());
    };
    writeGame(QJsonArray{"wmp11", "qasf"});
    auto effective = loadRecipes(root);
    for (const auto &recipe : effective) if (recipe.id == "media-game") {
        require(recipe.settings["winetricks"].toArray() == QJsonArray({"quartz", "wmp11", "qasf"}) &&
                recipe.settings["environment"].toObject()["TZ"] == "Asia/Tokyo" &&
                recipe.settings["environment"].toObject()["CUSTOM"] == "value",
                "inheritance merges environment and deduplicates components in parent-first order");
    }
    auto invalid = parent; invalid["id"] = "bad-verb";
    invalid["settings"] = QJsonObject{{"winetricks", QJsonArray{"quartz; rm -rf /"}}};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "recipe cannot inject a command");
    invalid["settings"] = QJsonObject{{"winetricks", QJsonArray{"-q"}}};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "recipe cannot pass Winetricks options");
    invalid["settings"] = QJsonObject{{"winetricks", QJsonArray{"quartz2"}}};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "recipe cannot request a verb absent from the bundled version");
    invalid["settings"] = QJsonObject{{"command", "sh -c true"}};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "recipe cannot add an executable command field");
    invalid["settings"] = QJsonObject{{"winetricks", QJsonArray{QJsonObject{{"url", "https://example.invalid/install.exe"}}}}};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "recipe cannot add an installer URL in a component entry");
    invalid["id"] = "missing-parent"; invalid["inherits"] = "absent-recipe"; invalid["settings"] = QJsonObject{};
    rejects([&] { saveRecipe(root, "default", QJsonDocument(invalid).toJson()); }, "editor refuses missing parent");
    touch(recipePath(root, "default", "missing-parent"), QJsonDocument(invalid).toJson());
    rejects([&] { loadRecipes(root); }, "missing recipe parent is rejected");
    QFile::remove(recipePath(root, "default", "missing-parent"));
    auto cycleA = invalid, cycleB = invalid;
    cycleA["id"] = "cycle-a"; cycleA["inherits"] = "cycle-b";
    cycleB["id"] = "cycle-b"; cycleB["inherits"] = "cycle-a";
    touch(recipePath(root, "default", "cycle-a"), QJsonDocument(cycleA).toJson());
    touch(recipePath(root, "default", "cycle-b"), QJsonDocument(cycleB).toJson());
    rejects([&] { loadRecipes(root); }, "recipe inheritance cycle is rejected");
    QFile::remove(recipePath(root, "default", "cycle-a")); QFile::remove(recipePath(root, "default", "cycle-b"));
    lib.applyRecipe(id, "media-game"); g = lib.game(id); g.runner = runner; lib.save(g); g = lib.game(id);
    require(g.winetricks == QStringList({"quartz", "wmp11", "qasf"}), "resolved verbs persist with the game");
    require(supervise(lib, id, false) == 1 && !QFileInfo::exists(prefix + "/winetricks-calls") &&
            contents(prefix + "/save.dat") == "keep save", "unapproved headless Play leaves prefix and save unchanged");
    auto approval = winetricksApproval(lib, g);
    QFile::remove(runner + "/protonfixes/winetricks");
    require(supervise(lib, id, false, {}, {}, false, false, approval) == 1 &&
            pendingWinetricks(lib, g) == QStringList({"quartz", "wmp11", "qasf"}) &&
            contents(lib.gamePath(id, "status.json")).contains("does not include executable Winetricks integration"),
            "missing bundled Winetricks integration is reported without a success receipt");
    touch(runner + "/protonfixes/winetricks", "#!/bin/sh\nexit 0\n");
    QFile::setPermissions(runner + "/protonfixes/winetricks", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    require(supervise(lib, id, false, {}, {}, false, false, approval) == 0 &&
            readJson(lib.gamePath(id, "recipe.lock.json"))["winetricks_applied"].toArray().size() == 3,
            "approved Play applies and records each requested component");
    const auto calls = contents(prefix + "/winetricks-calls");
    require(calls.contains("quartz:" + runner.toUtf8()) && calls.contains("qasf:" + runner.toUtf8()) &&
            calls.contains(prefix.toUtf8()), "Winetricks uses this game's runner and prefix");
    require(supervise(lib, id, false) == 0 && contents(prefix + "/winetricks-calls") == calls,
            "subsequent Play skips completed components");
    require(supervise(lib, id, true) == 0 && contents(prefix + "/winetricks-calls") == calls,
            "explicit Prepare does not reinstall completed components");
    writeGame(QJsonArray{"wmp11", "qasf", "corefonts", "cjkfonts"});
    lib.applyRecipe(id, "media-game"); g = lib.game(id); g.runner = runner; lib.save(g); g = lib.game(id);
    touch(prefix + "/fail-cjkfonts"); approval = winetricksApproval(lib, g);
    require(supervise(lib, id, false, {}, {}, false, false, approval) == 1 &&
            pendingWinetricks(lib, g) == QStringList{"cjkfonts"} &&
            contents(prefix + "/winetricks-calls").contains("corefonts:" + runner.toUtf8()) &&
            readJson(lib.gamePath(id, "setup.json"))["winetricks_state"] == "failed" &&
            contents(lib.gamePath(id, "launch.log")).contains("download failed"),
            "partial failure records completed components and leaves only the failed one pending");
    QFile::remove(prefix + "/fail-cjkfonts");
    require(supervise(lib, id, false, {}, {}, false, false, winetricksApproval(lib, g)) == 0 &&
            pendingWinetricks(lib, g).isEmpty(), "retry installs only the failed component");
    g.runner = newerRunner; lib.save(g); g = lib.game(id);
    require(!setupComplete(lib, g) && pendingWinetricks(lib, g).isEmpty() && supervise(lib, id, false) == 0 &&
            contents(prefix + "/save.dat") == "keep save", "runner change rechecks prefix without erasing saved data or reinstalling verbs");
    writeGame(QJsonArray{});
    lib.applyRecipe(id, "media-game"); g = lib.game(id); g.runner = newerRunner; lib.save(g); g = lib.game(id);
    require(pendingWinetricks(lib, g).isEmpty() &&
            readJson(lib.gamePath(id, "recipe.lock.json"))["winetricks_applied"].toArray().size() == 5,
            "removing requested components does not erase their applied record or prefix");
    writeGame(QJsonArray{"wmp11", "qasf", "cjkfonts", "corefonts"});
    lib.applyRecipe(id, "media-game"); g = lib.game(id); g.runner = newerRunner; lib.save(g); g = lib.game(id);
    auto interruptedReceipt = readJson(lib.gamePath(id, "recipe.lock.json"));
    auto interruptedApplied = interruptedReceipt["winetricks_applied"].toArray();
    interruptedApplied.removeAt(interruptedApplied.size() - 1);
    interruptedReceipt["winetricks_applied"] = interruptedApplied;
    writeJson(lib.gamePath(id, "recipe.lock.json"), interruptedReceipt);
    auto state = readJson(lib.gamePath(id, "setup.json")); state["winetricks_state"] = "running";
    writeJson(lib.gamePath(id, "setup.json"), state);
    writeJson(lib.gamePath(id, "status.json"), {{"state", "Preparing"}});
    require(lib.status(g) == "Interrupted" && pendingWinetricks(lib, g) == QStringList{"cjkfonts"} &&
            supervise(lib, id, false, {}, {}, false, false, winetricksApproval(lib, g)) == 0 &&
            pendingWinetricks(lib, g).isEmpty(), "interrupted component preparation remains visible and can retry");
}
void testSteamRemovalGuard(const QString &base, const QString &runner) {
    const auto root = base + "/Steam removal guard"; touch(root + "/Game/game.exe");
    const auto fake = base + "/fake-steam"; touch(fake, "#!/bin/sh\nexec touch steam-received\n");
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Library lib(root); lib.setSettings({{"steam", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); auto plan = lib.planRemoval(g.id);
    g.backend = "steam"; g.steamId = "123"; lib.save(g);
    require(!lib.removalIssue(g).isEmpty(), "Steam removal has an actionable explanation");
    rejects([&] { lib.planRemoval(g.id); }, "Steam removal blocked without lifecycle tracking");
    plan.original = lib.game(g.id);
    rejects([&] { lib.remove(plan); }, "Steam removal guard enforced during apply");
    require(supervise(lib, g.id, false) == 0, "fake Steam request dispatched");
    QElapsedTimer timeout; timeout.start();
    while (!QFileInfo::exists(g.folder + "/steam-received") && timeout.elapsed() < 2000) QThread::msleep(10);
    require(QFileInfo::exists(g.folder + "/steam-received"), "fake Steam receives request before fixture cleanup");
    require(QFileInfo::exists(lib.gamePath(g.id, "steam-request.json")), "untracked Steam request persisted");
    g.backend = "umu"; lib.save(g); writeJson(lib.gamePath(g.id, "status.json"), {{"state", "Ready"}});
    {
        Library reopened(root);
        rejects([&] { reopened.planRemoval(g.id); }, "backend switch and restart cannot bypass untracked request");
    }
    QFile::remove(lib.gamePath(g.id, "steam-request.json")); // Simulate pre-feature request state.
    writeJson(lib.gamePath(g.id, "status.json"), {{"state", "Requested in Steam"}});
    lib.save(g); writeJson(lib.gamePath(g.id, "status.json"), {{"state", "Ready"}});
    rejects([&] { lib.planRemoval(g.id); }, "legacy Steam request remains protected across edits");
    QFile::remove(lib.gamePath(g.id, "steam-request.json"));
    writeJson(lib.gamePath(g.id, "status.json"), {{"state", "Requested in Steam"}});
    lib.recordStatus(g.id, "Starting");
    rejects([&] { lib.planRemoval(g.id); }, "legacy request survives launch status replacement");
    require(QFileInfo::exists(g.executable) && lib.games().size() == 1, "Steam guard preserves files and record");
}
void testRemoval(const QString &base, const QString &runner) {
    const auto root = base + "/Removal library";
    touch(root + "/Game/game.exe"); touch(root + "/Other/game.exe");
    Library lib(root); lib.import(discover(root), runner);
    auto g = lib.games().first(); const auto other = lib.games().last();
    touch(lib.prefix(g.id) + "/save.dat", "remove this save");
    touch(lib.gamePath(g.id, "launch.log"), "remove logs");
    const auto external = base + "/Outside removal"; touch(external + "/keep.dat", "keep unrelated data");
    require(QFile::link(external, g.folder + "/linked-assets"), "directory symlink fixture created");
    auto plan = lib.planRemoval(g.id);
    require(plan.paths.contains(g.folder) && plan.paths.contains(lib.prefix(g.id)), "removal plan explicitly lists game and prefix");
    require(QFileInfo::exists(g.folder + "/game.exe") && QFileInfo::exists(lib.prefix(g.id) + "/save.dat"), "planning alone does not delete data");
    {
        QLockFile lock(lib.gamePath(g.id, "launch.lock")); require(lock.tryLock(), "removal busy fixture locked");
        rejects([&] { lib.planRemoval(g.id); }, "busy removal preview refused");
        rejects([&] { lib.remove(plan); }, "busy game not removed");
    }
    g.name = "Changed name"; lib.save(g);
    rejects([&] { lib.remove(plan); }, "stale removal settings refused");
    require(QFileInfo::exists(lib.prefix(g.id) + "/save.dat"), "stale removal keeps saves");
    g = lib.game(g.id); plan = lib.planRemoval(g.id);
    const auto held = root + "/Original held";
    require(QDir().rename(g.folder, held), "removal original moved"); touch(g.folder + "/replacement.exe");
    rejects([&] { lib.remove(plan); }, "replaced folder not deleted");
    require(QFileInfo::exists(g.folder + "/replacement.exe") && QFileInfo::exists(held + "/game.exe"), "both original and replacement kept");
    require(QDir(g.folder).removeRecursively() && QDir().rename(held, g.folder), "original fixture restored");
    Game child; child.name = "Shared child"; child.folder = g.folder + "/Nested"; child.executable = child.folder + "/child.exe"; child.workingDirectory = child.folder;
    touch(child.executable); lib.save(child);
    rejects([&] { lib.planRemoval(g.id); }, "folder shared by nested game refused");
    // Remove only the nested fixture entry to continue testing the parent.
    auto nested = lib.games(); for (const auto &item : nested) if (item.name == "Shared child") {
        rejects([&] { lib.planRemoval(item.id); }, "child inside another game cannot be fully removed");
    }
    // Use a separate library for destructive success so shared-folder records stay protected.
    const auto cleanRoot = base + "/Removal success"; touch(cleanRoot + "/Game/game.exe"); touch(cleanRoot + "/Keep/game.exe");
    Library clean(cleanRoot); clean.import(discover(cleanRoot), runner); const auto target = clean.games().first();
    touch(clean.prefix(target.id) + "/save.dat", "delete"); touch(clean.gamePath(target.id, "launch.log"), "delete");
    require(QFile::link(external, target.folder + "/linked-assets"), "success symlink fixture created");
    {
        // Failure after filesystem staging must restore the original names and database entry.
        auto connection = QSqlDatabase::addDatabase("QSQLITE", "removal-rollback-fixture");
        connection.setDatabaseName(clean.dataRoot() + "/library.sqlite"); require(connection.open(), "removal rollback database opened");
        {
            QSqlQuery q(connection); require(q.exec("CREATE TRIGGER reject_removal BEFORE DELETE ON games BEGIN SELECT RAISE(ABORT, 'fixture rejection'); END"), "removal rejection trigger installed");
            rejects([&] { clean.remove(clean.planRemoval(target.id)); }, "failed database removal rolls back staging");
            require(QFileInfo::exists(target.folder + "/game.exe") && QFileInfo::exists(clean.prefix(target.id) + "/save.dat") && clean.games().size() == 2, "rollback keeps game saves and entry");
            require(q.exec("DROP TRIGGER reject_removal"), "rejection trigger removed");
        }
        connection.close(); connection = QSqlDatabase(); QSqlDatabase::removeDatabase("removal-rollback-fixture");
    }
    const auto staleScan = discover(cleanRoot);
    auto result = clean.remove(clean.planRemoval(target.id));
    require(result.cleanupPaths.isEmpty(), "full cleanup succeeds");
    require(!QFileInfo::exists(target.folder) && !QFileInfo::exists(clean.prefix(target.id)) && !QFileInfo::exists(QFileInfo(clean.gamePath(target.id, "launch.log")).absolutePath()), "game prefix and state removed");
    require(QFileInfo::exists(external + "/keep.dat"), "recursive removal does not follow directory symlink");
    require(clean.games().size() == 1 && QFileInfo::exists(cleanRoot + "/Keep/game.exe"), "other game and library retained");
    rejects([&] { clean.game(target.id); }, "removed entry no longer exists");
    rejects([&] { clean.save(target); }, "stale Manage settings cannot resurrect removed game");
    require(clean.import(staleScan, runner) == 0, "stale discovery snapshot cannot resurrect removed folder");
    require(clean.import(discover(cleanRoot), runner) == 0, "removed folder is not rediscovered");
    Game broad; broad.name = "Broad folder"; broad.folder = cleanRoot; broad.executable = cleanRoot + "/broad.exe"; broad.workingDirectory = cleanRoot; clean.save(broad);
    for (const auto &item : clean.games()) if (item.name == broad.name) rejects([&] { clean.planRemoval(item.id); }, "main Games directory protected");
    const auto missingRoot = base + "/Removal missing"; touch(missingRoot + "/Missing/game.exe");
    Library missing(missingRoot); missing.import(discover(missingRoot), runner); const auto absent = missing.games().first();
    touch(missing.prefix(absent.id) + "/save.dat"); QDir(absent.folder).removeRecursively();
    require(missing.remove(missing.planRemoval(absent.id)).cleanupPaths.isEmpty() && missing.games().isEmpty(), "missing game can remove leftover owned data");
    const auto linkedRoot = base + "/Removal linked prefix"; touch(linkedRoot + "/Game/game.exe");
    Library linked(linkedRoot); linked.import(discover(linkedRoot), runner); const auto linkedGame = linked.games().first();
    QDir().mkpath(QFileInfo(linked.prefix(linkedGame.id)).absolutePath()); require(QFile::link(external, linked.prefix(linkedGame.id)), "linked prefix fixture created");
    rejects([&] { linked.planRemoval(linkedGame.id); }, "linked external prefix refused");
    require(QFileInfo::exists(external + "/keep.dat"), "external prefix data protected");
}
void testRelinking(const QString &base, const QString &runner, const QString &fake) {
    const auto root = base + "/Relink library";
    const auto folder = root + "/Original";
    touch(folder + "/bin/game.exe");
    Library lib(root); lib.setSettings({{"umu", fake}}); lib.import(discover(root), runner);
    auto g = lib.games().first(); const auto id = g.id;
    g.name = "Custom name"; g.arguments = {"argument with spaces"}; g.environment["CUSTOM"] = "keep"; lib.save(g); g = lib.game(id);
    require(supervise(lib, id, true) == 0, "relink fixture prefix prepared");
    const auto prefix = lib.prefix(id); touch(prefix + "/save.dat", "keep my save");
    const auto revision = g.runnerRevision; touch(runner + "/version", "changed-during-relink\n");
    {
        auto invalid = g;
        invalid.executable = root + "/Template!!?/game.exe";
        require(launchPathIssue(invalid).contains("Template!!?"), "reserved path component is named in error");
        rejects([&] { launchCommand(lib, invalid); }, "invalid Windows executable path is rejected before umu");
        invalid.executable = g.executable; invalid.workingDirectory = root + "/invalid*";
        require(!launchPathIssue(invalid).isEmpty(), "working directory is checked");
        invalid.workingDirectory = root + "/CON.txt";
        require(!launchPathIssue(invalid).isEmpty(), "reserved device names are checked");
        invalid.workingDirectory = root + "/trailing.";
        require(!launchPathIssue(invalid).isEmpty(), "trailing dot is checked");
        invalid.workingDirectory = root + "/trailing ";
        require(!launchPathIssue(invalid).isEmpty(), "trailing space is checked");
        invalid.executable = root + "/Good! (日本語) [1]/game.exe"; invalid.workingDirectory = root + "/Good! (日本語) [1]";
        require(launchPathIssue(invalid).isEmpty(), "spaces, Japanese and ordinary punctuation remain supported");
        const auto validationRoot = base + "/Path validation library";
        touch(validationRoot + "/Template!!?/game.exe");
        Library validation(validationRoot); validation.setSettings({{"umu", fake}});
        validation.import(discover(validationRoot), runner); const auto blocked = validation.games().first();
        writeJson(validation.gamePath(blocked.id, "status.json"), {{"state", "Needs attention"}, {"message", "old generic failure"}});
        require(validation.status(blocked) == "Path needs rename", "path diagnostic takes priority over generic failure");
        require(supervise(validation, blocked.id, false) == 1, "invalid path fails without invoking backend");
        require(!QFileInfo::exists(validation.prefix(blocked.id)), "invalid path does not allocate a prefix");
        require(readJson(validation.gamePath(blocked.id, "status.json"))["message"].toString().contains("Template!!?"), "actionable failure survives reopening");
    }
    {
        auto legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy-fixture");
        legacy.setDatabaseName(lib.dataRoot() + "/library.sqlite"); require(legacy.open(), "legacy fixture opened");
        { QSqlQuery q(legacy); require(q.exec("UPDATE games SET config=json_remove(config, '$.folder_identity', '$.source')"), "pre-feature configuration simulated"); }
        legacy.close(); legacy = QSqlDatabase(); QSqlDatabase::removeDatabase("legacy-fixture");
    }
    require(lib.game(id).folderIdentity.isEmpty(), "legacy record has no folder identity");
    require(lib.reconcileFolders(scanDiscovery(root, {folder})) == 0, "legacy identity migration is not a move");
    g = lib.game(id);
    require(g.runnerRevision == revision, "legacy migration does not accept runner update");
    require(g.folderIdentity == folderIdentity(folder), "legacy identity learned while folder exists");
    const auto renamed = root + "/Renamed 日本語";
    require(QDir().rename(folder, renamed), "fixture folder renamed");
    auto snapshot = scanDiscovery(root, {folder});
    if (!g.folderIdentity.isEmpty()) {
        require(lib.reconcileFolders(snapshot) == 1, "same directory identity relinks automatically");
        require(folderIdentity(renamed) == g.folderIdentity, "identity survives rename");
    } else lib.relink(lib.planRelink(id, renamed));
    g = lib.game(id);
    require(g.folder == renamed && g.executable == renamed + "/bin/game.exe", "automatic relink remaps executable");
    require(g.workingDirectory == renamed + "/bin", "automatic relink remaps working directory");
    require(g.name == "Custom name" && g.arguments.size() == 1 && g.environment["CUSTOM"] == "keep", "relink preserves customization");
    require(g.runnerRevision == revision && lib.status(g) == "Runner changed", "relink does not accept runner update");
    require(lib.import(snapshot.candidates, runner) == 0, "renamed directory does not duplicate entry");
    require(lib.prefix(id) == prefix && QFileInfo::exists(prefix + "/save.dat"), "relink keeps prefix and save data");

    const auto copied = root + "/Copied"; touch(copied + "/bin/game.exe");
    require(lib.import({inspect(copied)}, runner) == 1, "copy has its own installation");
    require(folderIdentity(copied).isEmpty() || folderIdentity(copied) != g.folderIdentity, "copy is not treated as rename");
    auto plan = lib.planRelink(id, copied);
    require(!plan.duplicateId.isEmpty(), "unused discovered duplicate shown in plan");
    {
        QLockFile lock(lib.gamePath(id, "launch.lock")); require(lock.tryLock(), "relink fixture lock acquired");
        rejects([&] { lib.relink(plan); }, "relink blocked while source is busy");
    }
    {
        const auto duplicateLockPath = lib.gamePath(plan.duplicateId, "launch.lock");
        QDir().mkpath(QFileInfo(duplicateLockPath).absolutePath());
        QLockFile lock(duplicateLockPath); require(lock.tryLock(), "destination relink lock acquired");
        rejects([&] { lib.relink(plan); }, "relink blocked while destination is busy");
    }
    auto duplicate = lib.game(plan.duplicateId); duplicate.name = "Configured copy"; lib.save(duplicate);
    rejects([&] { lib.relink(plan); }, "stale duplicate plan rejected");
    require(lib.games().size() == 2 && lib.game(id).folder == renamed, "failed merge preserves both records");
    rejects([&] { lib.planRelink(id, copied); }, "configured duplicate cannot be replaced");

    const auto destination = root + "/Transferred"; touch(destination + "/bin/game.exe");
    lib.import({inspect(destination)}, runner); plan = lib.planRelink(id, destination);
    const auto duplicateId = plan.duplicateId;
    require(QDir(renamed).removeRecursively(), "fixture old folder removed after transfer");
    lib.relink(plan); g = lib.game(id);
    require(g.folder == destination && g.executable == destination + "/bin/game.exe", "manual relink maps paths");
    require(lib.games().size() == 2, "manual relink consolidates unused discovered duplicate");
    rejects([&] { lib.game(duplicateId); }, "unused duplicate record removed");
    require(lib.game(duplicate.id).name == "Configured copy", "configured sibling is kept");
    require(g.runnerRevision == revision, "manual relink keeps runner acceptance");
    QFile save(prefix + "/save.dat"); require(save.open(QIODevice::ReadOnly) && save.readAll() == "keep my save", "prefix save contents unchanged");

    const auto prepared = root + "/Prepared copy"; touch(prepared + "/bin/game.exe");
    lib.import({inspect(prepared)}, runner);
    Game preparedGame; for (const auto &item : lib.games()) if (item.folder == prepared) preparedGame = item;
    QDir().mkpath(lib.prefix(preparedGame.id));
    rejects([&] { lib.planRelink(id, prepared); }, "prepared duplicate cannot be replaced");
    const auto missingExe = root + "/Changed layout"; QDir().mkpath(missingExe);
    plan = lib.planRelink(id, missingExe);
    rejects([&] { lib.relink(plan); }, "missing executable cannot commit a relink");
    require(lib.game(id).folder == destination, "failed relink keeps previous paths");
    touch(missingExe + "/new.exe");
    const auto outside = base + "/External working directory"; QDir().mkpath(outside);
    g.workingDirectory = outside; lib.save(g); g = lib.game(id);
    plan = lib.planRelink(id, missingExe, missingExe + "/new.exe");
    require(plan.updated.workingDirectory == outside, "external working directory is preserved");
    lib.relink(plan);
    require(lib.game(id).executable == missingExe + "/new.exe", "changed layout accepts selected executable");
    auto stale = lib.planRelink(id, destination); g = lib.game(id); g.arguments.append("newer edit"); lib.save(g);
    rejects([&] { lib.relink(stale); }, "concurrent settings edits cannot be overwritten");
    require(lib.game(id).arguments.size() == 2, "concurrent settings are retained");
    g = lib.game(id); g.folderIdentity = "another-directory";
    require(lib.status(g) == "Folder changed", "replaced directory is visible");
    rejects([&] { launchCommand(lib, g); }, "replaced directory cannot use old prefix implicitly");
    g = lib.game(id);
    if (!g.folderIdentity.isEmpty()) {
        const auto replacement = root + "/Actual original";
        require(QDir().rename(missingExe, replacement), "original moved before replacement");
        touch(missingExe + "/new.exe");
        require(lib.status(g) == "Folder changed", "reused folder name does not reuse installation identity");
        rejects([&] { launchCommand(lib, g); }, "reused folder name cannot launch with previous prefix");
        require(lib.reconcileFolders(scanDiscovery(root, {g.folder, copied, prepared})) == 1, "original directory is found beside replacement");
        require(lib.game(id).folder == replacement, "original identity follows original folder");
    }
    g = lib.game(id); g.backend = "steam"; lib.save(g);
    rejects([&] { lib.planRelink(id, destination); }, "Steam shortcut paths not silently relinked");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "temporary directory");
        auto root = temp.path() + "/Games with spaces 日本語"; QDir().mkpath(root);
        const auto folder = root + "/Template!!";
        touch(folder + "/Tenpure.exe"); touch(folder + "/unins000.exe"); touch(folder + "/game/code.xp3.rpa");
        touch(folder + "/renpy/__init__.py"); touch(folder + "/lib/python.exe");
        touch(root + "/.novelty/ignore/secret.exe");
        auto found = discover(root);
        for (const auto &engine : {"Generic", "Unknown"})
            require(defaultSetupProfile(engine) == "generic-game-v1", "unidentified games use the general-game fallback");
        for (const auto &engine : {"Kirikiri candidate", "Siglus candidate"})
            require(defaultSetupProfile(engine) == "generic-game-v1", "unverified engines use the general fallback");
        require(preferredProtonVersion("generic-proton-v1").isEmpty() &&
                preferredProtonVersion("generic-game-v1").isEmpty() &&
                preferredProtonVersion("renpy-bundled-v1").isEmpty(),
                "setup profiles do not pin a runner version");
        require(found.size() == 1, "ignore managed folder");
        require(found[0].engine == "Ren'Py", "RPA must not be misidentified as XP3");
        require(found[0].executables == QStringList{folder + "/Tenpure.exe"}, "exclude helpers/uninstallers");
        touch(root + "/Ambiguous/game.exe"); touch(root + "/Ambiguous/config.exe");
        found = discover(root); require(found.size() == 2, "discover second game");
        {
            // Real file metadata, deterministic elapsed time: no slow timer tests here.
            auto snapshot = scanDiscovery(root, {folder});
            require(snapshot.candidates.size() == 1, "known games excluded from copy checks");
            require(snapshot.watchDirectories.contains(root + "/Ambiguous"), "new folders watched");
            CopyTracker tracker;
            require(tracker.observe(snapshot.candidates, 0).isEmpty(), "first observation cannot import a copy");
            require(tracker.observe(snapshot.candidates, 3999).isEmpty(), "quiet interval required");
            touch(root + "/Ambiguous/assets/nested/data.bin", "first chunk");
            auto changed = scanDiscovery(root, {folder});
            require(changed.candidates[0].fingerprint != snapshot.candidates[0].fingerprint, "nested assets affect snapshot");
            require(tracker.observe(changed.candidates, 4000).isEmpty(), "new asset resets quiet time");
            touch(root + "/Ambiguous/assets/nested/data.bin", "larger second chunk");
            auto growing = scanDiscovery(root, {folder});
            require(growing.candidates[0].fingerprint != changed.candidates[0].fingerprint, "growing files affect snapshot");
            require(tracker.observe(growing.candidates, 8000).isEmpty(), "file growth resets quiet time");
            require(tracker.observe(growing.candidates, 11999).isEmpty(), "changed copy still waits");
            require(tracker.observe(growing.candidates, 12000).size() == 1, "quiet folder becomes importable");
            require(tracker.waiting() == 0, "no polling required once settled");
            tracker.observe(snapshot.candidates, 13000);
            require(tracker.observe({}, 14000).isEmpty() && tracker.waiting() == 0, "removed copy is forgotten");
            QDir().mkpath(root + "/Empty/nested");
            auto empty = scanDiscovery(root, {folder, root + "/Ambiguous"});
            require(empty.candidates.isEmpty(), "folders without launchers are not imported");
            require(empty.watchDirectories.contains(root + "/Empty/nested"), "empty nested directories watched for later executable");
            QDir(root + "/Empty").removeRecursively();
        }
        auto runner = temp.path() + "/Proton Test"; touch(runner + "/proton"); touch(runner + "/version", "test-1\n");
        auto fake = temp.path() + "/fake umu";
        touch(fake, "#!/bin/sh\nif [ \"$2\" = \"/c\" ]; then mkdir -p \"$WINEPREFIX\"; touch \"$WINEPREFIX/system.reg\"; exit 0; fi\nprintf '%s\\n' \"$PWD\" \"$PROTONPATH\" \"$WINEPREFIX\" \"$HOST_LC_ALL\" \"$@\" > \"$WINEPREFIX/invocation\"\nexit 0\n");
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QString id;
        {
            Library lib(root); lib.setSettings({{"umu", fake}, {"backend", "umu"}});
            require(lib.import(found, runner) == 2, "initial import"); require(lib.import(found, runner) == 0, "idempotent import");
            const auto list = lib.games();
            require(list.size() == 2, "two library entries");
            Game g; for (auto item : list) if (item.name == "Template!!") g = item;
            id = g.id;
            require(!id.isEmpty(), "stable ID");
            require(g.setupProfile == "renpy-bundled-v1", "new RenPy imports pin bundled-runtime baseline");
            g.engine = "Manually corrected label"; lib.save(g);
            require(lib.game(id).setupProfile == "renpy-bundled-v1", "engine label edits do not silently change pinned setup");
            require(lib.status(g) == "New", "new prefix state");
            require(list[0].executable.isEmpty(), "ambiguous executable needs selection");
            g.arguments = {"argument with spaces", "$(do-not-execute)", "日本語"}; g.locale = "ja_JP.UTF-8";
            g.environment.insert("WINEDLLOVERRIDES", "example=b"); lib.save(g);
            auto cmd = launchCommand(lib, g);
            require(cmd.arguments[1] == "argument with spaces", "no shell splitting");
            require(launchCommand(lib, g, true).arguments ==
                    QStringList({"C:\\windows\\system32\\cmd.exe", "/c", "exit", "0"}),
                    "prefix preparation runs a harmless command instead of an empty executable");
            require(cmd.environment.value("WINEPREFIX") == lib.prefix(id), "dedicated prefix");
            require(cmd.environment.value("HOST_LC_ALL") == "ja_JP.UTF-8", "Proton locale override");
            require(lib.prefix(id) != lib.prefix(list[0].id), "isolated prefixes");
            require(supervise(lib, id, true) == 0, "prepare succeeds");
            require(lib.status(g) == "Ready", "ready after successful prefix setup");
            require(supervise(lib, id, false) == 0, "fake backend launch");
            QFile invocation(lib.prefix(id) + "/invocation"); require(invocation.open(QIODevice::ReadOnly), "invocation captured");
            auto contents = invocation.readAll();
            require(contents.contains("argument with spaces\n$(do-not-execute)\n"), "arguments passed literally");
            require(contents.startsWith(folder.toUtf8()), "correct working directory");
            QLockFile lock(lib.gamePath(id, "launch.lock")); require(lock.tryLock(), "prefix lock");
            require(lib.busy(id), "busy state"); require(supervise(lib, id, false) == 2, "concurrent launch rejected");
            rejects([&] { lib.save(g); }, "editing busy game rejected"); lock.unlock();
            g.environment["WINEPREFIX"] = "/tmp/other";
            rejects([&] { launchCommand(lib, g); }, "prefix override rejected"); g.environment.remove("WINEPREFIX");
            auto settings = lib.settings(); settings["umu"] = "/missing/umu"; lib.setSettings(settings);
            require(supervise(lib, id, true) == 1, "missing backend fails");
            require(lib.status(g) == "Needs attention", "failure is persisted");
            settings["umu"] = fake; lib.setSettings(settings);
            require(supervise(lib, id, false) == 0, "retry recovers");
            touch(runner + "/version", "test-2\n");
            require(lib.status(lib.game(id)) == "Runner changed", "runner update detected");
            rejects([&] { launchCommand(lib, lib.game(id)); }, "unreviewed runner update rejected");
            lib.save(g); g = lib.game(id);
            require(g.runnerRevision == "test-2", "explicit save accepts runner revision");
            writeJson(lib.gamePath(id, "status.json"), {{"state", "Running"}});
            require(lib.status(g) == "Interrupted", "lost supervisor is not reported as running");
            require(supervise(lib, id, false) == 0, "interrupted state can retry");
            g.backend = "steam"; g.steamId = "abc";
            rejects([&] { launchCommand(lib, g); }, "invalid Steam ID rejected");
            rejects([&] { lib.gamePath("../../evil", "config.json"); }, "invalid ID cannot escape library");
            const auto prior = g.id; g.backend = "umu"; g.name = "Renamed"; lib.save(g);
            require(lib.game(prior).id == prior, "rename preserves installation ID");
            QDir(folder).rename(folder, root + "/Moved"); require(lib.status(g) == "Unavailable", "missing path kept unavailable");
            require(lib.games().size() == 2, "missing folder retains entry");
        }
        Library reopened(root); require(reopened.game(id).name == "Renamed", "database roundtrip");
        require(reopened.game(id).arguments.size() == 3, "argument persistence");
        touch(temp.path() + "/bad.json", "broken"); rejects([&] { readJson(temp.path() + "/bad.json"); }, "corrupt JSON rejected");
        testRelinking(temp.path(), runner, fake);
        testRemoval(temp.path(), runner);
        testSetupRecovery(temp.path());
        testRecipeWinetricks(temp.path());
        testSteamRemovalGuard(temp.path(), runner);
        testRuntimeBarrier(temp.path(), runner, fake);
        testPrefixTools(temp.path());
        testExtraLaunch(temp.path());
        testGameInstallation(temp.path());
        testFolderImport(temp.path());
        testOneLaunchDiagnostics(temp.path());
        testSchemaAndManagedRunners(temp.path());
        testSaveSnapshots(temp.path());
        testSaveDiscovery(temp.path());
        testSteamShortcut(temp.path());
        testDeviceIdentity(temp.path());
        testMovedLibrary(temp.path());
        testNestedGameDiscovery(temp.path());
        testNativeGameDiscovery(temp.path());
        testPairingProtocol();
        testSyncQueue(temp.path());
        testLanGameManifest();
        testSetupRecipePrecedence();
        testEditableRecipes(temp.path());
        testLegacyRuntimeMigration(temp.path());
        std::printf("PASS: %d assertions\n", assertions); return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
