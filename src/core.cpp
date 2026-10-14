#include "core.h"
#include "setup.h"
#include "profiles.h"
#include "runtime.h"
#include "saves.h"
#include "recipes.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/quuid.h>
#include <QtSql/qsqlerror.h>
#include <QtSql/qsqlquery.h>
#include <algorithm>
#include <stdexcept>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void check(QSqlQuery &q) { if (!q.exec()) fail(q.lastError().text()); }
void sql(QSqlDatabase db, const QString &text) { QSqlQuery q(db); if (!q.exec(text)) fail(q.lastError().text()); }
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
bool safeId(const QString &id) { return !QUuid(id).isNull(); }
QString movedPath(const QString &path, const QString &oldFolder, const QString &newFolder) {
    if (path.isEmpty()) return {};
    if (!QDir::isAbsolutePath(path)) return path;
    const auto relative = QDir(oldFolder).relativeFilePath(path);
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) return path;
    return QDir::cleanPath(QDir(newFolder).filePath(relative));
}
bool unusedInstallation(const Library &lib, const Game &g, bool ownLock = false) {
    if (g.source != "discovered" || QFileInfo::exists(lib.prefix(g.id))) return false;
    auto files = QDir(QFileInfo(lib.gamePath(g.id, "launch.lock")).absolutePath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (ownLock) files.removeAll("launch.lock");
    return files.isEmpty();
}
QStringList findExecutables(const QString &root, QStringList *watched, bool *native) {
    QStringList windowsFiles, linuxFiles, folders{root};
    for (qsizetype index = 0; index < folders.size(); ++index) {
        const auto path = folders[index];
        if (watched && watched->size() < 256) watched->append(path);
        const QDir dir(path);
        for (const auto &file : dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
            const bool windows = file.suffix().compare("exe", Qt::CaseInsensitive) == 0;
            if (!windows && !nativeExecutable(file.absoluteFilePath())) continue;
            (windows ? windowsFiles : linuxFiles).append(file.absoluteFilePath());
        }
        for (const auto &sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
            if (sub.fileName().startsWith('.')) continue; // Never inspect Novelty's hidden state or hidden bundles.
            folders.append(sub.absoluteFilePath());
        }
    }
    *native = windowsFiles.isEmpty() && !linuxFiles.isEmpty();
    windowsFiles.append(linuxFiles);
    return windowsFiles;
}
QByteArray metadataFingerprint(const QString &folder) {
    // Read names/stat data only, in the discovery worker. Never read game assets.
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    QStringList entries;
    const QDir root(folder);
    while (it.hasNext()) {
        const auto f = it.nextFileInfo();
        entries.append(root.relativeFilePath(f.absoluteFilePath()) + QChar::Null +
                       QString::number(f.size()) + QChar::Null +
                       QString::number(f.lastModified().toMSecsSinceEpoch()) + QChar::Null +
                       (f.isDir() ? "d" : "f"));
    }
    entries.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &entry : entries) { hash.addData(entry.toUtf8()); hash.addData(QByteArrayView("\n")); }
    return hash.result();
}
void rememberSteamRequest(const Library &lib, const QString &id) {
    if (readJson(lib.gamePath(id, "status.json"))["state"] == "Requested in Steam" &&
        !QFileInfo::exists(lib.gamePath(id, "steam-request.json")))
        writeJson(lib.gamePath(id, "steam-request.json"), {{"state", "untracked"}, {"updated", now()}});
}

}

