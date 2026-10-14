#include "recipes.h"
#include "setup.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qmap.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool validId(const QString &id) {
    static const QRegularExpression pattern("^[a-z0-9][a-z0-9._-]{0,79}$");
    return pattern.match(id).hasMatch();
}
bool validRelative(const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(QChar::Null)) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}
bool validTier(const QString &tier) { return tier == "default" || tier == "engines" || tier == "games"; }
QString checkedPath(const QString &root, const QString &relative) {
    if (!validRelative(relative)) fail("Recipe paths must be relative to the game folder.");
    const auto path = QDir(root).filePath(relative);
    const auto canonicalRoot = canonicalPath(root);
    if (QFileInfo::exists(path) && !canonicalPath(path).startsWith(canonicalRoot + '/'))
        fail("A recipe path resolves outside the game folder.");
    return path;
}
bool globMatch(const QString &pattern, const QString &relative) {
    const auto matches = [&](const QString &candidatePattern) {
        const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(candidatePattern),
                                             QRegularExpression::CaseInsensitiveOption);
        return expression.match(relative).hasMatch() ||
            expression.match(QFileInfo(relative).fileName()).hasMatch();
    };
    return matches(pattern) || (pattern.startsWith("*/") && matches(pattern.mid(2)));
}
QList<int> versionParts(const QString &text) {
    QList<int> parts;
    for (const auto &piece : text.split('.')) {
        bool okay = false; const int number = piece.toInt(&okay);
        if (!okay || number < 0) return {};
        parts.append(number);
    }
    return parts;
}
bool newer(const QList<int> &candidate, const QList<int> &current) {
    if (candidate.isEmpty()) return false;
    for (qsizetype i = 0; i < std::max(candidate.size(), current.size()); ++i) {
        const int left = i < candidate.size() ? candidate[i] : 0;
        const int right = i < current.size() ? current[i] : 0;
        if (left != right) return left > right;
    }
    return false;
}
bool conditionMatches(const QJsonObject &condition, const QString &base, const QStringList &executables) {
    if (condition.size() != 1) return false;
    if (condition.contains("file")) return QFileInfo(checkedPath(base, condition["file"].toString())).isFile();
    if (condition.contains("directory")) return QFileInfo(checkedPath(base, condition["directory"].toString())).isDir();
    if (condition.contains("file_glob")) {
        const auto pattern = condition["file_glob"].toString();
        if (!validRelative(pattern)) return false;
        const auto slash = pattern.lastIndexOf('/');
        const auto parent = slash < 0 ? base : checkedPath(base, pattern.left(slash));
        const auto name = pattern.mid(slash + 1);
        if (parent.contains('*') || parent.contains('?') || !QFileInfo(parent).isDir()) return false;
        for (const auto &file : QDir(parent).entryList(QDir::Files | QDir::NoSymLinks))
            if (globMatch(name, file)) return true;
        return false;
    }
    if (condition.contains("native_executable")) {
        if (condition["native_executable"] != true) return false;
        bool native = false, windows = false;
        for (const auto &path : executables) {
            if (QFileInfo(path).suffix().compare("exe", Qt::CaseInsensitive) == 0) windows = true;
            else if (nativeExecutable(path)) native = true;
        }
        return native && !windows;
    }
    return false;
}
QByteArray seed(const QString &id, const QString &name, const QJsonObject &match, const QJsonObject &settings,
                const QString &parent = {}) {
    QJsonObject object{{"schema", 1}, {"id", id}, {"name", name}, {"match", match}, {"settings", settings}};
    if (!parent.isEmpty()) object["inherits"] = parent;
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}
void writeNew(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size())
        fail("Cannot create recipe " + path);
}
}

