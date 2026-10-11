#include "saves.h"
#include "devices.h"
#include "sync_queue.h"
#include "runtime.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/quuid.h>
#include <QtCore/qsysinfo.h>
#include <QtCore/qset.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
struct NoSaveData : std::runtime_error { NoSaveData() : std::runtime_error("No saved data yet in the confirmed locations.") {} };
bool exists(const QString &path) { const QFileInfo info(path); return info.exists() || info.isSymbolicLink(); }
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validToken(const QString &value) { return !QUuid(value).isNull() && !value.contains('/'); }
bool validFingerprint(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool profileGameMatches(const SaveConfig &config, const QString &profileGameId,
                        const QString &authenticatedPeer = {}) {
    if (profileGameId == config.profileGameId) return true;
    for (const auto &link : config.peerLinks)
        if (link.profileGameId == profileGameId &&
            (authenticatedPeer.isEmpty() || link.fingerprint == authenticatedPeer)) return true;
    return false;
}
QString configPath(const Library &lib, const QString &id) { return lib.gamePath(id, "saves.json"); }
QString journalPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-restore.json"); }
QString pendingPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-pending.json"); }
QString headPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-head.json"); }
QString observationPath(const Library &lib, const QString &id) { return lib.gamePath(id, "save-observation.json"); }
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
    if (snapshotId.isEmpty()) { QFile::remove(headPath(lib, id)); return; }
    writeJson(headPath(lib, id), {{"schema", 1}, {"snapshot_id", snapshotId}});
}
void noLinks(const QString &path) {
    auto current = QString("/");
    for (const auto &part : QDir::cleanPath(QFileInfo(path).absoluteFilePath()).split('/', Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymbolicLink()) fail("Save paths cannot pass through symbolic links: " + current);
    }
}
QString cleanMappingPath(const QString &path) {
    const auto trimmed = path.trimmed();
    if (trimmed.isEmpty() || QDir::isAbsolutePath(trimmed) || trimmed.contains('\\') || trimmed.contains(QChar::Null) ||
        trimmed == "." || trimmed == ".." || trimmed.startsWith("../") || trimmed.contains("/../") ||
        trimmed.endsWith("/..") || trimmed.startsWith("./") || trimmed.contains("//"))
        fail("Choose a file or folder below the game or prefix root, using a relative path.");
    for (const auto &part : trimmed.split('/')) if (part.isEmpty() || part == "." || part == "..")
        fail("Save paths cannot contain . or .. components.");
    return trimmed;
}
QString mappingRoot(const Library &lib, const Game &game, const QString &scope) {
    if (scope == "prefix") return lib.prefix(game.id);
    if (scope == "appdata" || scope == "documents") {
        const auto users = lib.prefix(game.id) + "/drive_c/users";
        QString name = "steamuser";
        if (!QFileInfo(users + "/steamuser").isDir() && QFileInfo(users).isDir()) {
            QStringList available;
            for (const auto &entry : QDir(users).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name))
                if (entry.fileName().compare("Public", Qt::CaseInsensitive) != 0) available.append(entry.fileName());
            if (available.size() > 1) fail("Several Wine users exist. Choose an explicit prefix-relative save path.");
            if (available.size() == 1) name = available.first();
        }
        return users + '/' + name + (scope == "appdata" ? "/AppData/Roaming" : "/Documents");
    }
    if (scope == "game") {
        if (!QFileInfo(game.folder).isDir() ||
            (!game.folderIdentity.isEmpty() && folderIdentity(game.folder) != game.folderIdentity))
            fail("The game folder is unavailable or changed. Relink it before accessing mapped saves.");
        return game.folder;
    }
    fail("Save scope must be Game folder or Prefix.");
}
QString targetPath(const Library &lib, const Game &game, const SaveMapping &mapping) {
    const auto root = mappingRoot(lib, game, mapping.scope);
    const auto relative = cleanMappingPath(mapping.path);
    const auto target = QDir(root).filePath(relative);
    noLinks(target);
    if (canonicalPath(target) != QDir::cleanPath(target)) fail("Save path escapes its game or prefix root.");
    return target;
}
void validate(const Library &lib, const Game &game, const SaveConfig &config) {
    if (!validToken(config.profileId) || config.profileId != lib.profileId()) fail("Save mapping belongs to another local profile.");
    if (!validToken(config.profileGameId)) fail("The stable game identity is invalid.");
    if (!QStringList{"searching", "identified", "unidentified"}.contains(config.discoveryState))
        fail("Unknown save discovery state.");
    if (config.discoveryState == "identified" && config.mappings.isEmpty())
        fail("Identified saves need at least one confirmed location.");
    if (config.discoveryState == "unidentified" && !config.mappings.isEmpty())
        fail("Stop searching is available only when no save location is confirmed.");
    for (const auto &identity : {config.gameIdentity, config.releaseIdentity, config.saveCompatibility})
        if (identity.size() > 256 || identity.contains(QChar::Null)) fail("Save identity is too long or invalid.");
    if (config.mappings.size() > 32) fail("Use at most 32 save locations per game.");
    if (config.peerLinks.size() > 32) fail("Use at most 32 paired game links.");
    QSet<QString> linkedPeers;
    for (const auto &link : config.peerLinks) {
        if (!validFingerprint(link.fingerprint) || !validToken(link.profileGameId) ||
            link.profileGameId == config.profileGameId || linkedPeers.contains(link.fingerprint))
            fail("A paired game link is invalid or duplicated.");
        linkedPeers.insert(link.fingerprint);
    }
    QStringList all;
    for (const auto &mapping : config.mappings) {
        const auto key = targetPath(lib, game, mapping);
        for (const auto &other : all)
            if (key == other || key.startsWith(other + '/') || other.startsWith(key + '/'))
                fail("Save locations cannot overlap.");
        all.append(key);
    }
}
QJsonObject configJson(const SaveConfig &config) {
    QJsonArray mappings;
    for (const auto &mapping : config.mappings)
        mappings.append(QJsonObject{{"scope", mapping.scope}, {"path", mapping.path}});
    QJsonArray peerLinks;
    for (const auto &link : config.peerLinks)
        peerLinks.append(QJsonObject{{"fingerprint", link.fingerprint}, {"profile_game_id", link.profileGameId}});
    return {{"schema", 2}, {"profile_id", config.profileId}, {"profile_game_id", config.profileGameId},
            {"game_identity", config.gameIdentity}, {"release_identity", config.releaseIdentity},
            {"save_compatibility", config.saveCompatibility}, {"discovery_state", config.discoveryState},
            {"discovery_source", config.discoverySource}, {"candidates", config.candidates}, {"mappings", mappings},
            {"peer_links", peerLinks}};
}
SaveConfig parseConfig(const QJsonObject &data, const QString &gameId) {
    SaveConfig config; config.profileGameId = gameId;
    if (data.isEmpty()) return config;
    const auto schema = data["schema"].toInt();
    if ((schema != 1 && schema != 2) || !data["mappings"].isArray()) fail("Unsupported save mapping configuration.");
    if (schema == 2) {
        config.profileId = data["profile_id"].toString();
        config.profileGameId = data["profile_game_id"].toString();
        config.discoveryState = data["discovery_state"].toString();
        config.discoverySource = data["discovery_source"].toString();
        config.candidates = data["candidates"].toArray();
        if (data.contains("peer_links") && !data["peer_links"].isArray()) fail("Paired game links are damaged.");
        for (const auto &value : data["peer_links"].toArray()) {
            if (!value.isObject()) fail("Paired game link is damaged.");
            const auto link = value.toObject();
            config.peerLinks.append({link["fingerprint"].toString(), link["profile_game_id"].toString()});
        }
    }
    config.gameIdentity = data["game_identity"].toString();
    config.releaseIdentity = data["release_identity"].toString();
    config.saveCompatibility = data["save_compatibility"].toString();
    for (const auto &item : data["mappings"].toArray()) {
        if (!item.isObject()) fail("Invalid save mapping.");
        const auto entry = item.toObject();
        SaveMapping mapping{entry["scope"].toString(), entry["path"].toString()};
        if (schema == 1 && mapping.scope == "prefix") {
            const QString appdata = "drive_c/users/steamuser/AppData/Roaming/";
            const QString documents = "drive_c/users/steamuser/Documents/";
            if (mapping.path.startsWith(appdata)) { mapping.scope = "appdata"; mapping.path = mapping.path.mid(appdata.size()); }
            else if (mapping.path.startsWith(documents)) { mapping.scope = "documents"; mapping.path = mapping.path.mid(documents.size()); }
        }
        config.mappings.append(mapping);
    }
    if (schema == 1 && !config.mappings.isEmpty()) {
        config.discoveryState = "identified"; config.discoverySource = "manual";
    }
    return config;
}
QString recipePath(const Library &lib, const SaveConfig &config) {
    if (config.gameIdentity.isEmpty() || config.releaseIdentity.isEmpty()) return {};
    const auto key = QCryptographicHash::hash((config.gameIdentity + '\n' + config.releaseIdentity).toUtf8(),
                                             QCryptographicHash::Sha256).toHex();
    return lib.dataRoot() + "/save-recipes/" + QString::fromLatin1(key) + "/recipe.json";
}
QList<SaveMapping> recipeMappings(const Library &lib, const SaveConfig &config) {
    const auto path = recipePath(lib, config);
    if (path.isEmpty() || !exists(path)) return {};
    noLinks(path);
    const auto recipe = readJson(path);
    if (recipe["schema"].toInt() != 1 || recipe["game_identity"].toString() != config.gameIdentity ||
        recipe["edition_identity"].toString() != config.releaseIdentity || !recipe["save_roots"].isArray())
        fail("Save recipe does not match the confirmed game and edition identities.");
    QList<SaveMapping> mappings;
    for (const auto &value : recipe["save_roots"].toArray()) {
        const auto root = value.toObject();
        mappings.append(SaveMapping{root["scope"].toString(), root["path"].toString()});
    }
    if (mappings.isEmpty()) fail("Save recipe declares no locations.");
    return mappings;
}
QList<SaveMapping> renpyMappings(const Game &game) {
    if (game.engine != "Ren'Py" || !QFileInfo(game.folder + "/renpy/__init__.py").isFile() ||
        !QFileInfo(game.folder + "/game").isDir()) return {};
    for (const auto &argument : game.arguments) if (argument.contains("--savedir")) return {};
    QFile options(game.folder + "/game/options.rpy");
    if (!options.open(QIODevice::ReadOnly) || options.size() > 256 * 1024) return {};
    const auto source = QString::fromUtf8(options.readAll());
    QStringList active;
    for (const auto &line : source.split('\n')) if (!line.trimmed().startsWith('#')) active.append(line);
    const auto text = active.join('\n');
    if (text.contains(QRegularExpression(R"(\bconfig\.(?:savedir|extra_savedirs)\s*=)"))) return {};
    static const QRegularExpression literal(R"(\bconfig\.save_directory\s*=\s*['\"]([^'\"\r\n]{1,120})['\"])");
    const auto match = literal.match(text);
    if (match.hasMatch()) return {{"appdata", "RenPy/" + match.captured(1)}, {"game", "game/saves"}};
    if (text.contains(QRegularExpression(R"(\bconfig\.save_directory\s*=\s*None\b)")))
        return {{"game", "game/saves"}};
    return {};
}
void resolveDeclaredSavePaths(Library &lib, const Game &game, SaveConfig &config) {
    if (config.discoveryState != "searching" || !config.mappings.isEmpty()) return;
    auto recipe = recipeMappings(lib, config);
    if (!recipe.isEmpty()) {
        config.mappings = recipe; config.discoveryState = "identified"; config.discoverySource = "recipe";
    } else if (auto engine = renpyMappings(game); !engine.isEmpty()) {
        config.mappings = engine; config.discoveryState = "identified"; config.discoverySource = "engine";
    }
    validate(lib, game, config);
    if (config.discoveryState == "identified") writeJson(configPath(lib, game.id), configJson(config));
}
struct ObservedRoot { SaveMapping mapping; QString absolute; };
QList<ObservedRoot> likelyRoots(const Library &lib, const Game &game) {
    QList<ObservedRoot> roots;
    const auto add = [&](const SaveMapping &mapping) {
        const auto path = targetPath(lib, game, mapping);
        roots.append({mapping, path});
    };
    for (const auto &relative : {"save", "saves", "savedata", "save_data", "game/saves", "game/savedata"})
        add({"game", relative});
    const auto appdata = mappingRoot(lib, game, "appdata");
    const auto renpy = appdata + "/RenPy";
    if (QFileInfo(renpy).isDir() && !QFileInfo(renpy).isSymbolicLink()) {
        int count = 0;
        for (const auto &folder : QDir(renpy).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
            if (++count > 24) break;
            add({"appdata", "RenPy/" + folder.fileName()});
        }
    }
    const auto myGames = mappingRoot(lib, game, "documents") + "/My Games";
    if (QFileInfo(myGames).isDir() && !QFileInfo(myGames).isSymbolicLink()) {
        int count = 0;
        for (const auto &folder : QDir(myGames).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
            if (++count > 24) break;
            add({"documents", "My Games/" + folder.fileName()});
        }
    }
    return roots;
}
void scanObservedFiles(const QString &directory, const QString &relative, int depth, int maxDepth,
                       int &budget, QJsonArray &files) {
    if (depth > maxDepth || !QFileInfo(directory).isDir()) return;
    noLinks(directory);
    for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.isSymbolicLink()) continue;
        if (--budget < 0) fail("Save discovery reached its file limit; choose a save path manually.");
        const auto path = relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName();
        if (entry.isFile())
            files.append(QJsonObject{{"path", path}, {"size", static_cast<double>(entry.size())},
                                     {"modified", static_cast<double>(entry.lastModified().toMSecsSinceEpoch())}});
        else if (entry.isDir()) scanObservedFiles(entry.absoluteFilePath(), path, depth + 1, maxDepth, budget, files);
    }
}
QJsonArray observedState(const Library &lib, const Game &game) {
    QJsonArray roots;
    int likelyBudget = 1024;
    for (const auto &root : likelyRoots(lib, game)) {
        QJsonArray files;
        scanObservedFiles(root.absolute, {}, 0, 2, likelyBudget, files);
        roots.append(QJsonObject{{"scope", root.mapping.scope}, {"path", root.mapping.path}, {"files", files}});
    }
    // Compare a wider inventory as well. Games can save under arbitrary names in their
    // own folder or anywhere on Wine's C: drive. These locations are suggestions only.
    int broadBudget = 100000;
    for (const auto &root : QList<ObservedRoot>{{{"game", ""}, game.folder},
                                                {{"prefix", "drive_c"}, lib.prefix(game.id) + "/drive_c"}}) {
        QJsonArray files;
        scanObservedFiles(root.absolute, {}, 0, 12, broadBudget, files);
        roots.append(QJsonObject{{"scope", root.mapping.scope}, {"path", root.mapping.path},
                                 {"broad", true}, {"files", files}});
    }
    return roots;
}
SaveMapping observedFolder(const Library &lib, const Game &game, const QJsonObject &root, const QString &file) {
    const auto relative = QFileInfo(file).path();
    const auto rootPath = root["path"].toString();
    const auto path = relative == "." ? rootPath : rootPath.isEmpty() ? relative : rootPath + '/' + relative;
    if (root["scope"].toString() == "game")
        return {"game", path.isEmpty() ? file : path};
    const auto prefix = lib.prefix(game.id) + '/';
    const auto absolute = prefix + path;
    for (const auto &scope : {QString("appdata"), QString("documents")}) {
        const auto base = mappingRoot(lib, game, scope) + '/';
        if (absolute == base.chopped(1)) return {scope, QFileInfo(file).fileName()};
        if (absolute.startsWith(base)) return {scope, absolute.mid(base.size())};
    }
    return {"prefix", relative == "." ? rootPath + '/' + file : path};
}
bool eligibleSaveFile(const QString &path, const QString &rootPath) {
    const auto lower = path.toLower(), name = QFileInfo(lower).fileName();
    for (const auto &part : lower.split('/'))
        if (QStringList{"cache", "logs", "log", "shadercache", "__pycache__", "temp", "tmp"}.contains(part)) return false;
    if (name.endsWith(".log") || name.endsWith(".ini") || name.endsWith(".cfg") || name.endsWith(".json") ||
        name.endsWith(".tmp") || name.endsWith(".lock") || name == "log.txt") return false;
    if (name == "persistent" || name.contains("save") || name.contains("slot") || name.contains("progress") ||
        name.endsWith(".sav") || name.endsWith(".rpgsave") || name.endsWith(".rvdata2")) return true;
    return rootPath.toLower().contains("savedata") && name.endsWith(".dat");
}
void evaluateObservation(Library &lib, const Game &game) {
    const auto before = readJson(observationPath(lib, game.id));
    if (before.isEmpty()) return;
    if (before["schema"].toInt() != 1 || before["game_id"].toString() != game.id || !before["roots"].isArray())
        fail("Invalid save observation record; inspect it before rescanning.");
    auto config = loadSaveConfig(lib, game.id);
    if (config.discoveryState != "searching") { QFile::remove(observationPath(lib, game.id)); return; }
    const auto after = observedState(lib, game);
    QMap<QString, QJsonObject> previous;
    for (const auto &value : before["roots"].toArray()) {
        const auto object = value.toObject(); previous.insert(object["scope"].toString() + '/' + object["path"].toString(), object);
    }
    QMap<QString, QJsonObject> candidates;
    QSet<QString> strongCandidates;
    for (const auto &value : after) {
        const auto root = value.toObject();
        const auto key = root["scope"].toString() + '/' + root["path"].toString();
        // An observation written by an older build has no broad baseline.
        if (root["broad"].toBool() && !previous.contains(key)) continue;
        const auto oldFiles = previous.value(key)["files"].toArray();
        QMap<QString, QJsonObject> oldByPath;
        for (const auto &item : oldFiles) oldByPath.insert(item.toObject()["path"].toString(), item.toObject());
        int changed = 0;
        for (const auto &item : root["files"].toArray()) {
            const auto file = item.toObject();
            const auto path = file["path"].toString();
            if (oldByPath.value(path) == file) continue;
            if (!root["broad"].toBool()) {
                if (eligibleSaveFile(path, root["path"].toString())) ++changed;
                continue;
            }
            const auto mapping = observedFolder(lib, game, root, path);
            const auto candidateKey = mapping.scope + '/' + mapping.path;
            auto candidate = candidates.value(candidateKey);
            const int count = candidate["count"].toInt() + 1;
            auto examples = candidate["examples"].toArray();
            if (examples.size() < 3) examples.append(QFileInfo(path).fileName());
            QStringList names;
            for (const auto &example : examples) names.append(example.toString());
            candidate = {{"scope", mapping.scope}, {"path", mapping.path}, {"count", count},
                         {"examples", examples},
                         {"evidence", QString::number(count) + " new or changed file(s) after Play (" +
                             names.join(", ") + "); review before using"}};
            candidates.insert(candidateKey, candidate);
        }
        if (changed) {
            const auto candidateKey = key;
            strongCandidates.insert(candidateKey);
            candidates.insert(candidateKey, QJsonObject{{"scope", root["scope"]}, {"path", root["path"]},
                {"evidence", QString::number(changed) + " likely save file(s) changed after Play"}});
        }
    }
    QJsonArray combined = config.candidates;
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        const auto item = it.value(); bool known = false;
        for (int i = 0; i < combined.size(); ++i)
            if (combined[i].toObject()["scope"] == item["scope"] && combined[i].toObject()["path"] == item["path"]) {
                combined.replace(i, item); known = true; break;
            }
        if (!known && combined.size() < 64) combined.append(item);
    }
    QJsonArray distinct;
    for (const auto &value : combined) {
        const auto item = value.toObject();
        const auto key = item["scope"].toString() + '/' + item["path"].toString();
        bool covered = false;
        for (const auto &strong : strongCandidates)
            if (key != strong && key.startsWith(strong + '/')) { covered = true; break; }
        if (!covered) distinct.append(item);
    }
    combined = distinct;
    if (combined.size() == 1 && strongCandidates.contains(
            combined.first().toObject()["scope"].toString() + '/' + combined.first().toObject()["path"].toString()) &&
        config.mappings.isEmpty()) {
        const auto item = combined.first().toObject();
        config.mappings.append(SaveMapping{item["scope"].toString(), item["path"].toString()});
        config.discoveryState = "identified"; config.discoverySource = "observed";
        config.candidates = {};
    } else if (!combined.isEmpty()) config.candidates = combined;
    validate(lib, game, config);
    writeJson(configPath(lib, game.id), configJson(config));
    if (!QFile::remove(observationPath(lib, game.id))) fail("Cannot clear completed save observation.");
}
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
void inspectNode(const QString &path, const QString &relative, QJsonArray &entries) {
    const QFileInfo info(path);
    if (info.isSymbolicLink()) fail("Save mapping contains a symbolic link: " + path);
    if (info.isDir()) {
        entries.append(QJsonObject{{"path", relative}, {"type", "directory"}});
        const QDir dir(path);
        for (const auto &child : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name))
            inspectNode(child.absoluteFilePath(), relative.isEmpty() ? child.fileName() : relative + '/' + child.fileName(), entries);
    } else if (info.isFile()) {
        entries.append(QJsonObject{{"path", relative}, {"type", "file"},
                                    {"size", static_cast<double>(info.size())}, {"sha256", QString::fromLatin1(digest(path))}});
    } else fail("Save mapping is neither a regular file nor a directory: " + path);
}
QJsonArray inventory(const QString &path) {
    QJsonArray entries;
    if (exists(path)) inspectNode(path, QString(), entries);
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
SaveSnapshot snapshotLocked(Library &lib, const Game &game, const SaveConfig &config, const QString &reason) {
    validate(lib, game, config);
    if (config.mappings.isEmpty()) fail("Map at least one save file or folder before creating a snapshot.");
    const auto root = snapshotsRoot(lib, game.id);
    noLinks(root);
    if (!QDir().mkpath(root)) fail("Cannot create save snapshot storage.");
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
        if (result.files == 0) throw NoSaveData();
        QString parent;
        QString head = currentHead(lib, game.id);
        if (head.isEmpty()) {
            const auto prior = listSaveSnapshots(lib, game.id);
            if (!prior.isEmpty()) head = prior.first().id; // Legacy libraries had no explicit applied head.
        }
        if (!head.isEmpty()) {
            const auto older = readJson(root + '/' + head + "/manifest.json");
            if (older["id"].toString() != head) fail("Current save snapshot is missing or invalid.");
            verifySnapshot(root + '/' + head, older);
            if (older["schema"].toInt() != 2 ||
                !profileGameMatches(config, older["profile_game_id"].toString()) ||
                older["game_identity"].toString() != config.gameIdentity ||
                older["release_identity"].toString() != config.releaseIdentity ||
                older["save_compatibility"].toString() != config.saveCompatibility) head.clear();
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
    try {
        const auto snapshot = snapshotLocked(lib, game, config, "after play");
        queuePeerSync(lib, game.id, snapshot.id);
    }
    catch (const NoSaveData &) { /* A confirmed, empty root is not a failed Play. */ }
    if (!QFile::remove(pendingPath(lib, game.id))) fail("Cannot clear pending save snapshot record.");
}
struct SaveLocks {
    QLockFile mutation, launch;
    SaveLocks(const Library &lib, const QString &id)
        : mutation(lib.dataRoot() + "/mutation.lock"), launch(lib.gamePath(id, "launch.lock")) {
        mutation.setStaleLockTime(0); launch.setStaleLockTime(0);
        if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry shortly.");
        if (!QDir().mkpath(QFileInfo(launch.fileName()).absolutePath()) || !launch.tryLock())
            fail("This game is running or being prepared. Close it before changing saves.");
        if (!runtimeRecoveryIssue(lib, id).isEmpty()) fail("Resolve the game's interrupted launch before changing saves.");
    }
};
QJsonObject snapshotManifest(const Library &lib, const QString &id, const QString &snapshotId) {
    if (!validToken(snapshotId)) fail("Invalid save snapshot ID.");
    const auto root = snapshotsRoot(lib, id) + '/' + snapshotId;
    noLinks(root);
    const auto manifest = readJson(root + "/manifest.json");
    if (manifest["id"].toString() != snapshotId) fail("Save snapshot ID does not match its manifest.");
    verifySnapshot(root, manifest);
    return manifest;
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
QString stageSuffix(const QString &nonce, const QString &type) { return ".novelty-" + type + "-" + nonce; }
void rollback(const Library &lib, const Game &game, const QJsonObject &journal) {
    const auto nonce = journal["nonce"].toString();
    if (journal["schema"].toInt() != 1 || !validToken(nonce) || !journal["items"].isArray()) fail("Invalid restore journal; inspect saves before playing.");
    const auto snapshot = snapshotManifest(lib, game.id, journal["snapshot"].toString());
    const auto backupId = journal["backup"].toString();
    const auto backup = backupId.isEmpty() ? QJsonObject{} : snapshotManifest(lib, game.id, backupId);
    for (const auto &value : journal["items"].toArray()) {
        const auto item = value.toObject();
        const SaveMapping mapping{item["scope"].toString(), item["path"].toString()};
        const auto target = targetPath(lib, game, mapping);
        const auto held = target + stageSuffix(nonce, "held"), staged = target + stageSuffix(nonce, "incoming");
        noLinks(held); noLinks(staged);
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
        if (exists(held)) {
            if (exists(target) && inventory(target) != incomingEntries)
                fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
            if (exists(target)) removeNode(target);
            if (!QDir().rename(held, target)) fail("Cannot restore the original save at " + target);
        } else if (!exists(staged)) {
            // The incoming item was installed, and the held copy may already have been cleaned.
            // Restore from the verified safety snapshot if the target still has the incoming bytes.
            const auto currentEntries = inventory(target);
            if (currentEntries == originalEntries) continue; // This item was not changed yet.
            if (currentEntries != incomingEntries)
                fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
            if (exists(target)) removeNode(target);
            if (!backupId.isEmpty()) {
                const auto original = snapshotsRoot(lib, game.id) + '/' + backupId + "/items/" + QString::number(index);
                if (exists(original)) copyNode(original, target);
            } else if (!originalEntries.isEmpty()) {
                for (const auto &entry : originalEntries) {
                    const auto path = entry.toObject()["path"].toString();
                    if (entry.toObject()["type"] != "directory" ||
                        (!path.isEmpty() && cleanMappingPath(path) != path) ||
                        !QDir().mkpath(path.isEmpty() ? target : target + '/' + path))
                        fail("Cannot recreate the original empty save folder.");
                }
            }
        } else if (inventory(target) != originalEntries) {
            fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
        }
        if (exists(staged)) removeNode(staged);
    }
    const auto previousHead = journal["previous_head"].toString();
    setHead(lib, game.id, previousHead.isEmpty() ? backupId : previousHead);
    if (!QFile::remove(journalPath(lib, game.id))) fail("Cannot clear save restore journal.");
}
} // namespace

SaveConfig loadSaveConfig(const Library &lib, const QString &gameId) {
    const auto game = lib.game(gameId);
    auto config = parseConfig(readJson(configPath(lib, gameId)), gameId);
    if (config.profileId.isEmpty()) config.profileId = lib.profileId();
    validate(lib, game, config);
    return config;
}
QString saveRecipePath(const Library &lib, const SaveConfig &config) { return recipePath(lib, config); }
QString saveMappingRoot(const Library &lib, const Game &game, const QString &scope) {
    const auto root = mappingRoot(lib, game, scope); noLinks(root); return root;
}
SaveMapping saveMappingFromPath(const Library &lib, const Game &game, const QString &path) {
    const auto input = path.trimmed();
    if (input.isEmpty()) fail("Choose a save file or folder first.");
    const auto checked = [&](const SaveMapping &mapping) {
        targetPath(lib, game, mapping);
        return mapping;
    };
    auto gameFolder = QDir(lib.root()).relativeFilePath(game.folder);
    if (gameFolder == "." || gameFolder == ".." || gameFolder.startsWith("../"))
        gameFolder = QFileInfo(game.folder).fileName();
    const auto displayRoot = '/' + QDir::cleanPath(gameFolder);
    if (input.startsWith(displayRoot + '/'))
        return checked({"game", input.mid(displayRoot.size() + 1)});
    for (const auto &scope : {QString("appdata"), QString("documents"), QString("prefix")}) {
        const auto label = scope == "appdata" ? "AppData:/" : scope == "documents" ? "Documents:/" : "Prefix:/";
        if (input.startsWith(label, Qt::CaseInsensitive))
            return checked({scope, input.mid(QString(label).size())});
    }
    if (!QDir::isAbsolutePath(input)) return checked({"game", input});
    const auto absolute = QDir::cleanPath(input);
    const auto below = [&](const QString &scope) -> QString {
        const auto root = QDir::cleanPath(mappingRoot(lib, game, scope)) + '/';
        return absolute.startsWith(root) ? absolute.mid(root.size()) : QString();
    };
    if (const auto relative = below("game"); !relative.isEmpty()) return checked({"game", relative});
    for (const auto &scope : {QString("appdata"), QString("documents")}) {
        try {
            if (const auto relative = below(scope); !relative.isEmpty()) return checked({scope, relative});
        } catch (const std::exception &) { /* A prefix-relative path can still be used with several Wine users. */ }
    }
    if (const auto relative = below("prefix"); !relative.isEmpty()) return checked({"prefix", relative});
    fail("Choose a path inside this game's folder or Wine prefix.");
}
void beginSaveObservation(Library &lib, const Game &game) {
    if (exists(observationPath(lib, game.id))) evaluateObservation(lib, game);
    auto config = loadSaveConfig(lib, game.id);
    resolveDeclaredSavePaths(lib, game, config);
    if (config.discoveryState != "searching") return;
    writeJson(observationPath(lib, game.id), {{"schema", 1}, {"game_id", game.id},
        {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"roots", observedState(lib, game)}});
}
void finishSaveObservation(Library &lib, const Game &game) { evaluateObservation(lib, game); }
QStringList recoverSaveObservations(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        if (!exists(observationPath(lib, game.id)) || lib.supervisorActive(game.id)) continue;
        try {
            SaveLocks locks(lib, game.id);
            evaluateObservation(lib, game);
            messages.append("Checked save discovery after the last Play for " + game.name + ".");
        } catch (const std::exception &e) {
            messages.append("Save discovery for " + game.name + " needs attention: " + QString::fromUtf8(e.what()));
        }
    }
    return messages;
}
QList<SaveMapping> suggestedSaveMappings(const Library &lib, const QString &gameId) {
    const auto game = lib.game(gameId);
    QList<SaveMapping> suggestions;
    if (game.engine != "Ren'Py" && game.setupProfile != "renpy-bundled-v1") return suggestions;
    const auto populated = [](const QString &path) {
        const QFileInfo info(path);
        return info.isDir() && !info.isSymbolicLink() &&
            !QDir(path).entryList(QDir::Files | QDir::NoSymLinks | QDir::Hidden).isEmpty();
    };
    if (populated(game.folder + "/game/saves")) suggestions.append(SaveMapping{"game", "game/saves"});
    const auto users = lib.prefix(gameId) + "/drive_c/users";
    if (QFileInfo(users).isDir() && !QFileInfo(users).isSymbolicLink()) {
        for (const auto &user : QDir(users).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
            const auto renpy = user.absoluteFilePath() + "/AppData/Roaming/RenPy";
            if (!QFileInfo(renpy).isDir() || QFileInfo(renpy).isSymbolicLink()) continue;
            for (const auto &folder : QDir(renpy).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name))
                if (populated(folder.absoluteFilePath()))
                    suggestions.append(SaveMapping{"appdata", "RenPy/" + folder.fileName()});
        }
    }
    return suggestions;
}
void saveSaveConfig(Library &lib, const QString &gameId, const SaveConfig &config) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before editing mappings.");
    if (!pendingSaveSnapshotIssue(lib, gameId).isEmpty()) fail("Finish the pending post-play snapshot before editing save mappings.");
    const auto game = lib.game(gameId);
    auto updated = config;
    if (updated.profileId.isEmpty()) updated.profileId = lib.profileId();
    if (updated.profileGameId.isEmpty()) updated.profileGameId = loadSaveConfig(lib, gameId).profileGameId;
    if (!updated.mappings.isEmpty() && updated.discoveryState == "searching" && updated.discoverySource.isEmpty()) {
        updated.discoveryState = "identified"; updated.discoverySource = "manual";
    }
    if (updated.discoveryState == "identified") updated.candidates = {};
    validate(lib, game, updated);
    writeJson(configPath(lib, gameId), configJson(updated));
}
void linkPeerSaveGame(Library &lib, const QString &gameId, const QString &peerFingerprint,
                      const QString &peerProfileGameId) {
    if (!validFingerprint(peerFingerprint) || !validToken(peerProfileGameId) ||
        !isTrustedPeer(lib, peerFingerprint)) fail("Pair and verify this device before linking its game.");
    auto config = loadSaveConfig(lib, gameId);
    if (config.gameIdentity.isEmpty() || config.releaseIdentity.isEmpty() ||
        config.saveCompatibility.isEmpty() || config.mappings.isEmpty())
        fail("Confirm the game, edition, save format and save paths before linking a peer game.");
    if (peerProfileGameId == config.profileGameId) return;
    bool found = false;
    for (auto &link : config.peerLinks) if (link.fingerprint == peerFingerprint) {
        link.profileGameId = peerProfileGameId; found = true; break;
    }
    if (!found) config.peerLinks.append({peerFingerprint, peerProfileGameId});
    saveSaveConfig(lib, gameId, config);
}
void setSaveDiscoveryState(Library &lib, const QString &gameId, const QString &state) {
    if (state != "searching" && state != "unidentified") fail("Choose Rescan or Stop searching.");
    auto config = loadSaveConfig(lib, gameId);
    config.discoveryState = state;
    if (state == "unidentified") { config.mappings.clear(); config.discoverySource = "user"; }
    else { config.mappings.clear(); config.discoverySource.clear(); config.candidates = {}; }
    saveSaveConfig(lib, gameId, config);
}
SaveSnapshot createSaveSnapshot(Library &lib, const QString &gameId, const QString &reason) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before creating a snapshot.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    const auto snapshot = snapshotLocked(lib, game, config, reason.left(80));
    queuePeerSync(lib, gameId, snapshot.id);
    return snapshot;
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
    verifySnapshot(folder, manifest);
    return folder;
}
QStringList saveSnapshotLineage(const Library &lib, const QString &gameId, int maxSnapshots) {
    if (maxSnapshots < 1 || maxSnapshots > 256) fail("Invalid save history limit.");
    QStringList newestFirst;
    QSet<QString> seen;
    auto id = appliedSaveSnapshotId(lib, gameId);
    while (!id.isEmpty() && newestFirst.size() < maxSnapshots) {
        if (!validToken(id) || seen.contains(id)) fail("Save history contains an invalid parent.");
        seen.insert(id);
        const auto manifest = snapshotManifest(lib, gameId, id);
        if (manifest["schema"].toInt() != 2) break;
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
        const auto manifest = snapshotManifest(lib, gameId, snapshot.id);
        if (manifest["schema"].toInt() != 2 || manifest["profile_id"].toString() == config.profileId ||
            !profileGameMatches(config, manifest["profile_game_id"].toString()) ||
            manifest["game_identity"].toString() != config.gameIdentity ||
            manifest["release_identity"].toString() != config.releaseIdentity ||
            manifest["save_compatibility"].toString() != config.saveCompatibility || snapshot.id == head)
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
void restoreSaveSnapshot(Library &lib, const QString &gameId, const QString &snapshotId) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before restoring another snapshot.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    const auto manifest = snapshotManifest(lib, gameId, snapshotId);
    if (manifest["schema"].toInt() == 2 &&
        (!profileGameMatches(config, manifest["profile_game_id"].toString()) ||
         manifest["game_identity"].toString() != config.gameIdentity ||
         manifest["release_identity"].toString() != config.releaseIdentity ||
         manifest["save_compatibility"].toString() != config.saveCompatibility))
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
        writeJson(journalPath(lib, gameId), journal);
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto staged = target + stageSuffix(nonce, "incoming");
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) fail("Cannot create save parent folder: " + target);
            const auto source = snapshotsRoot(lib, gameId) + '/' + snapshotId + "/items/" + QString::number(i);
            if (exists(source)) copyNode(source, staged);
            if (inventory(staged) != items[i].toObject()["entries"].toArray()) fail("Cannot verify staged save restore.");
        }
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto held = target + stageSuffix(nonce, "held"), staged = target + stageSuffix(nonce, "incoming");
            if (exists(target) && !QDir().rename(target, held)) fail("Cannot hold original save: " + target);
            if (exists(staged) && !QDir().rename(staged, target)) fail("Cannot install restored save: " + target);
        }
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto held = target + stageSuffix(nonce, "held");
            if (exists(held)) removeNode(held);
        }
        setHead(lib, gameId, snapshotId);
        if (!QFile::remove(journalPath(lib, gameId))) fail("Cannot clear completed save restore journal.");
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
SaveImportResult importSaveSnapshot(Library &lib, const QString &gameId, const QString &bundleDirectory,
                                    const QString &authenticatedPeer) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before importing saves.");
    const auto game = lib.game(gameId);
    const auto config = loadSaveConfig(lib, gameId);
    if (config.gameIdentity.isEmpty() || config.releaseIdentity.isEmpty() || config.saveCompatibility.isEmpty() ||
        config.mappings.isEmpty()) fail("Confirm this game's identity, edition, compatibility and save paths before syncing.");
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
        manifest["game_identity"].toString() != config.gameIdentity ||
        manifest["release_identity"].toString() != config.releaseIdentity ||
        manifest["save_compatibility"].toString() != config.saveCompatibility)
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
QString saveRestoreIssue(const Library &lib, const QString &gameId) {
    return exists(journalPath(lib, gameId)) ? "An interrupted save restore needs recovery before Play." : QString();
}
QString pendingSaveSnapshotIssue(const Library &lib, const QString &gameId) {
    return exists(pendingPath(lib, gameId)) ? "A post-play save snapshot is pending. Use Rescan to retry it before Play." : QString();
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
QStringList recoverSaveRestores(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        if (!exists(journalPath(lib, game.id))) continue;
        if (lib.supervisorActive(game.id)) continue;
        try {
            SaveLocks locks(lib, game.id);
            rollback(lib, game, readJson(journalPath(lib, game.id)));
            messages.append("Recovered the original saves for " + game.name + " after an interrupted restore.");
        } catch (const std::exception &e) {
            messages.append("Save restore recovery for " + game.name + " needs attention: " + QString::fromUtf8(e.what()));
        }
    }
    return messages;
}
} // namespace novelty