QString canonicalPath(const QString &path) {
    const auto info = QFileInfo(path);
    return info.canonicalFilePath().isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : info.canonicalFilePath();
}
bool nativeExecutable(const QString &path) {
    const QFileInfo info(path);
    const auto name = info.fileName().toLower();
    if (info.isSymLink() || !info.isFile() || !info.isExecutable() ||
        name.endsWith(".so") || name.contains(".so.")) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto header = file.read(4);
    return header.startsWith(QByteArrayView("\x7f" "ELF", 4)) || header.startsWith("#!");
}
QString folderIdentity(const QString &path) {
#ifdef __linux__
    struct statx info{};
    const auto encoded = QFile::encodeName(path);
    const auto required = STATX_TYPE | STATX_INO | STATX_BTIME;
    if (statx(AT_FDCWD, encoded.constData(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, required, &info) == 0 &&
        (info.stx_mask & required) == required && S_ISDIR(info.stx_mode))
        return QString("%1:%2:%3:%4:%5").arg(info.stx_dev_major).arg(info.stx_dev_minor)
            .arg(info.stx_ino).arg(info.stx_btime.tv_sec).arg(info.stx_btime.tv_nsec);
#else
    Q_UNUSED(path);
#endif
    // No trustworthy identity: require explicit relinking rather than guessing.
    return {};
}
QString launchPathIssue(const Game &game) {
    if (game.backend != "umu") return {};
    for (const auto &path : {game.executable, game.workingDirectory}) {
        if (path.isEmpty()) continue;
        for (const auto &part : canonicalPath(path).split('/', Qt::SkipEmptyParts)) {
            QString reason;
            for (const auto ch : part) if (QString("<>:\"\\|?*").contains(ch) || ch.unicode() < 32) {
                reason = "contains a Windows-reserved character"; break;
            }
            if (part.endsWith('.') || part.endsWith(' ')) reason = "ends with a period or space";
            const auto base = part.section('.', 0, 0).toUpper();
            if (QStringList{"CON", "PRN", "AUX", "NUL"}.contains(base) ||
                (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) && QString("123456789¹²³").contains(base.back())))
                reason = "is a Windows-reserved device name";
            if (!reason.isEmpty()) return "The folder/file name “" + part + "” " + reason +
                ". Rename it, then Rescan or relink the game. Your prefix and saves can stay in place.";
        }
    }
    return {};
}
QString resolveProgram(const QString &program) {
    if (program.contains('/')) {
        QFileInfo f(program);
        return f.isFile() && f.isExecutable() ? f.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(program);
}
QString managedUmuPath(const QString &root) {
    return root + "/.novelty/tools/umu-1.4.4/umu-run";
}
QJsonObject Game::json() const {
    QJsonObject env;
    for (auto it = environment.begin(); it != environment.end(); ++it) env.insert(it.key(), it.value());
    return {{"schema", 1}, {"id", id}, {"name", name}, {"folder", folder},
            {"executable", executable}, {"cwd", workingDirectory}, {"runner", runner},
            {"locale", locale}, {"backend", backend}, {"steam_id", steamId}, {"runner_revision", runnerRevision},
            {"folder_identity", folderIdentity}, {"source", source},
            {"installer", installer}, {"install_pending", installPending},
            {"engine", engine}, {"evidence", evidence}, {"setup_profile", setupProfile},
            {"recipe_id", recipeId}, {"recipe_revision", recipeRevision},
            {"recipe_candidates", QJsonArray::fromStringList(recipeCandidates)},
            {"winetricks", QJsonArray::fromStringList(winetricks)},
            {"recipe_saves", recipeSaves},
            {"runner_selector", runnerSelector}, {"umu_launcher", umuLauncher}, {"umu_resolved", umuResolved},
            {"umu_game_id", umuGameId}, {"store", store}, {"save_strategy", saveStrategy},
            {"arguments", QJsonArray::fromStringList(arguments)}, {"environment", env}, {"metadata", metadata}};
}
Game Game::fromJson(const QJsonObject &j) {
    if (j.value("schema").toInt() != 1) fail("Unsupported game configuration version.");
    Game g;
    g.id = j["id"].toString(); g.name = j["name"].toString(); g.folder = j["folder"].toString();
    g.executable = j["executable"].toString(); g.workingDirectory = j["cwd"].toString();
    g.runner = j["runner"].toString(); g.locale = j["locale"].toString();
    g.runnerRevision = j["runner_revision"].toString();
    g.installer = j["installer"].toString(); g.installPending = j["install_pending"].toBool();
    g.folderIdentity = j["folder_identity"].toString(); g.source = j["source"].toString("manual");
    g.backend = j["backend"].toString("umu"); g.steamId = j["steam_id"].toString();
    g.engine = j["engine"].toString("Generic"); g.evidence = j["evidence"].toString();
    g.metadata = j["metadata"].toObject();
    // Legacy installations retain their exact generic baseline, regardless of detected engine.
    g.setupProfile = j.contains("setup_profile") ? j["setup_profile"].toString() : "generic-proton-v1";
    g.recipeId = j["recipe_id"].toString(); g.recipeRevision = j["recipe_revision"].toString();
    for (const auto &value : j["recipe_candidates"].toArray()) g.recipeCandidates.append(value.toString());
    for (const auto &value : j["winetricks"].toArray()) g.winetricks.append(value.toString());
    g.recipeSaves = j["recipe_saves"].toArray();
    g.runnerSelector = j["runner_selector"].toString(); g.umuLauncher = j["umu_launcher"].toString();
    g.umuResolved = j["umu_resolved"].toString();
    g.umuGameId = j["umu_game_id"].toString(); g.store = j["store"].toString();
    g.saveStrategy = j["save_strategy"].toString();
    for (auto a : j["arguments"].toArray()) g.arguments.append(a.toString());
    const auto env = j["environment"].toObject();
    for (auto it = env.begin(); it != env.end(); ++it) g.environment.insert(it.key(), it.value().toString());
    if (!safeId(g.id)) fail("Invalid installation ID.");
    return g;
}
namespace {
QString storedPath(const QString &root, const QString &path) {
    if (path == root) return ".";
    if (path.startsWith(root + '/')) return path.mid(root.size() + 1);
    return path; // Explicit external paths and command names remain unchanged.
}
QString resolvedPath(const QString &root, const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path)) return path;
    if (path.split('/', Qt::KeepEmptyParts).contains("..")) fail("A stored library path escapes the Games folder.");
    return QDir::cleanPath(QDir(root).filePath(path));
}
QJsonObject storedGame(const QString &root, const Game &game) {
    auto data = game.json();
    for (const auto &key : {"folder", "executable", "cwd", "runner", "installer", "umu_resolved"})
        data[key] = storedPath(root, data[key].toString());
    return data;
}
Game resolvedGame(const QString &root, const QJsonObject &data) {
    auto game = Game::fromJson(data);
    game.folder = resolvedPath(root, game.folder);
    game.executable = resolvedPath(root, game.executable);
    game.workingDirectory = resolvedPath(root, game.workingDirectory);
    game.runner = resolvedPath(root, game.runner);
    game.installer = resolvedPath(root, game.installer);
    game.umuResolved = resolvedPath(root, game.umuResolved);
    return game;
}
QJsonObject storedLibrarySettings(const QString &root, QJsonObject data) {
    for (const auto &key : {"runner", "umu", "steam"})
        if (data[key].isString()) {
            const auto original = data[key].toString();
            auto path = storedPath(root, original);
            if (path != original && !path.contains('/') && path != ".") path.prepend("./");
            data[key] = path;
        }
    return data;
}
QJsonObject resolvedLibrarySettings(const QString &root, QJsonObject data) {
    for (const auto &key : {"runner", "umu", "steam"})
        if (data[key].isString()) {
            const auto path = data[key].toString();
            if ((QString::fromLatin1(key) == "runner" && !path.isEmpty()) || path == "." || path.contains('/'))
                data[key] = resolvedPath(root, path);
        }
    return data;
}
}
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.exists()) return {};
    if (!f.open(QIODevice::ReadOnly)) fail("Cannot read " + path + ": " + f.errorString());
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) fail("Invalid JSON in " + path);
    return doc.object();
}
void writeJson(const QString &path, const QJsonObject &data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) fail("Cannot create directory for " + path);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) fail("Cannot write " + path + ": " + f.errorString());
    const auto bytes = QJsonDocument(data).toJson();
    if (f.write(bytes) != bytes.size() || !f.commit()) fail("Cannot save " + path + ": " + f.errorString());
}
Candidate inspect(const QString &folder, const QString &libraryRoot) {
    Candidate c;
    c.folder = canonicalPath(folder); c.name = QFileInfo(c.folder).fileName();
    const auto all = findExecutables(c.folder, &c.watchDirectories, &c.native);
    const auto root = libraryRoot.isEmpty() ? QFileInfo(c.folder).absolutePath() : libraryRoot;
    const auto selection = selectRecipe(root, c.folder, all, "generic-game-v1");
    if (!selection.empty() && !selection.ambiguous()) {
        const auto recipe = selection.selected();
        c.executables = recipeExecutables(recipe, c.folder, all);
        c.native = recipe.settings["backend"] == "native";
        c.engine = recipe.tier == "engines" ? recipe.name : "Generic";
        c.evidence = "Recipe: " + recipe.name;
    } else {
        c.executables = all;
        c.evidence = "Several recipes match; choose one in Manage.";
    }
    return c;
}
QList<Candidate> discover(const QString &root) {
    if (!QFileInfo(root).isDir()) fail("Games folder is unavailable: " + root);
    QList<Candidate> out;
    QDir d(root);
    for (auto f : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (f.fileName().startsWith('.')) continue;
        if (QFileInfo::exists(f.absoluteFilePath() + "/.novelty-transfer.json")) continue;
        auto c = inspect(f.absoluteFilePath());
        if (!c.executables.isEmpty()) out.append(c);
    }
    return out;
}
DiscoverySnapshot scanDiscovery(const QString &root, const QStringList &knownFolders) {
    if (!QFileInfo(root).isDir()) fail("Games folder is unavailable: " + root);
    DiscoverySnapshot result; result.watchDirectories.append(root);
    for (const auto &known : knownFolders) if (QFileInfo(known).isDir()) {
        result.watchDirectories.append(known); result.folderIdentities.insert(known, folderIdentity(known));
    }
    for (const auto &folder : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (folder.fileName().startsWith('.')) continue;
        const auto path = canonicalPath(folder.absoluteFilePath());
        if (QFileInfo::exists(path + "/.novelty-transfer.json")) continue;
        result.folderIdentities.insert(path, folderIdentity(path));
        if (knownFolders.contains(path)) { result.watchDirectories.append(path); continue; }
        auto candidate = inspect(path);
        // Also watch empty/unrecognised folders so a later executable can trigger discovery.
        result.watchDirectories.append(candidate.watchDirectories);
        if (candidate.executables.isEmpty()) continue;
        candidate.fingerprint = metadataFingerprint(path);
        result.candidates.append(candidate);
    }
    result.watchDirectories.removeDuplicates();
    return result;
}
QList<Candidate> CopyTracker::observe(const QList<Candidate> &candidates, qint64 elapsedMs, qint64 quietMs) {
    QList<Candidate> ready;
    QMap<QString, Observation> next;
    for (const auto &candidate : candidates) {
        const auto old = pending_.constFind(candidate.folder);
        Observation observation{candidate.fingerprint, elapsedMs};
        if (old != pending_.cend() && old->fingerprint == candidate.fingerprint) observation = *old;
        if (elapsedMs - observation.changedAt >= quietMs) ready.append(candidate);
        else next.insert(candidate.folder, observation);
    }
    pending_ = std::move(next);
    return ready;
}
QStringList installedRunners(const QString &libraryRoot) {
    QStringList out;
    const auto base = QDir::homePath() + "/.local/share/Steam";
    QStringList locations{base + "/compatibilitytools.d", base + "/steamapps/common"};
    if (!libraryRoot.isEmpty()) locations.prepend(QDir(libraryRoot).filePath(".novelty/runners"));
    for (const auto &dir : locations) {
        for (auto f : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (QFileInfo(QDir(f.filePath()).filePath("proton")).isFile()) out.append(f.canonicalFilePath());
        }
    }
    out.removeDuplicates(); return out;
}
QString runnerVersion(const QString &runner) {
    QFile f(QDir(runner).filePath("version"));
    if (f.open(QIODevice::ReadOnly)) return QString::fromUtf8(f.read(4096)).trimmed();
    return QFileInfo(runner).fileName();
}