QString recipeRoot(const QString &libraryRoot) { return libraryRoot + "/.novelty/recipes"; }
QString recipePath(const QString &libraryRoot, const QString &tier, const QString &id) {
    if (!validTier(tier) || !validId(id)) fail("Invalid recipe location or ID.");
    return recipeRoot(libraryRoot) + '/' + tier + '/' + id + ".json";
}
void ensureRecipeCatalog(const QString &libraryRoot) {
    const auto root = recipeRoot(libraryRoot);
    for (const auto &tier : {"default", "engines", "games"})
        if (!QDir().mkpath(root + '/' + tier)) fail("Cannot create recipe directory.");
    const auto marker = root + "/catalog-v1";
    const auto runtimeMap = libraryRoot + "/.novelty/runtime-appids.json";
    if (!QFileInfo::exists(runtimeMap))
        writeNew(runtimeMap, QJsonDocument(QJsonObject{{"1391110", "steamrt2"}, {"1628350", "steamrt3"},
            {"4183110", "steamrt4"}, {"4185400", "steamrt4-arm64"}}).toJson(QJsonDocument::Indented));
    if (QFileInfo::exists(marker)) return;
    const QJsonObject generic{{"backend", "umu"}, {"setup_profile", "generic-game-v1"},
        {"runner", "GE-Proton11-7"},
        {"ignore_executables", QJsonArray{"unins*", "*uninstall*", "setup.exe", "install.exe",
            "python*.exe", "zsync*.exe", "crashreporter.exe", "*/lib/*", "*/ost/*", "*/soundtrack/*"}}};
    const QJsonObject vn{{"setup_profile", "generic-proton-v1"}};
    const QJsonObject renpy{{"setup_profile", "renpy-bundled-v1"}, {"save_strategy", "renpy"}};
    const QJsonObject native{{"backend", "native"}, {"ignore_executables", generic["ignore_executables"]}};
    const struct { const char *tier, *id, *name; QJsonObject match, settings; const char *parent; } builtins[] = {
        {"default", "generic-game-v1", "Generic game", {}, generic, ""},
        {"default", "generic-vn-v1", "Generic VN", {}, vn, "generic-game-v1"},
        {"engines", "renpy-bundled-v1", "Ren'Py", {{"root", "executable_ancestor"},
            {"all", QJsonArray{QJsonObject{{"file", "renpy/__init__.py"}}, QJsonObject{{"directory", "game"}}}}}, renpy, "generic-vn-v1"},
        {"engines", "kirikiri-candidate-v1", "Kirikiri candidate", {{"root", "executable_ancestor"},
            {"all", QJsonArray{QJsonObject{{"file_glob", "*.xp3"}}}}}, {}, "generic-vn-v1"},
        {"engines", "siglus-candidate-v1", "Siglus candidate", {{"root", "executable_ancestor"},
            {"all", QJsonArray{QJsonObject{{"file", "SiglusEngine.exe"}}}}}, {}, "generic-vn-v1"},
        {"engines", "native-linux-v1", "Native Linux", {{"all", QJsonArray{QJsonObject{{"native_executable", true}}}}}, native, ""}
    };
    for (const auto &entry : builtins) {
        const auto path = recipePath(libraryRoot, entry.tier, entry.id);
        if (!QFileInfo::exists(path)) writeNew(path, seed(entry.id, entry.name, entry.match, entry.settings, entry.parent));
    }
    writeNew(marker, "Recipe catalog initialized. User edits are preserved.\n");
}
Recipe parseRecipe(const QByteArray &json, const QString &path, const QString &tier) {
    if (!validTier(tier) || json.size() > 256 * 1024) fail("Invalid or oversized recipe: " + path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Invalid recipe JSON: " + path);
    const auto object = document.object();
    const QSet<QString> topLevel{"schema", "id", "name", "inherits", "match", "settings"};
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!topLevel.contains(it.key())) fail("Unknown recipe field: " + it.key());
    const auto id = object["id"].toString(), name = object["name"].toString().trimmed();
    if (object["schema"].toInt(-1) != 1 || !validId(id) || name.isEmpty() || name.size() > 120 ||
        !object["settings"].isObject() || (object.contains("match") && !object["match"].isObject()))
        fail("Unsupported or incomplete recipe: " + path);
    if (object.contains("inherits") && (!object["inherits"].isString() || !validId(object["inherits"].toString()) ||
        object["inherits"].toString() == id)) fail("Invalid recipe parent: " + path);
    const auto match = object["match"].toObject(), settings = object["settings"].toObject();
    for (auto it = match.begin(); it != match.end(); ++it)
        if (it.key() != "root" && it.key() != "all") fail("Unknown recipe match field: " + it.key());
    if (match.contains("all") && !match["all"].isArray()) fail("Invalid recipe match list: " + path);
    if (tier != "default" && (!match["all"].isArray() || match["all"].toArray().isEmpty()))
        fail("An engine or game recipe needs match conditions: " + path);
    if (tier == "default" && !match["all"].toArray().isEmpty())
        fail("A default recipe cannot have match conditions: " + path);
    if (match.contains("root") && match["root"] != "game" && match["root"] != "executable_ancestor")
        fail("Invalid recipe match root: " + path);
    for (const auto &value : match["all"].toArray()) {
        const auto condition = value.toObject();
        if (!value.isObject() || condition.size() != 1) fail("Invalid recipe match condition: " + path);
        const auto key = condition.begin().key();
        if (key == "native_executable") { if (condition[key] != true) fail("Invalid native match condition: " + path); }
        else if ((key != "file" && key != "directory" && key != "file_glob") ||
                 !condition[key].isString() || !validRelative(condition[key].toString()))
            fail("Invalid recipe match path: " + path);
    }
    const QSet<QString> allowedSettings{"executable", "working_directory", "runner", "umu_launcher", "umu_game_id",
        "store", "backend", "setup_profile", "ignore_executables", "saves", "save_strategy", "arguments", "environment", "locale", "winetricks"};
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (!allowedSettings.contains(it.key())) fail("Unknown recipe setting: " + it.key());
    for (const auto &key : {"executable", "working_directory", "runner", "umu_launcher", "umu_game_id", "store", "backend", "setup_profile", "locale"})
        if (settings.contains(key) && !settings[key].isString()) fail("Invalid recipe setting: " + path);
    if (settings.contains("arguments")) {
        if (!settings["arguments"].isArray()) fail("Invalid recipe arguments: " + path);
        for (const auto &value : settings["arguments"].toArray()) if (!value.isString() || value.toString().contains(QChar::Null))
            fail("Invalid recipe argument: " + path);
    }
    if (settings.contains("environment")) {
        if (!settings["environment"].isObject()) fail("Invalid recipe environment: " + path);
        const auto environment = settings["environment"].toObject();
        for (auto it = environment.begin(); it != environment.end(); ++it)
            if (!it.value().isString() || it.key().isEmpty() || it.key().contains('=') || it.key().contains(QChar::Null) ||
                it.value().toString().contains(QChar::Null)) fail("Invalid recipe environment variable: " + path);
    }
    static const QRegularExpression umuId("^[A-Za-z0-9_-]{1,80}$");
    if (settings.contains("umu_game_id") && !settings["umu_game_id"].toString().isEmpty() &&
        !umuId.match(settings["umu_game_id"].toString()).hasMatch()) fail("Invalid umu game ID: " + path);
    for (const auto &key : {"executable", "working_directory"})
        if (settings.contains(key) && !validRelative(settings[key].toString())) fail("Invalid relative recipe path: " + path);
    if (settings.contains("backend") && !QStringList{"umu", "native", "steam"}.contains(settings["backend"].toString()))
        fail("Invalid recipe backend: " + path);
    if (settings.contains("save_strategy") && settings["save_strategy"] != "renpy")
        fail("Invalid recipe save strategy: " + path);
    if (settings.contains("winetricks")) {
        if (!settings["winetricks"].isArray()) fail("Winetricks must be an array: " + path);
        QStringList verbs;
        for (const auto &value : settings["winetricks"].toArray()) {
            if (!value.isString()) fail("Winetricks verbs must be strings: " + path);
            verbs.append(value.toString());
        }
        validateWinetricks(verbs);
    }
    if (settings.contains("ignore_executables")) {
        if (!settings["ignore_executables"].isArray()) fail("Invalid executable exclusions: " + path);
        for (const auto &value : settings["ignore_executables"].toArray())
            if (!value.isString() || value.toString().isEmpty() || value.toString().contains(".."))
                fail("Invalid executable exclusion: " + path);
    }
    if (settings.contains("saves")) {
        if (!settings["saves"].isArray()) fail("Invalid recipe save paths: " + path);
        for (const auto &value : settings["saves"].toArray()) {
            const auto item = value.toObject();
            if (!value.isObject() || !QStringList{"game", "appdata", "documents", "prefix"}.contains(item["scope"].toString()) ||
                !validRelative(item["path"].toString())) fail("Invalid recipe save path: " + path);
        }
    }
    return {id, name, tier, path, match, settings,
        QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex(),
        object["inherits"].toString(), settings.contains("runner")};
}
QList<Recipe> loadRecipes(const QString &libraryRoot) {
    ensureRecipeCatalog(libraryRoot);
    QList<Recipe> recipes;
    for (const auto &tier : {"default", "engines", "games"}) {
        const auto directory = recipeRoot(libraryRoot) + '/' + tier;
        for (const auto &name : QDir(directory).entryList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
            const auto path = directory + '/' + name;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) fail("Cannot read recipe " + path);
            auto recipe = parseRecipe(file.read(256 * 1024 + 1), path, tier);
            if (name != recipe.id + ".json") fail("Recipe filename and ID differ: " + path);
            for (const auto &other : recipes) if (other.id == recipe.id) fail("Duplicate recipe ID: " + recipe.id);
            recipes.append(recipe);
        }
    }
    QMap<QString, int> index;
    for (int i = 0; i < recipes.size(); ++i) index.insert(recipes[i].id, i);
    QSet<QString> resolving, resolved;
    std::function<void(int, int)> resolve = [&](int i, int depth) {
        auto &recipe = recipes[i];
        if (resolved.contains(recipe.id)) return;
        if (depth > 16 || resolving.contains(recipe.id)) fail("Recipe inheritance has a cycle or exceeds 16 levels: " + recipe.id);
        resolving.insert(recipe.id);
        if (!recipe.inherits.isEmpty()) {
            if (!index.contains(recipe.inherits)) fail("Missing recipe parent: " + recipe.inherits);
            const auto parentIndex = index.value(recipe.inherits);
            resolve(parentIndex, depth + 1);
            const auto &parent = recipes[parentIndex];
            auto merged = parent.settings;
            for (auto it = recipe.settings.begin(); it != recipe.settings.end(); ++it) {
                if (it.key() == "environment") {
                    auto environment = merged["environment"].toObject();
                    const auto child = it.value().toObject();
                    for (auto item = child.begin(); item != child.end(); ++item) environment[item.key()] = item.value();
                    merged["environment"] = environment;
                } else if (it.key() == "winetricks") {
                    QJsonArray verbs; QSet<QString> seen;
                    for (const auto &source : {merged["winetricks"].toArray(), it.value().toArray()})
                        for (const auto &value : source) if (!seen.contains(value.toString())) {
                            seen.insert(value.toString()); verbs.append(value);
                        }
                    merged["winetricks"] = verbs;
                } else merged[it.key()] = it.value();
            }
            recipe.settings = merged;
            recipe.revision = QCryptographicHash::hash(parent.revision + ':' + recipe.revision,
                QCryptographicHash::Sha256).toHex();
        }
        if (recipe.inherits.isEmpty() && recipe.settings.contains("winetricks")) {
            QJsonArray unique; QSet<QString> seen;
            for (const auto &value : recipe.settings["winetricks"].toArray()) if (!seen.contains(value.toString())) {
                seen.insert(value.toString()); unique.append(value);
            }
            recipe.settings["winetricks"] = unique;
        }
        if (!recipe.settings["winetricks"].toArray().isEmpty() &&
            recipe.settings["backend"].toString("umu") != "umu")
            fail("Winetricks requires the umu backend: " + recipe.id);
        resolving.remove(recipe.id); resolved.insert(recipe.id);
    };
    for (int i = 0; i < recipes.size(); ++i) resolve(i, 0);
    return recipes;
}
bool recipeMatches(const Recipe &recipe, const QString &gameFolder, const QStringList &executables) {
    const auto all = recipe.match["all"].toArray();
    if (all.isEmpty()) return recipe.tier == "default";
    const auto relevantExecutables = recipeExecutables(recipe, gameFolder, executables);
    QStringList roots{gameFolder};
    if (recipe.match["root"] == "executable_ancestor") {
        roots.clear();
        for (const auto &executable : executables) {
            auto path = QFileInfo(executable).absolutePath();
            while (path == gameFolder || path.startsWith(gameFolder + '/')) {
                if (!roots.contains(path)) roots.append(path);
                if (path == gameFolder) break;
                path = QFileInfo(path).absolutePath();
            }
        }
    }
    for (const auto &root : roots) {
        bool complete = true;
        for (const auto &value : all) if (!conditionMatches(value.toObject(), root, relevantExecutables)) {
            complete = false; break;
        }
        if (complete) return true;
    }
    return false;
}
RecipeSelection selectRecipe(const QString &libraryRoot, const QString &gameFolder,
                             const QStringList &executables, const QString &defaultId) {
    const auto recipes = loadRecipes(libraryRoot);
    for (const auto &tier : {"games", "engines"}) {
        RecipeSelection selection; selection.tier = tier;
        for (const auto &recipe : recipes) if (recipe.tier == tier && recipeMatches(recipe, gameFolder, executables))
            selection.matches.append(recipe);
        if (!selection.empty()) return selection;
    }
    RecipeSelection selection; selection.tier = "default";
    for (const auto &recipe : recipes) if (recipe.tier == "default" && recipe.id == defaultId)
        selection.matches.append(recipe);
    if (selection.empty()) fail("The selected default recipe is missing: " + defaultId);
    return selection;
}
QStringList recipeExecutables(const Recipe &recipe, const QString &gameFolder, const QStringList &found) {
    const auto explicitPath = recipe.settings["executable"].toString();
    if (!explicitPath.isEmpty()) {
        const auto path = checkedPath(gameFolder, explicitPath);
        return QFileInfo(path).isFile() ? QStringList{canonicalPath(path)} : QStringList{};
    }
    QStringList filtered;
    for (const auto &path : found) {
        const auto relative = QDir(gameFolder).relativeFilePath(path);
        bool ignored = false;
        for (const auto &value : recipe.settings["ignore_executables"].toArray())
            if (globMatch(value.toString(), relative)) { ignored = true; break; }
        if (!ignored) filtered.append(path);
    }
    QStringList windows, native;
    for (const auto &path : filtered)
        (QFileInfo(path).suffix().compare("exe", Qt::CaseInsensitive) == 0 ? windows : native).append(path);
    return windows.isEmpty() ? native : windows;
}
void applyRecipeToGame(Game &game, const Recipe &recipe) {
    const auto &settings = recipe.settings;
    game.recipeId = recipe.id; game.recipeRevision = QString::fromLatin1(recipe.revision);
    game.recipeCandidates.clear(); game.engine = recipe.tier == "engines" ? recipe.name : "Generic";
    game.evidence = "Recipe: " + recipe.name;
    game.backend = settings["backend"].toString("umu");
    game.setupProfile = settings["setup_profile"].toString("generic-proton-v1");
    game.runnerSelector = settings["runner"].toString();
    game.umuLauncher = settings["umu_launcher"].toString();
    game.umuResolved = game.umuLauncher.isEmpty() ? QString() :
        resolveUmuSelector(QFileInfo(game.folder).absolutePath(), game.umuLauncher);
    game.umuGameId = settings["umu_game_id"].toString();
    game.store = settings["store"].toString();
    game.recipeSaves = settings["saves"].toArray();
    game.winetricks.clear();
    for (const auto &value : settings["winetricks"].toArray()) game.winetricks.append(value.toString());
    game.saveStrategy = settings["save_strategy"].toString();
    if (settings.contains("locale")) game.locale = settings["locale"].toString();
    if (settings.contains("arguments")) {
        game.arguments.clear();
        for (const auto &value : settings["arguments"].toArray()) game.arguments.append(value.toString());
    }
    if (settings.contains("environment")) {
        game.environment.clear();
        const auto environment = settings["environment"].toObject();
        for (auto it = environment.begin(); it != environment.end(); ++it)
            game.environment.insert(it.key(), it.value().toString());
    }
    if (const auto relative = settings["executable"].toString(); !relative.isEmpty())
        game.executable = canonicalPath(checkedPath(game.folder, relative));
    if (const auto relative = settings["working_directory"].toString(); !relative.isEmpty())
        game.workingDirectory = canonicalPath(checkedPath(game.folder, relative));
}
void saveRecipe(const QString &libraryRoot, const QString &tier, const QByteArray &json) {
    const auto recipe = parseRecipe(json, {}, tier);
    const auto path = recipePath(libraryRoot, tier, recipe.id);
    if (QFileInfo(path).isSymbolicLink()) fail("A recipe cannot be a symbolic link.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        fail("Cannot save recipe " + path);
}
void removeRecipe(const QString &libraryRoot, const QString &tier, const QString &id) {
    const auto path = recipePath(libraryRoot, tier, id);
    if (!QFileInfo(path).isFile() || QFileInfo(path).isSymbolicLink() || !QFile::remove(path))
        fail("Cannot remove recipe " + path);
}
QString resolveRunnerSelector(const QString &libraryRoot, const QString &selector) {
    if (selector.isEmpty()) return {};
    if (QDir::isAbsolutePath(selector))
        return QFileInfo(QDir(selector).filePath("proton")).isFile() ? canonicalPath(selector) : QString();
    const auto runners = installedRunners(libraryRoot);
    QString winner; QList<int> best;
    for (const auto &runner : runners) {
        const auto name = QFileInfo(runner).fileName();
        if (selector == "proton-experimental") {
            if (name.contains("Experimental", Qt::CaseInsensitive)) return runner;
        } else if (selector == "proton-latest") {
            static const QRegularExpression stable("^Proton[ -]([0-9]+(?:[.][0-9]+)*)", QRegularExpression::CaseInsensitiveOption);
            const auto match = stable.match(name);
            if (match.hasMatch()) {
                const auto version = versionParts(match.captured(1));
                if (!version.isEmpty() && (winner.isEmpty() || newer(version, best))) { winner = runner; best = version; }
            }
        } else if (selector == "ge-proton-latest") {
            static const QRegularExpression ge("^GE-Proton([0-9]+)-([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
            const auto match = ge.match(name);
            if (match.hasMatch()) {
                const QList<int> version{match.captured(1).toInt(), match.captured(2).toInt()};
                if (winner.isEmpty() || newer(version, best)) { winner = runner; best = version; }
            }
        } else if (name == selector) return runner;
    }
    return winner;
}
QString resolveUmuSelector(const QString &libraryRoot, const QString &selector) {
    if (selector.isEmpty()) return {};
    if (QDir::isAbsolutePath(selector)) return resolveProgram(selector);
    const auto tools = libraryRoot + "/.novelty/tools";
    QString winner; QList<int> best;
    for (const auto &entry : QDir(tools).entryInfoList({"umu-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const auto name = entry.fileName();
        const auto path = resolveProgram(entry.filePath() + "/umu-run");
        if (path.isEmpty()) continue;
        if (selector == name) return path;
        if (selector == "umu-latest") {
            const auto version = versionParts(name.mid(4));
            if (!version.isEmpty() && (winner.isEmpty() || newer(version, best)))
                { winner = path; best = version; }
        }
    }
    return winner;
}
} // namespace novelty
