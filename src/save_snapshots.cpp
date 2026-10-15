#include "save_internal.h"

namespace novelty {
namespace save_detail {
QByteArray digest(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) fail("Cannot read save file: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto data = file.read(1024 * 1024);
        if (data.isEmpty() && file.error() != QFile::NoError) fail("Cannot finish reading save file: " + path);
        hash.addData(data);
    }
    return hash.result().toHex();
}
constexpr qint64 maxSnapshotBytes = 4LL * 1024 * 1024 * 1024;
void inspectNode(const QString &path, const QString &relative, QJsonArray &entries,
                 SnapshotBudget *budget, int depth = 0) {
    const QFileInfo info(path);
    if (budget && (++budget->entries > 20000 || depth > 64))
        fail("Save folder has too many entries or nesting levels. Choose a more specific save location.");
    if (info.isSymbolicLink()) fail("Save mapping contains a symbolic link: " + path);
    if (info.isDir()) {
        if (!info.isReadable() || !info.isExecutable()) fail("Cannot read save directory: " + path);
        entries.append(QJsonObject{{"path", relative}, {"type", "directory"}});
        QFileInfoList children;
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            children.append(it.nextFileInfo());
            if (budget && children.size() + budget->entries > 20000)
                fail("Save folder has too many entries. Choose a more specific save location.");
        }
        std::sort(children.begin(), children.end(), [](const auto &a, const auto &b) { return a.fileName() < b.fileName(); });
        for (const auto &child : children)
            inspectNode(child.absoluteFilePath(), relative.isEmpty() ? child.fileName() : relative + '/' + child.fileName(), entries, budget, depth + 1);
    } else if (info.isFile()) {
        if (budget) {
            if (info.size() < 0 || info.size() > maxSnapshotBytes - budget->bytes)
                fail("Save locations exceed the 4 GiB backup limit. Choose a more specific save folder or file.");
            budget->bytes += info.size();
        }
        entries.append(QJsonObject{{"path", relative}, {"type", "file"},
                                    {"size", static_cast<double>(info.size())}});
    } else fail("Save mapping is neither a regular file nor a directory: " + path);
}
QJsonArray inventory(const QString &path, SnapshotBudget *budget, bool hashes) {
    QJsonArray entries;
    if (exists(path)) inspectNode(path, QString(), entries, budget);
    if (hashes) for (int i = 0; i < entries.size(); ++i) {
        auto entry = entries[i].toObject();
        if (entry["type"] != "file") continue;
        const auto relative = entry["path"].toString();
        entry["sha256"] = QString::fromLatin1(digest(relative.isEmpty() ? path : path + '/' + relative));
        entries[i] = entry;
    }
    return entries;
}
void copyNode(const QString &source, const QString &destination) {
    const QFileInfo info(source);
    if (info.isSymbolicLink()) fail("Save mapping contains a symbolic link: " + source);
    if (info.isDir()) {
        if (!QDir().mkpath(destination)) fail("Cannot create snapshot directory: " + destination);
        const QDir dir(source);
        for (const auto &child : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name))
            copyNode(child.absoluteFilePath(), QDir(destination).filePath(child.fileName()));
    } else if (info.isFile()) {
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source, destination))
            fail("Cannot copy save file: " + source);
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(QFile::encodeName(source).constData(), error);
        if (error) fail("Cannot read save modification time: " + source);
        std::filesystem::last_write_time(QFile::encodeName(destination).constData(), modified, error);
        if (error) fail("Cannot preserve save modification time: " + destination);
    } else fail("Save mapping is neither a regular file nor a directory: " + source);
}
void removeNode(const QString &path) {
    const QFileInfo info(path);
    if (info.isSymbolicLink() || info.isFile()) { if (!QFile::remove(path)) fail("Cannot remove temporary save file: " + path); }
    else if (info.isDir() && !QDir(path).removeRecursively()) fail("Cannot remove temporary save directory: " + path);
}
void verifySnapshot(const QString &directory, const QJsonObject &manifest) {
    if ((manifest["schema"].toInt() != 1 && manifest["schema"].toInt() != 2) || !manifest["items"].isArray())
        fail("Unsupported save snapshot.");
    const auto items = manifest["items"].toArray();
    const auto children = QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    if (children != QStringList{"items", "manifest.json"}) fail("Save snapshot contains unexpected files.");
    const auto storedItems = QDir(directory + "/items").entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &name : storedItems) {
        bool index = false; const int number = name.toInt(&index);
        if (!index || number < 0 || number >= items.size() || name != QString::number(number))
            fail("Save snapshot contains an unexpected item.");
    }
    int files = 0; qint64 bytes = 0;
    for (int i = 0; i < items.size(); ++i) {
        const auto item = items[i].toObject();
        if (item["scope"].toString().isEmpty() || item["path"].toString().isEmpty() || !item["entries"].isArray())
            fail("Save snapshot manifest is incomplete.");
        const auto path = directory + "/items/" + QString::number(i);
        noLinks(path);
        if (inventory(path) != item["entries"].toArray()) fail("Save snapshot verification failed: " + path);
        for (const auto &entry : item["entries"].toArray()) if (entry.toObject()["type"] == "file") {
            ++files; bytes += static_cast<qint64>(entry.toObject()["size"].toDouble());
        }
    }
    if (manifest["schema"].toInt() == 2) {
        if (manifest["files"].toInt(-1) != files || static_cast<qint64>(manifest["bytes"].toDouble(-1)) != bytes ||
            !manifest["parent_ids"].isArray()) fail("Save snapshot totals or ancestry are invalid.");
        QSet<QString> seen;
        for (const auto &parent : manifest["parent_ids"].toArray()) {
            const auto id = parent.toString();
            if (!validToken(id) || id == manifest["id"].toString() || seen.contains(id))
                fail("Save snapshot ancestry is invalid.");
            seen.insert(id);
        }
    }
}
void verifyAvailableMappingRoots(const Library &lib, const Game &game, const SaveConfig &config) {
    for (const auto &mapping : config.mappings) {
        const auto base = mapping.scope == "game" ? mappingRoot(lib, game, "game") : lib.prefix(game.id);
        noLinks(base);
        const QFileInfo baseInfo(base);
        if (!baseInfo.isDir() || !baseInfo.isReadable() || !baseInfo.isExecutable())
            fail("The game or prefix save root is unavailable. Reconnect it before capturing saves: " + base);
        const auto target = targetPath(lib, game, mapping);
        auto parent = QFileInfo(target).absolutePath();
        while (parent.startsWith(base + '/') || parent == base) {
            const QFileInfo info(parent);
            if (exists(parent) && (!info.isDir() || !info.isReadable() || !info.isExecutable()))
                fail("Cannot inspect the mapped save location: " + parent);
            if (parent == base) break;
            parent = QFileInfo(parent).absolutePath();
        }
    }
}
SaveSnapshot snapshotLocked(Library &lib, const Game &game, const SaveConfig &config, const QString &reason,
                            bool allowEmpty) {
    validate(lib, game, config);
    if (config.mappings.isEmpty()) fail("Map at least one save file or folder before creating a snapshot.");
    if (allowEmpty) verifyAvailableMappingRoots(lib, game, config);
    const auto root = snapshotsRoot(lib, game.id);
    noLinks(root);
    if (!QDir().mkpath(root)) fail("Cannot create save snapshot storage.");
    // Count bytes and entries before hashing or copying. A mistaken UserData/assets mapping
    // must not fill the disk; previously created snapshots remain readable without this limit.
    SnapshotBudget budget;
    for (const auto &mapping : config.mappings)
        if (inventory(targetPath(lib, game, mapping), &budget, false).size() > 10000)
            fail("Save location has too many entries for a portable backup. Choose a more specific location.");
    struct statvfs storage{};
    if (statvfs(QFile::encodeName(root).constData(), &storage) == 0 && storage.f_frsize &&
        storage.f_bavail < (budget.bytes + 16LL * 1024 * 1024 + storage.f_frsize - 1) / storage.f_frsize)
        fail("Not enough free space to back up saves. Free some space and use Rescan to retry.");
    SaveSnapshot result; result.id = token(); result.created = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    result.reason = reason;
    const auto stage = root + "/.pending-" + result.id, final = root + '/' + result.id;
    if (exists(stage) || exists(final) || !QDir().mkpath(stage + "/items")) fail("Cannot stage save snapshot.");
    try {
        QJsonArray items;
        for (int i = 0; i < config.mappings.size(); ++i) {
            const auto &mapping = config.mappings[i];
            const auto source = targetPath(lib, game, mapping), destination = stage + "/items/" + QString::number(i);
            const auto before = inventory(source);
            if (exists(source)) copyNode(source, destination);
            const auto after = inventory(destination);
            if (before != after) fail("Save changed while copying. Close the game and retry the snapshot.");
            for (const auto &entry : after) if (entry.toObject()["type"] == "file") {
                ++result.files; result.bytes += static_cast<qint64>(entry.toObject()["size"].toDouble());
            }
            items.append(QJsonObject{{"scope", mapping.scope}, {"path", mapping.path}, {"entries", after}});
        }
        if (result.files == 0 && !allowEmpty) throw NoSaveData();
        QString parent;
        QString head = currentHead(lib, game.id);
        // Only an explicitly applied head proves ancestry. Imported history may never have been played.
        if (!head.isEmpty()) {
            const auto older = readJson(root + '/' + head + "/manifest.json");
            if (older["id"].toString() != head) fail("Current save snapshot is missing or invalid.");
            verifySnapshot(root + '/' + head, older);
            if (older["schema"].toInt() != 2 ||
                !mappingsMatch(config, older) ||
                !profileGameMatches(config, older["profile_game_id"].toString()) ||
                (!config.gameIdentity.isEmpty() && older["game_identity"].toString() != config.gameIdentity) ||
                (!config.releaseIdentity.isEmpty() && older["release_identity"].toString() != config.releaseIdentity) ||
                (!config.saveCompatibility.isEmpty() && older["save_compatibility"].toString() != config.saveCompatibility)) head.clear();
            if (!head.isEmpty()) {
                parent = head;
                bool identical = older["items"].toArray() == items;
                if (identical) {
                    if (!QDir(stage).removeRecursively()) fail("Cannot clear duplicate snapshot staging.");
                    const auto prior = listSaveSnapshots(lib, game.id);
                    for (const auto &snapshot : prior) if (snapshot.id == head) { setHead(lib, game.id, head); return snapshot; }
                    fail("Current save snapshot is missing from history.");
                }
            }
        }
        const auto machine = QSysInfo::machineUniqueId();
        const auto author = machine.isEmpty() ? QString() : QString::fromLatin1(QCryptographicHash::hash(
            config.profileId.toUtf8() + machine, QCryptographicHash::Sha256).toHex());
        const QJsonObject manifest{{"schema", 2}, {"id", result.id}, {"created", result.created},
                                   {"reason", reason}, {"game_identity", config.gameIdentity},
                                   {"profile_id", config.profileId}, {"profile_game_id", config.profileGameId},
                                   {"release_identity", config.releaseIdentity},
                                   {"save_compatibility", config.saveCompatibility}, {"parent_ids", parent.isEmpty() ? QJsonArray{} : QJsonArray{parent}},
                                   {"author_device", author},
                                   {"items", items}, {"files", result.files}, {"bytes", static_cast<double>(result.bytes)}};
        if (QJsonDocument(manifest).toJson().size() > 1024 * 1024)
            fail("Save backup metadata exceeds the portable snapshot limit. Choose a more specific save location.");
        writeJson(stage + "/manifest.json", manifest);
        verifySnapshot(stage, manifest);
        if (!QDir().rename(stage, final)) fail("Cannot publish save snapshot.");
        setHead(lib, game.id, result.id);
        return result;
    } catch (...) { if (QFileInfo(stage).isDir()) QDir(stage).removeRecursively(); throw; }
}
void finishPendingLocked(Library &lib, const Game &game) {
    const auto pending = readJson(pendingPath(lib, game.id));
    if (pending.isEmpty()) return;
    if (pending["schema"].toInt() != 1 || pending["game_id"].toString() != game.id ||
        !pending["config"].isObject()) fail("Invalid pending save snapshot record; inspect it before playing.");
    const auto config = loadSaveConfig(lib, game.id);
    const auto previous = pending["config"].toObject();
    const auto current = configJson(config);
    if (current["profile_game_id"] != previous["profile_game_id"] ||
        current["game_identity"] != previous["game_identity"] ||
        current["release_identity"] != previous["release_identity"] ||
        current["save_compatibility"] != previous["save_compatibility"] ||
        current["mappings"] != previous["mappings"])
        fail("Save mappings changed since the game ran. Review them before completing the pending snapshot.");
    snapshotLocked(lib, game, config, "after play", true);
    if (!QFile::remove(pendingPath(lib, game.id))) fail("Cannot clear pending save snapshot record.");
    if (readJson(lib.gamePath(game.id, "status.json"))["state"] == "Save needs attention")
        lib.recordStatus(game.id, "Ready", "Pending save backup completed.");
}
QJsonObject snapshotManifest(const Library &lib, const QString &id, const QString &snapshotId, bool verifyContents) {
    if (!validToken(snapshotId)) fail("Invalid save snapshot ID.");
    const auto root = snapshotsRoot(lib, id) + '/' + snapshotId;
    noLinks(root);
    const auto manifest = readJson(root + "/manifest.json");
    if (manifest["id"].toString() != snapshotId) fail("Save snapshot ID does not match its manifest.");
    if (verifyContents) verifySnapshot(root, manifest);
    return manifest;
}
bool liveMatchesSnapshot(const Library &lib, const Game &game, const SaveConfig &config,
                         const QJsonObject &manifest) {
    validate(lib, game, config);
    verifyAvailableMappingRoots(lib, game, config);
    if (!mappingsMatch(config, manifest)) fail("Save mappings changed since the peer transfer.");
    const auto items = manifest["items"].toArray();
    for (int i = 0; i < items.size(); ++i)
        if (inventory(targetPath(lib, game, config.mappings[i])) != items[i].toObject()["entries"].toArray())
            return false;
    return true;
}
bool hasAncestor(const Library &lib, const QString &gameId, const QString &descendant, const QString &ancestor) {
    if (descendant == ancestor) return true;
    QStringList waiting{descendant}; QSet<QString> seen;
    while (!waiting.isEmpty()) {
        const auto id = waiting.takeLast();
        if (!validToken(id) || seen.contains(id) || seen.size() > 10000) continue;
        seen.insert(id);
        const auto path = snapshotsRoot(lib, gameId) + '/' + id + "/manifest.json";
        if (!QFileInfo(path).isFile()) continue; // The peer may not have sent older history yet.
        const auto manifest = readJson(path);
        if (manifest["id"].toString() != id || manifest["schema"].toInt() != 2) continue;
        for (const auto &parent : manifest["parent_ids"].toArray()) {
            const auto previous = parent.toString();
            if (previous == ancestor) return true;
            waiting.append(previous);
        }
    }
    return false;
}
} // namespace save_detail
using namespace save_detail;