Library::Library(QString root) : root_(canonicalPath(root)), connection_(QUuid::createUuid().toString()) {
    if (!QFileInfo(root_).isDir()) fail("Games folder is unavailable: " + root_);
    if (!QDir().mkpath(dataRoot())) fail("Games folder is not writable. Choose a writable folder for this prototype.");
    ensureRecipeCatalog(root_);
    const bool newDatabase = !QFileInfo::exists(dataRoot() + "/library.sqlite");
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(dataRoot() + "/library.sqlite");
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!db_.open()) fail(db_.lastError().text());
    QSqlQuery version(db_); if (!version.exec("PRAGMA user_version")) fail(version.lastError().text());
    version.first();
    if (version.value(0).toInt() > 2) fail("This library was created by a newer Novelty version.");
    const int schema = version.value(0).toInt(); version.finish();
    sql(db_, "PRAGMA synchronous=FULL");
    sql(db_, "CREATE TABLE IF NOT EXISTS games (id TEXT PRIMARY KEY, folder TEXT NOT NULL UNIQUE, config TEXT NOT NULL)");
    sql(db_, "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    {
        QSqlQuery profile(db_);
        profile.prepare("INSERT OR IGNORE INTO settings(key,value) VALUES('profile_id',?)");
        profile.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces)); check(profile);
    }
    if (newDatabase) sql(db_, "INSERT OR IGNORE INTO settings(key,value) VALUES('runtime_storage','managed')");
    if (schema < 2) {
        sql(db_, "BEGIN IMMEDIATE");
        try {
            sql(db_, "CREATE TABLE IF NOT EXISTS removals (game_id TEXT PRIMARY KEY, phase TEXT NOT NULL, manifest TEXT NOT NULL)");
            sql(db_, "PRAGMA user_version=2"); sql(db_, "COMMIT");
        } catch (...) { db_.rollback(); throw; }
    }
    rebaseMovedLibrary();
}
Library::~Library() {
    db_.close(); db_ = QSqlDatabase(); QSqlDatabase::removeDatabase(connection_);
}
QString Library::dataRoot() const { return root_ + "/.novelty"; }
QString Library::profileId() const {
    QSqlQuery q(db_); if (!q.exec("SELECT value FROM settings WHERE key='profile_id'")) fail(q.lastError().text());
    if (!q.next() || QUuid(q.value(0).toString()).isNull()) fail("Library profile identity is missing or invalid.");
    return q.value(0).toString();
}
QString Library::gamePath(const QString &id, const QString &file) const {
    if (!safeId(id) || file.contains('/') || file.contains("..")) fail("Invalid installation path.");
    return dataRoot() + "/installations/" + id + "/" + file;
}
QString Library::prefix(const QString &id) const {
    if (!safeId(id)) fail("Invalid installation ID.");
    return dataRoot() + "/prefixes/" + id;
}
void Library::rebaseMovedLibrary() {
    QSqlQuery format(db_);
    if (!format.exec("SELECT value FROM settings WHERE key='path_format'")) fail(format.lastError().text());
    if (format.next()) {
        if (format.value(0).toString() != "relative-v1") fail("Unsupported library path format.");
        return;
    }
    auto entries = games();
    // Legacy rows contain absolute paths. A missing direct child with the same
    // name under this root identifies the source of a moved library.
    QSet<QString> candidates;
    for (const auto &g : entries) {
        const auto parent = QFileInfo(g.folder).absolutePath();
        if (parent != root_ && !QFileInfo(g.folder).exists() &&
            QFileInfo(QDir(root_).filePath(QFileInfo(g.folder).fileName())).isDir())
            candidates.insert(parent);
    }
    if (candidates.size() > 1)
        fail("Several former Games folders match this library. Relocation needs manual review.");
    QString previous = candidates.isEmpty() ? root_ : *candidates.cbegin();
    if (previous.isEmpty()) previous = root_;
    previous = canonicalPath(previous);
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry opening the moved Games folder.");
    QSet<QString> discarded;
    QList<Game> updated;
    for (auto g : entries) {
        if (previous != root_ && (g.folder == previous || g.folder.startsWith(previous + '/'))) {
            if (QFileInfo(g.folder).absolutePath() != previous)
                fail("A registered game is not directly inside the former Games folder. Relink it manually before moving the library.");
            if (QFileInfo::exists(gamePath(g.id, "settings-change.json")) || busy(g.id))
                fail("Finish game activity and settings recovery before moving the Games folder.");
            const auto destination = QDir(root_).filePath(QFileInfo(g.folder).fileName());
            for (const auto &other : entries) if (other.id != g.id && other.folder == destination) {
                if (!unusedInstallation(*this, other))
                    fail("The moved library has two records for " + destination + ". Review the new entry before retrying relocation.");
                discarded.insert(other.id);
            }
            g.folder = destination;
            g.folderIdentity = folderIdentity(destination);
            g.executable = movedPath(g.executable, previous, root_);
            g.workingDirectory = movedPath(g.workingDirectory, previous, root_);
            g.runner = movedPath(g.runner, previous, root_);
            g.installer = movedPath(g.installer, previous, root_);
            g.umuResolved = movedPath(g.umuResolved, previous, root_);
        }
        updated.append(g);
    }
    auto app = settings();
    for (const auto &key : {"runner", "umu", "steam"})
        if (app[key].isString()) app[key] = movedPath(app[key].toString(), previous, root_);
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        for (const auto &id : discarded) {
            QSqlQuery remove(db_); remove.prepare("DELETE FROM games WHERE id=?");
            remove.addBindValue(id); check(remove);
        }
        for (const auto &g : updated) {
            if (discarded.contains(g.id)) continue;
            const auto data = storedGame(root_, g);
            QSqlQuery save(db_); save.prepare("UPDATE games SET folder=?, config=? WHERE id=?");
            save.addBindValue(data["folder"].toString());
            save.addBindValue(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
            save.addBindValue(g.id); check(save);
        }
        QSqlQuery existingApp(db_);
        if (!existingApp.exec("SELECT 1 FROM settings WHERE key='app'")) fail(existingApp.lastError().text());
        if (existingApp.next()) {
            QSqlQuery saveApp(db_);
            saveApp.prepare("UPDATE settings SET value=? WHERE key='app'");
            saveApp.addBindValue(QString::fromUtf8(QJsonDocument(storedLibrarySettings(root_, app)).toJson(QJsonDocument::Compact)));
            check(saveApp);
        }
        sql(db_, "INSERT INTO settings(key,value) VALUES('path_format','relative-v1')");
        sql(db_, "DELETE FROM settings WHERE key='library_root'");
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
}
QList<Game> Library::games() const {
    QList<Game> out; QSqlQuery q(db_);
    if (!q.exec("SELECT config FROM games ORDER BY json_extract(config, '$.name') COLLATE NOCASE")) fail(q.lastError().text());
    while (q.next()) out.append(resolvedGame(root_, QJsonDocument::fromJson(q.value(0).toByteArray()).object()));
    return out;
}
Game Library::game(const QString &id) const {
    QSqlQuery q(db_); q.prepare("SELECT config FROM games WHERE id=?"); q.addBindValue(id); check(q);
    if (!q.next()) fail("Game not found: " + id);
    return resolvedGame(root_, QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
void Library::save(Game g, const SaveConfig *saveConfig) {
    const bool creating = g.id.isEmpty();
    if (g.id.isEmpty()) g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!safeId(g.id) || g.name.trimmed().isEmpty()) fail("A game needs a valid ID and name.");
    if (g.backend == "umu") resolveSetupProfile(g);
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock(saveConfig ? 2000 : 0)) fail("Library maintenance is in progress. Retry saving shortly.");
    if (!creating) game(g.id); // A stale Manage window must not recreate a removed installation.
    if (busy(g.id)) fail("Wait until the game's launch or preparation finishes before editing it.");
    if (!QDir().mkpath(QFileInfo(gamePath(g.id, "launch.lock")).absolutePath())) fail("Cannot lock game settings.");
    QLockFile launch(gamePath(g.id, "launch.lock")); launch.setStaleLockTime(0);
    if (!launch.tryLock() || !recoveryIssue(g.id).isEmpty()) fail("This game is busy. Retry saving shortly.");
    if (!g.runner.isEmpty()) g.runnerRevision = runnerVersion(g.runner);
    g.folder = canonicalPath(g.folder);
    if (g.backend == "native" && !g.executable.isEmpty() &&
        (!nativeExecutable(g.executable) || !canonicalPath(g.executable).startsWith(g.folder + '/') ||
         !QFileInfo(g.workingDirectory).isDir()))
        fail("Choose an executable Linux game inside this game's folder and an existing working directory.");
    if (g.folderIdentity.isEmpty()) g.folderIdentity = folderIdentity(g.folder);
    g.source = "manual";
    if (!saveConfig) { store(g); return; }
    if (creating) fail("Create the game before configuring its saves.");
    if (!pendingSaveSnapshotIssue(*this, g.id).isEmpty()) fail("Finish the pending post-play snapshot before editing save mappings.");
    const auto before = game(g.id);
    const auto config = validatedSaveConfig(*this, g, *saveConfig);
    sql(db_, "BEGIN IMMEDIATE");
    try {
        store(g);
        if (before.json() != g.json()) stageSaveConfig(before, g, config);
        sql(db_, "COMMIT");
    } catch (...) {
        db_.rollback();
        try { recoverSettingsChange(g.id); } catch (...) { /* Keep the journal and block edits until Rescan. */ }
        throw;
    }
    if (before.json() == g.json()) writeJson(gamePath(g.id, "saves.json"), config);
    else QFile::remove(gamePath(g.id, "settings-change.json"));
}
void Library::stageSaveConfig(const Game &before, const Game &after, const QJsonObject &config) {
    const auto path = gamePath(after.id, "saves.json");
    // SQLite decides the outcome. If killed before COMMIT, recovery restores the old JSON;
    // after COMMIT it retains the new JSON. The game records must differ for this journal.
    writeJson(gamePath(after.id, "settings-change.json"), {{"schema", 1},
        {"before_game", before.json()}, {"after_game", after.json()},
        {"had_config", QFileInfo::exists(path)}, {"before_config", readJson(path)}, {"after_config", config}});
    writeJson(path, config);
}
void Library::recoverSettingsChange(const QString &id) {
    const auto path = gamePath(id, "settings-change.json");
    if (!QFileInfo::exists(path)) return;
    const auto journal = readJson(path);
    const auto before = journal["before_game"].toObject(), after = journal["after_game"].toObject();
    const auto current = game(id).json();
    if (journal["schema"] != 1 || before["id"] != id || after["id"] != id || before == after ||
        !journal["before_config"].isObject() || !journal["after_config"].isObject() ||
        (current != before && current != after)) fail("Settings recovery needs review; the game changed after an interrupted update.");
    const auto configPath = gamePath(id, "saves.json");
    if (current == before && !journal["had_config"].toBool()) {
        if (QFileInfo::exists(configPath) && !QFile::remove(configPath)) fail("Cannot undo interrupted save settings.");
    } else {
        const auto desired = journal[current == after ? "after_config" : "before_config"].toObject();
        if (readJson(configPath) != desired) writeJson(configPath, desired);
    }
    if (!QFile::remove(path)) fail("Cannot clear settings recovery journal. Use Rescan to retry.");
}
QStringList Library::recoverSettingsChanges() {
    QStringList messages;
    auto pending = games();
    pending.removeIf([&](const Game &g) { return !QFileInfo::exists(gamePath(g.id, "settings-change.json")); });
    if (pending.isEmpty()) return messages;
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) return messages;
    for (const auto &g : pending) {
        if (!QFileInfo::exists(gamePath(g.id, "settings-change.json"))) continue;
        QLockFile launch(gamePath(g.id, "launch.lock")); launch.setStaleLockTime(0);
        if (!launch.tryLock()) continue;
        try { recoverSettingsChange(g.id); messages.append("Recovered settings for " + g.name + "."); }
        catch (const std::exception &e) { messages.append(QString::fromUtf8(e.what())); }
    }
    return messages;
}
Game Library::selectExecutable(const QString &id, const QString &chosen) {
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry selecting the executable shortly.");
    auto g = game(id);
    if (busy(id)) fail("Wait until this game's installation or launch finishes before selecting an executable.");
    if (!g.executable.isEmpty()) fail("This game already has an executable. Change it in Manage → Launch.");
    if (!QFileInfo(g.folder).isDir() || (!g.folderIdentity.isEmpty() && folderIdentity(g.folder) != g.folderIdentity))
        fail("The game folder is missing or changed. Relink it before selecting an executable.");
    const QFileInfo selected(chosen);
    const auto path = canonicalPath(chosen);
    const bool windows = selected.suffix().compare("exe", Qt::CaseInsensitive) == 0;
    const bool nativeLauncher = nativeExecutable(chosen);
    if (selected.isSymLink() || !selected.isFile() || !(windows || nativeLauncher) ||
        !path.startsWith(g.folder + '/'))
        fail("Choose an existing Windows .exe or executable Linux game inside this game's folder.");
    g.executable = path;
    if (nativeLauncher) g.backend = "native";
    else if (windows && g.backend == "native") g.backend = "umu";
    g.workingDirectory = QFileInfo(path).absolutePath();
    bool defaultRecipe = g.recipeId.isEmpty();
    for (const auto &recipe : loadRecipes(root_))
        if (recipe.id == g.recipeId && recipe.tier == "default") defaultRecipe = true;
    if (defaultRecipe) {
        const auto choice = selectRecipe(root_, g.folder, {path}, settings()["default_recipe"].toString("generic-game-v1"));
        if (choice.ambiguous()) {
            g.recipeCandidates.clear();
            for (const auto &recipe : choice.matches) g.recipeCandidates.append(recipe.id);
        } else if (choice.selected().tier != "default") {
            applyRecipeToGame(g, choice.selected());
            g.executable = path; g.workingDirectory = QFileInfo(path).absolutePath();
        }
    }
    if (g.installPending) { g.installPending = false; g.installer.clear(); }
    if (const auto issue = launchPathIssue(g); !issue.isEmpty()) fail(issue);
    store(g);
    recordStatus(id, "Ready", "Game executable selected.");
    return g;
}
void Library::setMetadata(const QString &id, const QJsonObject &metadata) {
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry saving metadata shortly.");
    auto g = game(id); // Keep current launch settings even if a Manage dialog is open.
    g.metadata = metadata;
    store(g);
}
void Library::store(const Game &g) {
    if (!safeId(g.id) || g.name.trimmed().isEmpty()) fail("A game needs a valid ID and name.");
    validateWinetricks(g.winetricks);
    for (const auto &path : {g.folder, g.executable, g.workingDirectory, g.runner, g.installer})
        if (!path.isEmpty() && removalReservedFolder(path)) fail("This path is reserved by interrupted removal. Use Rescan to recover it first.");
    // Preserve the older adapter's untracked Steam request across backend changes.
    rememberSteamRequest(*this, g.id);
    QSqlQuery q(db_);
    q.prepare("INSERT INTO games(id,folder,config) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET folder=excluded.folder,config=excluded.config");
    const auto data = storedGame(root_, g);
    q.addBindValue(g.id); q.addBindValue(data["folder"].toString());
    q.addBindValue(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))); check(q);
}
RelinkPlan Library::planRelink(const QString &id, const QString &folder, const QString &executable, const QString &cwd) const {
    RelinkPlan plan; plan.original = game(id); plan.updated = plan.original;
    if (plan.original.backend == "steam") fail("Steam owns this game's shortcut paths. Folder relinking for Steam is a later increment.");
    if (busy(id)) fail("Wait until this game finishes before relinking its folder.");
    if (folder.trimmed().isEmpty() || !QFileInfo(folder).isDir()) fail("Choose an existing game folder.");
    auto &g = plan.updated; g.folder = canonicalPath(folder); g.folderIdentity = folderIdentity(g.folder);
    g.executable = executable.isEmpty() ? movedPath(g.executable, plan.original.folder, g.folder) : canonicalPath(executable);
    g.workingDirectory = cwd.isEmpty() ? movedPath(g.workingDirectory, plan.original.folder, g.folder) : canonicalPath(cwd);
    g.source = "manual";
    for (const auto &other : games()) if (other.id != id && other.folder == g.folder) {
        if (busy(other.id) || !unusedInstallation(*this, other))
            fail("This folder already belongs to " + other.name + ". Its configuration or setup is in use; choose another folder.");
        plan.duplicateId = other.id; plan.duplicateName = other.name; plan.duplicateConfig = other.json();
    }
    return plan;
}
void Library::relink(const RelinkPlan &plan) {
    const auto id = plan.original.id;
    if (!safeId(id) || plan.updated.id != id || plan.original.backend == "steam") fail("Invalid folder relink plan.");
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry relinking shortly.");
    if (!recoveryIssue(id).isEmpty()) fail(recoveryIssue(id));
    const auto lockDirectory = QFileInfo(gamePath(id, "launch.lock")).absolutePath();
    if (!QDir().mkpath(lockDirectory)) fail("Cannot lock this installation for relinking.");
    QLockFile lock(gamePath(id, "launch.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock()) fail("This game is running or being prepared. Retry after it exits.");
    if (!recoveryIssue(id).isEmpty()) fail(recoveryIssue(id));
    std::unique_ptr<QLockFile> duplicateLock;
    QString duplicateDirectory;
    if (!plan.duplicateId.isEmpty()) {
        duplicateDirectory = QFileInfo(gamePath(plan.duplicateId, "launch.lock")).absolutePath();
        if (!QDir().mkpath(duplicateDirectory)) fail("Cannot lock the discovered duplicate.");
        duplicateLock = std::make_unique<QLockFile>(gamePath(plan.duplicateId, "launch.lock")); duplicateLock->setStaleLockTime(0);
        if (!duplicateLock->tryLock()) fail("The destination installation is in use.");
        if (!recoveryIssue(plan.duplicateId).isEmpty()) fail(recoveryIssue(plan.duplicateId));
    }
    sql(db_, "BEGIN IMMEDIATE");
    try {
        if (game(id).json() != plan.original.json()) fail("This game's settings changed. Review the relink again.");
        const auto &g = plan.updated;
        if (!QFileInfo(g.folder).isDir() || (!g.folderIdentity.isEmpty() && folderIdentity(g.folder) != g.folderIdentity))
            fail("The destination folder changed or disappeared. Choose it again.");
        if (!g.executable.isEmpty() && !QFileInfo(g.executable).isFile()) fail("The executable is missing from the new folder. Choose it in the relink dialog.");
        if (!QFileInfo(g.workingDirectory).isDir()) fail("Choose an existing working directory in the relink dialog.");
        if (!plan.duplicateId.isEmpty()) {
            const auto duplicate = game(plan.duplicateId);
            if (duplicate.json() != plan.duplicateConfig || !unusedInstallation(*this, duplicate, true))
                fail("The discovered duplicate was configured or prepared. Its data has been kept; review the destination again.");
            QSqlQuery remove(db_); remove.prepare("DELETE FROM games WHERE id=?"); remove.addBindValue(plan.duplicateId); check(remove);
        }
        store(g);
        const auto previousName = QFileInfo(plan.original.folder).fileName();
        if (normalizedSaveGameName(previousName) != normalizedSaveGameName(QFileInfo(g.folder).fileName())) {
            auto config = loadSaveConfig(*this, id);
            config.folderAliases.removeAll(previousName); config.folderAliases.prepend(previousName);
            while (config.folderAliases.size() > 8) config.folderAliases.removeLast();
            stageSaveConfig(plan.original, g, validatedSaveConfig(*this, g, config));
        }
        sql(db_, "COMMIT");
    } catch (...) {
        db_.rollback();
        try { recoverSettingsChange(id); } catch (...) { /* Recover before the next edit or Play. */ }
        throw;
    }
    QFile::remove(gamePath(id, "settings-change.json"));
    if (duplicateLock) { duplicateLock->unlock(); QDir().rmdir(duplicateDirectory); }
}
int Library::reconcileFolders(const DiscoverySnapshot &snapshot) {
    auto existing = games(); int moved = 0;
    // Learn identities for pre-feature records without accepting runner updates.
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        for (auto &g : existing) {
            const auto identity = snapshot.folderIdentities.value(g.folder);
            if (!g.folderIdentity.isEmpty() || identity.isEmpty() || busy(g.id)) continue;
            auto updated = g; updated.folderIdentity = identity;
            QSqlQuery q(db_); q.prepare("UPDATE games SET config=? WHERE id=? AND config=?");
            q.addBindValue(QString::fromUtf8(QJsonDocument(storedGame(root_, updated)).toJson(QJsonDocument::Compact))); q.addBindValue(g.id);
            // Compare JSON semantically; older records can have different whitespace/optional keys.
            QSqlQuery current(db_); current.prepare("SELECT config FROM games WHERE id=?"); current.addBindValue(g.id); check(current); current.next();
            const auto raw = current.value(0).toString();
            if (resolvedGame(root_, QJsonDocument::fromJson(raw.toUtf8()).object()).json() != g.json()) continue;
            q.addBindValue(raw); check(q); if (q.numRowsAffected() == 1) g = updated;
        }
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    for (const auto &g : existing) {
        if (g.backend == "steam" || g.folderIdentity.isEmpty() || busy(g.id)) continue;
        const auto matches = snapshot.folderIdentities.keys(g.folderIdentity);
        if (matches.size() != 1 || matches.first() == g.folder) continue;
        const auto destination = matches.first();
        if (std::any_of(existing.begin(), existing.end(), [&](const Game &other) { return other.id != g.id && other.folder == destination; })) continue;
        try { relink(planRelink(g.id, destination)); ++moved; } catch (const std::exception &) { /* Keep the old record available for manual repair. */ }
    }
    return moved;
}
int Library::import(const QList<Candidate> &candidates, const QString &runner) {
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Use Rescan to retry discovery.");
    auto existing = games(); int added = 0;
    const auto backend = settings()["backend"].toString("umu");
    for (auto c : candidates) {
        if (!QFileInfo(c.folder).isDir()) continue; // Another window may have removed an older scan result.
        if (removalReservedFolder(c.folder)) continue;
        if (std::any_of(existing.begin(), existing.end(), [&](const Game &g) { return g.folder == c.folder; })) continue;
        Game g; g.id = QUuid::createUuid().toString(QUuid::WithoutBraces); g.name = c.name; g.folder = c.folder;
        g.runner = runner; g.backend = c.native ? "native" : backend;
        const auto choice = selectRecipe(root_, c.folder, c.executables,
            settings()["default_recipe"].toString("generic-game-v1"));
        if (choice.ambiguous()) {
            for (const auto &recipe : choice.matches) g.recipeCandidates.append(recipe.id);
        } else {
            applyRecipeToGame(g, choice.selected());
            if (choice.selected().tier != "default" && choice.selected().runnerExplicit &&
                (runner.isEmpty() || runner == settings()["runner"].toString())) g.runner.clear();
            c.executables = recipeExecutables(choice.selected(), c.folder, c.executables);
        }
        if (c.executables.size() == 1) g.executable = c.executables.first();
        g.workingDirectory = g.executable.isEmpty() ? g.folder : QFileInfo(g.executable).absolutePath();
        if (!choice.ambiguous() && !choice.selected().settings["working_directory"].toString().isEmpty())
            g.workingDirectory = canonicalPath(QDir(g.folder).filePath(choice.selected().settings["working_directory"].toString()));
        g.runnerRevision = runnerVersion(g.runner); g.folderIdentity = folderIdentity(g.folder); g.source = "discovered";
        store(g); existing.append(g); ++added;
    }
    return added;
}
Game Library::applyRecipe(const QString &id, const QString &recipeId) {
    auto g = game(id);
    const auto found = inspect(g.folder, root_).executables;
    const auto selection = selectRecipe(root_, g.folder, found,
        settings()["default_recipe"].toString("generic-game-v1"));
    Recipe chosen;
    for (const auto &recipe : selection.matches) if (recipe.id == recipeId) chosen = recipe;
    if (chosen.id.isEmpty()) fail("This recipe does not fully match the game folder.");
    auto updated = g;
    applyRecipeToGame(updated, chosen);
    if (chosen.settings.contains("runner")) { updated.runner.clear(); updated.runnerRevision.clear(); }
    const auto candidates = recipeExecutables(chosen, g.folder, found);
    if (const auto relative = chosen.settings["executable"].toString(); !relative.isEmpty())
        updated.executable = canonicalPath(QDir(g.folder).filePath(relative));
    else if (candidates.size() == 1) updated.executable = candidates.first();
    else updated.executable.clear();
    updated.workingDirectory = chosen.settings["working_directory"].toString().isEmpty() ?
        (updated.executable.isEmpty() ? g.folder : QFileInfo(updated.executable).absolutePath()) :
        canonicalPath(QDir(g.folder).filePath(chosen.settings["working_directory"].toString()));
    auto config = loadSaveConfig(*this, id);
    if (chosen.settings.contains("saves") && config.mappings.isEmpty()) {
        for (const auto &value : chosen.settings["saves"].toArray()) {
            const auto item = value.toObject(); config.mappings.append({item["scope"].toString(), item["path"].toString()});
        }
        if (!config.mappings.isEmpty()) { config.discoveryState = "identified"; config.discoverySource = "recipe"; }
        save(updated, &config);
    } else save(updated);
    return game(id);
}
bool Library::busy(const QString &id) const {
    return supervisorActive(id) || !recoveryIssue(id).isEmpty();
}
QString Library::recoveryIssue(const QString &id) const {
    if (QFileInfo::exists(gamePath(id, "settings-change.json")))
        return "An interrupted settings change needs recovery. Use Rescan before Play.";
    const auto removal = removalRecoveryIssue(id);
    if (!removal.isEmpty()) return removal;
    const auto saves = saveRestoreIssue(*this, id);
    if (!saves.isEmpty()) return saves;
    return runtimeRecoveryIssue(*this, id);
}
bool Library::supervisorActive(const QString &id) const {
    QLockFile lock(gamePath(id, "launch.lock")); lock.setStaleLockTime(0);
    if (!QFileInfo::exists(lock.fileName())) return false;
    if (lock.tryLock()) { lock.unlock(); return false; }
    return lock.error() == QLockFile::LockFailedError;
}
void Library::recordStatus(const QString &id, const QString &state, const QString &message) const {
    rememberSteamRequest(*this, id);
    writeJson(gamePath(id, "status.json"), {{"state", state}, {"message", message}, {"updated", now()}});
}
QString Library::status(const Game &g) const {
    if (supervisorActive(g.id)) return readJson(gamePath(g.id, "status.json"))["state"].toString("Starting");
    if (!recoveryIssue(g.id).isEmpty()) return "Recovery required";
    if (!QFileInfo(g.folder).isDir() || (!g.executable.isEmpty() && !QFileInfo(g.executable).isFile())) return "Unavailable";
    if (!g.recipeCandidates.isEmpty()) return "Choose recipe";
    const auto identity = folderIdentity(g.folder);
    if (!g.folderIdentity.isEmpty() && identity != g.folderIdentity) return "Folder changed";
    if (!launchPathIssue(g).isEmpty()) return "Path needs rename";
    const auto last = readJson(gamePath(g.id, "status.json"));
    if (last["state"].toString() == "Needs attention") return "Needs attention";
    if (QStringList{"Preparing", "Running", "Starting", "Configuring"}.contains(last["state"].toString())) return "Interrupted";
    if (last["state"] == "Save needs attention" || !pendingSaveSnapshotIssue(*this, g.id).isEmpty()) return "Save needs attention";
    if (g.installPending) return "Installation pending";
    if (QFileInfo::exists(g.folder + "/.novelty-transfer.json")) return "Transfer incomplete";
    if (g.backend == "steam") return g.steamId.isEmpty() ? "Needs Steam shortcut" : "Steam shortcut";
    if (g.backend == "native") return g.executable.isEmpty() ? "Choose executable" :
        nativeExecutable(g.executable) ? "Ready" : "Executable not runnable";
    if (!findSetupProfile(g.setupProfile)) return "Choose setup profile";
    if (g.executable.isEmpty()) return "Choose executable";
    if (!QFileInfo(QDir(g.runner).filePath("proton")).isFile()) return "Choose runner";
    if (!g.runnerRevision.isEmpty() && runnerVersion(g.runner) != g.runnerRevision) return "Runner changed";
    auto umu = g.umuLauncher.isEmpty() ? settings()["umu"].toString("umu-run") :
               g.umuResolved;
    if (umu == "umu-run" && resolveProgram(umu).isEmpty()) {
        umu = dataRoot() + "/tools/umu-run"; // Keep an existing legacy selection stable.
        if (resolveProgram(umu).isEmpty()) umu = managedUmuPath(root_);
    }
    if (resolveProgram(umu).isEmpty()) return "umu missing";
    if (setupComplete(*this, g)) return "Ready";
    return QFileInfo::exists(prefix(g.id)) || QFileInfo::exists(gamePath(g.id, "setup.json")) ? "Needs setup" : "New";
}
QJsonObject Library::settings() const {
    QSqlQuery q(db_); if (!q.exec("SELECT value FROM settings WHERE key='app'")) fail(q.lastError().text());
    if (!q.next()) return {{"umu", "umu-run"}, {"steam", "steam"}, {"backend", "umu"},
                           {"default_recipe", "generic-game-v1"}};
    return resolvedLibrarySettings(root_, QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
bool Library::managedRuntimeStorage() const {
    QSqlQuery q(db_); if (!q.exec("SELECT value FROM settings WHERE key='runtime_storage'")) fail(q.lastError().text());
    return q.next() && q.value(0).toString() == "managed";
}
void Library::enableManagedRuntimeStorage() {
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry runtime migration shortly.");
    for (const auto &g : games())
        if (busy(g.id) || !recoveryIssue(g.id).isEmpty())
            fail("Finish running or recovering games before switching runtime storage.");
    if (managedRuntimeStorage()) return;
    QSqlQuery q(db_);
    q.prepare("INSERT INTO settings(key,value) VALUES('runtime_storage','managed') "
              "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    check(q);
}
void Library::setSettings(const QJsonObject &s) {
    QSqlQuery q(db_); q.prepare("INSERT INTO settings(key,value) VALUES('app',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.addBindValue(QString::fromUtf8(QJsonDocument(storedLibrarySettings(root_, s)).toJson(QJsonDocument::Compact))); check(q);
}
} // namespace novelty
