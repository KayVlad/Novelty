#include "saves.h"
#include "devices.h"
#include "sync_queue.h"
#include "runtime.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/quuid.h>
#include <QtCore/qsysinfo.h>
#include <QtCore/qset.h>
#include <sys/statvfs.h>
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
        if (exists(headPath(lib, id)) && !QFile::remove(headPath(lib, id))) fail("Cannot clear the current save snapshot pointer.");
        return;
    }
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
    if (config.folderAliases.size() > 8) fail("Too many previous game folder names.");
    for (const auto &alias : config.folderAliases)
        if (alias.isEmpty() || alias.size() > 100 || alias.contains('/') || alias.contains(QChar::Null))
            fail("A previous game folder name is invalid.");
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
            {"discovery_source", config.discoverySource}, {"candidates", config.candidates},
            {"folder_aliases", QJsonArray::fromStringList(config.folderAliases)}, {"mappings", mappings},
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
        if (data.contains("folder_aliases") && !data["folder_aliases"].isArray()) fail("Previous folder names are damaged.");
        for (const auto &value : data["folder_aliases"].toArray()) {
            if (!value.isString()) fail("Previous folder name is damaged.");
            config.folderAliases.append(value.toString());
        }
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
QString saveNameKey(const QString &name) {
    QString key;
    for (const auto ch : name.normalized(QString::NormalizationForm_KC).toCaseFolded())
        if (ch.isLetterOrNumber()) key.append(ch);
    return key;
}
QList<SaveMapping> heuristicSaveMappings(const Library &lib, const Game &game) {
    QList<SaveMapping> found;
    QSet<QString> paths;
    const auto add = [&](const QString &absolute) {
        if (!QFileInfo(absolute).isDir() || QFileInfo(absolute).isSymbolicLink()) return;
        try {
            const auto mapping = saveMappingFromPath(lib, game, absolute);
            if (!paths.contains(absolute)) { paths.insert(absolute); found.append(mapping); }
        } catch (const std::exception &) { /* Unsafe or unsupported paths are never candidates. */ }
    };
    const QStringList conventional = {"save", "saves", "savedata", "save_data", "userdata", "user_data",
                                      "game/saves", "game/savedata"};
    QStringList bases{game.folder};
    const auto executableDirectory = QFileInfo(game.executable).absolutePath();
    if (executableDirectory.startsWith(game.folder + '/') && executableDirectory != game.folder)
        bases.append(executableDirectory);
    for (const auto &base : bases) {
        for (const auto &relative : conventional) {
            QStringList current{base};
            for (const auto &part : QString(relative).split('/')) {
                QStringList next;
                for (const auto &parent : current) {
                    if (QFileInfo(parent).isSymbolicLink()) continue;
                    for (const auto &child : QDir(parent).entryInfoList(QDir::Dirs | QDir::NoSymLinks |
                                                      QDir::NoDotAndDotDot, QDir::Name))
                        if (child.fileName().compare(part, Qt::CaseInsensitive) == 0)
                            next.append(child.absoluteFilePath());
                }
                current = next;
                if (current.isEmpty()) break;
            }
            for (const auto &path : current) add(path);
        }
    }

    QSet<QString> titles;
    for (const auto &name : {game.name, QFileInfo(game.folder).fileName(), game.metadata["title"].toString(),
                             game.metadata["original_title"].toString()})
        if (auto key = saveNameKey(name); key.size() >= 3) titles.insert(key);
    const auto stem = saveNameKey(QFileInfo(game.executable).completeBaseName());
    if (stem.size() >= 4 && !QSet<QString>{"game", "start", "launcher", "setup", "play", "autorun"}.contains(stem))
        titles.insert(stem);
    const auto users = lib.prefix(game.id) + "/drive_c/users";
    int userCount = 0;
    for (const auto &user : QDir(users).entryInfoList(QDir::Dirs | QDir::NoSymLinks |
                                                     QDir::NoDotAndDotDot, QDir::Name)) {
        if (user.fileName().compare("Public", Qt::CaseInsensitive) == 0 || ++userCount > 4) continue;
        for (const auto &relative : {"Saved Games", "Documents", "AppData/Roaming", "AppData/Local",
                                     "AppData/LocalLow"}) {
            const auto root = user.absoluteFilePath() + '/' + relative;
            if (!QFileInfo(root).isDir() || QFileInfo(root).isSymbolicLink()) continue;
            int budget = 512;
            const auto walk = [&](auto &&self, const QString &directory, int depth) -> void {
                if (depth > 4 || budget <= 0) return;
                for (const auto &child : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoSymLinks |
                                                                  QDir::NoDotAndDotDot, QDir::Name)) {
                    if (--budget < 0) break;
                    const auto key = saveNameKey(child.fileName());
                    if (QSet<QString>{"cache", "caches", "logs", "temp", "tmp", "shadercache"}.contains(key)) continue;
                    if (titles.contains(key))
                        add(child.absoluteFilePath());
                    else if (depth < 4) self(self, child.absoluteFilePath(), depth + 1);
                }
            };
            walk(walk, root, 1);
        }
    }
    return found;
}
void discoverSavePaths(Library &lib, const Game &game) {
    auto config = loadSaveConfig(lib, game.id);
    if (!config.mappings.isEmpty()) return;
    if (config.discoveryState == "unidentified") {
        config.discoveryState = "searching";
        config.discoverySource.clear();
    }
    resolveDeclaredSavePaths(lib, game, config);
    if (config.discoveryState == "identified") return;
    const auto found = heuristicSaveMappings(lib, game);
    config.candidates = {}; // Old observation suggestions are not trusted as confirmed paths.
    if (found.size() == 1) {
        config.mappings = found;
        config.discoveryState = "identified";
        config.discoverySource = "heuristic";
    }
    validate(lib, game, config);
    writeJson(configPath(lib, game.id), configJson(config));
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
constexpr qint64 maxSnapshotBytes = 4LL * 1024 * 1024 * 1024;
struct SnapshotBudget { qint64 bytes = 0; int entries = 0; };
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
QJsonArray inventory(const QString &path, SnapshotBudget *budget = nullptr, bool hashes = true) {
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
SaveSnapshot snapshotLocked(Library &lib, const Game &game, const SaveConfig &config, const QString &reason) {
    validate(lib, game, config);
    if (config.mappings.isEmpty()) fail("Map at least one save file or folder before creating a snapshot.");
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
        if (result.files == 0) throw NoSaveData();
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
    try {
        const auto snapshot = snapshotLocked(lib, game, config, "after play");
        queuePeerSync(lib, game.id, snapshot.id);
    }
    catch (const NoSaveData &) { /* A confirmed, empty root is not a failed Play. */ }
    if (!QFile::remove(pendingPath(lib, game.id))) fail("Cannot clear pending save snapshot record.");
    if (readJson(lib.gamePath(game.id, "status.json"))["state"] == "Save needs attention")
        lib.recordStatus(game.id, "Ready", "Pending save backup completed.");
}
struct SaveLocks {
    QLockFile mutation, launch;
    SaveLocks(const Library &lib, const QString &id, int mutationWaitMs = 0)
        : mutation(lib.dataRoot() + "/mutation.lock"), launch(lib.gamePath(id, "launch.lock")) {
        mutation.setStaleLockTime(0); launch.setStaleLockTime(0);
        if (!mutation.tryLock(mutationWaitMs)) fail("Library maintenance is in progress. Retry shortly.");
        if (!QDir().mkpath(QFileInfo(launch.fileName()).absolutePath()) || !launch.tryLock())
            fail("This game is busy. Wait for its current operation before changing saves.");
        if (!runtimeRecoveryIssue(lib, id).isEmpty()) fail("Resolve the game's interrupted launch before changing saves.");
    }
};
QJsonObject snapshotManifest(const Library &lib, const QString &id, const QString &snapshotId, bool verifyContents = true) {
    if (!validToken(snapshotId)) fail("Invalid save snapshot ID.");
    const auto root = snapshotsRoot(lib, id) + '/' + snapshotId;
    noLinks(root);
    const auto manifest = readJson(root + "/manifest.json");
    if (manifest["id"].toString() != snapshotId) fail("Save snapshot ID does not match its manifest.");
    if (verifyContents) verifySnapshot(root, manifest);
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
    const auto phase = journal["phase"].toString();
    if (!phase.isEmpty() && phase != "committed") fail("Unknown restore recovery phase; inspect saves before playing.");
    const bool committed = phase == "committed";
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
            if (currentEntries == originalEntries) continue; // This item was not changed yet.
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
            // Keep the installed copy until the backup is fully staged, so another I/O failure
            // or kill can retry recovery without mistaking a partial copy for player edits.
            if (exists(target) && !QDir().rename(target, staged)) fail("Cannot hold the restored save during recovery.");
            if (exists(recoveryStage) && !QDir().rename(recoveryStage, target)) fail("Cannot install the original save during recovery.");
        } else if (inventory(target) != originalEntries) {
            fail("A save changed during restore recovery. Inspect " + target + " before retrying.");
        }
        if (exists(staged)) removeNode(staged);
    }
    const auto previousHead = journal["previous_head"].toString();
    setHead(lib, game.id, committed ? journal["snapshot"].toString() : previousHead.isEmpty() ? backupId : previousHead);
    if (!QFile::remove(journalPath(lib, game.id))) fail("Cannot clear save restore journal.");
}
} // namespace