SaveSnapshot createSaveSnapshot(Library &lib, const QString &gameId, const QString &reason) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before creating a snapshot.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    return snapshotLocked(lib, game, config, reason.left(80));
}
SaveSnapshot captureVerifiedSaveState(Library &lib, const QString &gameId, const QString &reason) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before capturing saves.");
    if (!pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish the pending post-play snapshot before capturing saves.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    return snapshotLocked(lib, game, config, reason.left(80), true);
}
SaveSnapshot captureVerifiedSaveStateDuringLaunch(Library &lib, const QString &gameId, const QString &reason) {
    if (!saveRestoreIssue(lib, gameId).isEmpty() || !pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish save recovery or the pending post-play snapshot before server sync.");
    const auto game = lib.game(gameId);
    return snapshotLocked(lib, game, loadSaveConfig(lib, gameId), reason.left(80), true);
}
QJsonObject verifyPortableSaveBundle(const QString &bundleDirectory) {
    const auto source = QDir::cleanPath(bundleDirectory);
    noLinks(source);
    const QFileInfo root(source), metadata(source + "/manifest.json");
    noLinks(metadata.absoluteFilePath());
    if (!root.isDir() || metadata.size() < 1 || metadata.size() > 1024 * 1024)
        fail("Portable save bundle or its manifest is unavailable.");
    const auto manifest = readJson(metadata.absoluteFilePath());
    if (manifest["schema"].toInt() != 2 || !validToken(manifest["id"].toString()) ||
        !validToken(manifest["profile_id"].toString()) ||
        !validToken(manifest["profile_game_id"].toString()) || !manifest["items"].isArray())
        fail("Portable save bundle identity or version is invalid.");
    const auto items = manifest["items"].toArray();
    if (items.isEmpty() || items.size() > 32 || manifest["files"].toInt(-1) < 0 ||
        manifest["bytes"].toDouble(-1) < 0 || manifest["bytes"].toDouble() > 512.0 * 1024 * 1024)
        fail("Portable save bundle exceeds the server transfer limit.");
    QSet<QString> paths;
    qsizetype entries = 0;
    for (const auto &value : items) {
        const auto item = value.toObject();
        const auto scope = item["scope"].toString(), path = item["path"].toString();
        if (!QStringList{"game", "prefix", "appdata", "documents"}.contains(scope) ||
            cleanMappingPath(path) != path || paths.contains(scope + ':' + path))
            fail("Portable save bundle mapping is invalid or duplicated.");
        paths.insert(scope + ':' + path);
        entries += item["entries"].toArray().size();
        if (entries > 20000) fail("Portable save bundle has too many entries.");
    }
    verifySnapshot(source, manifest);
    return manifest;
}
QJsonObject inspectVerifiedLiveSaveState(const Library &lib, const QString &gameId) {
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    validate(lib, game, config);
    if (config.mappings.isEmpty()) fail("Map at least one save location before checking server saves.");
    verifyAvailableMappingRoots(lib, game, config);
    SnapshotBudget budget;
    QJsonArray items;
    int files = 0;
    for (const auto &mapping : config.mappings) {
        const auto entries = inventory(targetPath(lib, game, mapping), &budget);
        for (const auto &value : entries) if (value.toObject()["type"] == "file") ++files;
        items.append(QJsonObject{{"scope", mapping.scope}, {"path", mapping.path}, {"entries", entries}});
    }
    if (budget.bytes > 512LL * 1024 * 1024)
        fail("Mapped saves exceed the 512 MiB server transfer limit.");
    return {{"items", items}, {"files", files}, {"bytes", static_cast<double>(budget.bytes)},
            {"game_identity", config.gameIdentity}, {"release_identity", config.releaseIdentity},
            {"save_compatibility", config.saveCompatibility}};
}
bool captureAvailableSaveSnapshot(Library &lib, const QString &gameId, const QString &reason) {
    SaveLocks locks(lib, gameId, 2000);
    if (!saveRestoreIssue(lib, gameId).isEmpty())
        fail("Recover the interrupted save restore before transferring this game.");
    if (!pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish the pending post-play snapshot before transferring this game.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    if (config.mappings.isEmpty()) return false;
    try {
        snapshotLocked(lib, game, config, reason.left(80));
        return true;
    } catch (const NoSaveData &) {
        return false;
    }
}
QList<SaveSnapshot> listSaveSnapshots(const Library &lib, const QString &gameId) {
    lib.game(gameId);
    const auto root = snapshotsRoot(lib, gameId);
    noLinks(root);
    QList<SaveSnapshot> result;
    for (const auto &folder : QDir(root).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!validToken(folder.fileName())) continue;
        const auto manifest = readJson(folder.absoluteFilePath() + "/manifest.json");
        if ((manifest["schema"].toInt() != 1 && manifest["schema"].toInt() != 2) || manifest["id"].toString() != folder.fileName()) continue;
        result.append({folder.fileName(), manifest["created"].toString(), manifest["reason"].toString(),
                       manifest["files"].toInt(), static_cast<qint64>(manifest["bytes"].toDouble())});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.created > b.created; });
    return result;
}
QString appliedSaveSnapshotId(const Library &lib, const QString &gameId) {
    lib.game(gameId);
    return currentHead(lib, gameId);
}
QString verifiedSaveSnapshotBundle(const Library &lib, const QString &gameId, const QString &snapshotId) {
    lib.game(gameId);
    const auto manifest = snapshotManifest(lib, gameId, snapshotId);
    const auto folder = snapshotsRoot(lib, gameId) + '/' + snapshotId;
    if (manifest["schema"].toInt() != 2) fail("Only portable save snapshots can be sent to a peer.");
    return folder;
}
QStringList saveSnapshotLineage(const Library &lib, const QString &gameId, int maxSnapshots) {
    if (maxSnapshots < 1 || maxSnapshots > 256) fail("Invalid save history limit.");
    const auto config = loadSaveConfig(lib, gameId);
    QStringList newestFirst;
    QSet<QString> seen;
    auto id = appliedSaveSnapshotId(lib, gameId);
    while (!id.isEmpty() && newestFirst.size() < maxSnapshots) {
        if (!validToken(id) || seen.contains(id)) fail("Save history contains an invalid parent.");
        seen.insert(id);
        const auto manifest = snapshotManifest(lib, gameId, id, false);
        // Older builds could link across a mapping change. Keep those snapshots, but do not
        // send incompatible ancestors as part of the current installation's save chain.
        if (manifest["schema"].toInt() != 2 || !mappingsMatch(config, manifest) ||
            !labelsCompatible(config, manifest)) break;
        newestFirst.append(id);
        const auto parents = manifest["parent_ids"].toArray();
        if (parents.size() > 1) break; // A branch merge needs explicit reconciliation.
        id = parents.isEmpty() ? QString() : parents.first().toString();
    }
    std::reverse(newestFirst.begin(), newestFirst.end());
    return newestFirst;
}
QList<ImportedSaveCandidate> importedSaveCandidates(const Library &lib, const QString &gameId) {
    lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    const auto head = currentHead(lib, gameId);
    QList<ImportedSaveCandidate> result;
    for (const auto &snapshot : listSaveSnapshots(lib, gameId)) {
        // Status/list queries need metadata only. Payloads are verified before import,
        // restore or transmission; hashing the entire history here stalls the GUI.
        const auto manifest = snapshotManifest(lib, gameId, snapshot.id, false);
        if (manifest["schema"].toInt() != 2 || manifest["profile_id"].toString() == config.profileId ||
            !profileGameMatches(config, manifest["profile_game_id"].toString()) ||
            !labelsCompatible(config, manifest) || !mappingsMatch(config, manifest) || snapshot.id == head)
            continue;
        const QString relation = head.isEmpty() ? "available" :
            hasAncestor(lib, gameId, snapshot.id, head) ? "ahead" :
            hasAncestor(lib, gameId, head, snapshot.id) ? "behind" : "conflict";
        if (relation != "behind") result.append({snapshot.id, snapshot.created, relation});
    }
    for (int i = result.size() - 1; i >= 0; --i) {
        bool superseded = false;
        for (int j = 0; j < result.size(); ++j)
            if (j != i && hasAncestor(lib, gameId, result[j].snapshotId, result[i].snapshotId)) {
                superseded = true; break;
            }
        if (superseded) result.removeAt(i);
    }
    return result;
}
bool adoptInitialPeerSave(Library &lib, const QString &gameId) {
    if (!currentHead(lib, gameId).isEmpty()) return false;
    const auto candidates = importedSaveCandidates(lib, gameId);
    if (candidates.size() != 1 || candidates.first().relation != "available") return false;
    // A newly registered game should have empty mapped roots. If files appeared
    // locally, retain and snapshot them before leaving the peer head for review.
    if (captureAvailableSaveSnapshot(lib, gameId, "local progress before peer sync")) return false;
    restoreSaveSnapshot(lib, gameId, candidates.first().snapshotId);
    return true;
}
SaveImportResult importSaveSnapshotLocked(Library &lib, const QString &gameId, const QString &bundleDirectory,
                                          const QString &authenticatedPeer) {
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before importing saves.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    if (config.mappings.isEmpty()) fail("Choose this game's save location before syncing.");
    const auto source = QDir::cleanPath(bundleDirectory);
    noLinks(source);
    if (!QFileInfo(source).isDir() || QFileInfo(source + "/manifest.json").size() > 1024 * 1024)
        fail("Save bundle is missing or its manifest is too large.");
    const auto manifest = readJson(source + "/manifest.json");
    const auto id = manifest["id"].toString();
    if (!authenticatedPeer.isEmpty() && (!validFingerprint(authenticatedPeer) || !isTrustedPeer(lib, authenticatedPeer)))
        fail("The source device is not trusted for this Games library.");
    if (manifest["schema"].toInt() != 2 || !validToken(id) ||
        !profileGameMatches(config, manifest["profile_game_id"].toString(), authenticatedPeer) ||
        !labelsCompatible(config, manifest))
        fail("Save bundle belongs to a different game, edition or save format.");
    const auto items = manifest["items"].toArray();
    if (items.size() != config.mappings.size() || items.size() > 32) fail("Save bundle mappings do not match this installation.");
    for (int i = 0; i < items.size(); ++i) {
        const auto item = items[i].toObject();
        if (item["scope"].toString() != config.mappings[i].scope ||
            item["path"].toString() != config.mappings[i].path || item["entries"].toArray().size() > 10000)
            fail("Save bundle paths do not match this installation.");
    }
    verifySnapshot(source, manifest);
    const auto root = snapshotsRoot(lib, gameId);
    noLinks(root);
    if (!QDir().mkpath(root)) fail("Cannot create imported save storage.");
    const auto target = root + '/' + id;
    bool added = false;
    if (exists(target)) {
        const auto previous = snapshotManifest(lib, gameId, id);
        if (previous != manifest) fail("A different save snapshot already uses this ID.");
    } else {
        const auto stage = root + "/.import-" + token();
        try {
            copyNode(source, stage);
            if (readJson(stage + "/manifest.json") != manifest) fail("Imported save manifest changed during copying.");
            verifySnapshot(stage, manifest);
            if (!QDir().rename(stage, target)) fail("Cannot publish imported save snapshot.");
            added = true;
        } catch (...) { if (QFileInfo(stage).isDir()) QDir(stage).removeRecursively(); throw; }
    }
    const auto head = currentHead(lib, gameId);
    const auto relation = head.isEmpty() ? "available" : head == id ? "current" :
                          hasAncestor(lib, gameId, id, head) ? "ahead" :
                          hasAncestor(lib, gameId, head, id) ? "behind" : "conflict";
    return {added, relation, id};
}
SaveImportResult importSaveSnapshot(Library &lib, const QString &gameId, const QString &bundleDirectory,
                                    const QString &authenticatedPeer) {
    SaveLocks locks(lib, gameId);
    return importSaveSnapshotLocked(lib, gameId, bundleDirectory, authenticatedPeer);
}
SaveImportResult importSaveSnapshotDuringLaunch(Library &lib, const QString &gameId,
                                                const QString &bundleDirectory) {
    if (!saveRestoreIssue(lib, gameId).isEmpty() || !pendingSaveSnapshotIssue(lib, gameId).isEmpty())
        fail("Finish save recovery or the pending post-play snapshot before server import.");
    return importSaveSnapshotLocked(lib, gameId, bundleDirectory, {});
}
QString pendingSaveSnapshotIssue(const Library &lib, const QString &gameId) {
    return exists(pendingPath(lib, gameId)) ? "A post-play backup is pending. Use Rescan to retry; local Play remains available." : QString();
}
void queuePostExitSaveSnapshot(const Library &lib, const Game &game) {
    const auto config = loadSaveConfig(lib, game.id);
    if (config.mappings.isEmpty()) return;
    if (exists(pendingPath(lib, game.id))) return;
    writeJson(pendingPath(lib, game.id), {{"schema", 1}, {"game_id", game.id},
        {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"config", configJson(config)}});
}
void finishPostExitSaveSnapshot(Library &lib, const Game &game) {
    if (!exists(pendingPath(lib, game.id))) return;
    QLockFile mutation(lib.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance prevented the post-play save snapshot. Use Rescan to retry it.");
    finishPendingLocked(lib, game);
}
QStringList recoverPendingSaveSnapshots(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        if (!exists(pendingPath(lib, game.id))) continue;
        if (lib.supervisorActive(game.id)) continue;
        try {
            SaveLocks locks(lib, game.id);
            if (!saveRestoreIssue(lib, game.id).isEmpty()) fail("Recover the interrupted save restore first.");
            finishPendingLocked(lib, game);
            messages.append("Completed the pending save snapshot for " + game.name + ".");
        } catch (const std::exception &e) {
            messages.append("Save snapshot for " + game.name + " is pending: " + QString::fromUtf8(e.what()));
        }
    }
    return messages;
}
} // namespace novelty
