#include "save_internal.h"

namespace novelty {
namespace save_detail {
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
    if (config.serverGameKey.toUtf8().size() > 256) fail("Server game link is too long.");
    for (const auto ch : config.serverGameKey)
        if (ch.isNull() || ch.category() == QChar::Other_Control)
            fail("Server game link contains an invalid character.");
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
            {"save_compatibility", config.saveCompatibility}, {"server_game_key", config.serverGameKey},
            {"discovery_state", config.discoveryState},
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
        config.serverGameKey = data["server_game_key"].toString();
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
void resolveDeclaredSavePaths(Library &lib, const Game &game, SaveConfig &config) {
    if (config.discoveryState != "searching" || !config.mappings.isEmpty()) return;
    if (!game.recipeSaves.isEmpty()) {
        for (const auto &value : game.recipeSaves) {
            const auto item = value.toObject();
            config.mappings.append({item["scope"].toString(), item["path"].toString()});
        }
        config.discoveryState = "identified"; config.discoverySource = "recipe";
        validate(lib, game, config);
        writeJson(configPath(lib, game.id), configJson(config));
        return;
    }
    auto recipe = recipeMappings(lib, config);
    if (!recipe.isEmpty()) {
        config.mappings = recipe; config.discoveryState = "identified"; config.discoverySource = "recipe";
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
} // namespace save_detail
using namespace save_detail;

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
} // namespace novelty