QString normalizedSaveGameName(const QString &name) { return saveNameKey(name); }

SaveConfig loadSaveConfig(const Library &lib, const QString &gameId) {
    if (exists(lib.gamePath(gameId, "settings-change.json")))
        fail("An interrupted settings change needs recovery. Use Rescan before changing saves.");
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
    discoverSavePaths(lib, game);
}
void finishSaveObservation(Library &lib, const Game &game) { discoverSavePaths(lib, game); }
QList<SaveMapping> saveDiscoveryCandidates(const Library &lib, const QString &gameId) {
    if (!loadSaveConfig(lib, gameId).mappings.isEmpty()) return {};
    return heuristicSaveMappings(lib, lib.game(gameId));
}
QStringList unresolvedGameSaveLocations(const Library &lib, const QString &gameId) {
    const auto game = lib.game(gameId);
    if (!loadSaveConfig(lib, gameId).mappings.isEmpty()) return {};
    const auto candidates = heuristicSaveMappings(lib, game);
    if (candidates.size() < 2) return {};
    QStringList paths;
    for (const auto &candidate : candidates)
        if (candidate.scope == "game") paths.append(candidate.path);
    paths.removeDuplicates();
    return paths;
}
QStringList recoverSaveObservations(Library &lib) {
    QStringList messages;
    for (const auto &game : lib.games()) {
        if (!exists(observationPath(lib, game.id)) || lib.supervisorActive(game.id)) continue;
        try {
            SaveLocks locks(lib, game.id);
            discoverSavePaths(lib, game);
            if (!QFile::remove(observationPath(lib, game.id))) fail("Cannot clear obsolete save observation.");
            messages.append("Updated save discovery for " + game.name + ".");
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
    SaveLocks locks(lib, gameId, 2000);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before editing mappings.");
    if (!pendingSaveSnapshotIssue(lib, gameId).isEmpty()) fail("Finish the pending post-play snapshot before editing save mappings.");
    if (exists(lib.gamePath(gameId, "settings-change.json"))) fail("Use Rescan to recover the interrupted settings change.");
    writeJson(configPath(lib, gameId), validatedSaveConfig(lib, lib.game(gameId), config));
}
QJsonObject validatedSaveConfig(const Library &lib, const Game &game, const SaveConfig &config) {
    const auto gameId = game.id;
    auto updated = config;
    if (updated.profileId.isEmpty()) updated.profileId = lib.profileId();
    if (updated.profileGameId.isEmpty()) updated.profileGameId = loadSaveConfig(lib, gameId).profileGameId;
    if (!updated.mappings.isEmpty() && updated.discoveryState == "searching" && updated.discoverySource.isEmpty()) {
        updated.discoveryState = "identified"; updated.discoverySource = "manual";
    }
    if (updated.discoveryState == "identified") updated.candidates = {};
    validate(lib, game, updated);
    return configJson(updated);
}
void linkPeerSaveGame(Library &lib, const QString &gameId, const QString &peerFingerprint,
                      const QString &peerProfileGameId) {
    if (!validFingerprint(peerFingerprint) || !validToken(peerProfileGameId) ||
        !isTrustedPeer(lib, peerFingerprint)) fail("Pair and verify this device before linking its game.");
    auto config = loadSaveConfig(lib, gameId);
    if (config.mappings.isEmpty()) fail("Choose this game's save location before linking peer saves.");
    if (peerProfileGameId == config.profileGameId) return;
    for (const auto &other : lib.games()) {
        if (other.id == gameId) continue;
        const auto otherConfig = loadSaveConfig(lib, other.id);
        for (const auto &link : otherConfig.peerLinks)
            if (link.fingerprint == peerFingerprint && link.profileGameId == peerProfileGameId)
                fail("This peer game is already linked to another local game.");
    }
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
        const auto snapshot = snapshotLocked(lib, game, config, reason.left(80));
        queuePeerSync(lib, gameId, snapshot.id);
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
void restoreSaveSnapshot(Library &lib, const QString &gameId, const QString &snapshotId) {
    SaveLocks locks(lib, gameId);
    if (!saveRestoreIssue(lib, gameId).isEmpty()) fail("Recover the interrupted save restore before restoring another snapshot.");
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
        auto committed = journal; committed["phase"] = "committed";
        writeJson(journalPath(lib, gameId), committed);
        setHead(lib, gameId, snapshotId);
        for (int i = 0; i < items.size(); ++i) {
            const auto target = targetPath(lib, game, config.mappings[i]);
            const auto held = target + stageSuffix(nonce, "held");
            if (exists(held)) removeNode(held);
        }
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
QString saveRestoreIssue(const Library &lib, const QString &gameId) {
    return exists(journalPath(lib, gameId)) ? "An interrupted save restore needs recovery before Play." : QString();
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
} // namespace novelty
