#include "core.h"
#include "setup.h"
#include "profiles.h"
#include "runtime.h"
#include "saves.h"
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
QStringList findExecutables(const QString &root, QStringList *watched, int depth = 0) {
    QStringList out;
    if (watched) watched->append(root);
    QDir dir(root);
    for (const auto &f : dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        if (f.suffix().compare("exe", Qt::CaseInsensitive) != 0) continue;
        const auto base = f.completeBaseName().toLower();
        if (base.startsWith("unins") || base.contains("uninstall") || base == "setup" ||
            base == "install" || base.startsWith("python") || base.startsWith("zsync") ||
            base == "crashreporter") continue;
        out.append(f.absoluteFilePath());
    }
    if (depth < 2) {
        for (const auto &sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
            if (QStringList{"lib", "renpy", "saves", "savedata", "cache", ".novelty"}.contains(sub.fileName().toLower())) continue;
            out.append(findExecutables(sub.absoluteFilePath(), watched, depth + 1));
        }
    }
    return out;
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
    for (auto a : j["arguments"].toArray()) g.arguments.append(a.toString());
    const auto env = j["environment"].toObject();
    for (auto it = env.begin(); it != env.end(); ++it) g.environment.insert(it.key(), it.value().toString());
    if (!safeId(g.id)) fail("Invalid installation ID.");
    return g;
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
Candidate inspect(const QString &folder) {
    Candidate c;
    c.folder = canonicalPath(folder); c.name = QFileInfo(c.folder).fileName();
    c.executables = findExecutables(c.folder, &c.watchDirectories);
    QDir d(c.folder);
    if (QFileInfo::exists(d.filePath("renpy/__init__.py")) && QFileInfo(d.filePath("game")).isDir()) {
        c.engine = "Ren'Py"; c.evidence = "Ren'Py runtime directory and game directory found. Generic Proton setup; no extra dependencies assumed.";
    } else if (!d.entryList({"*.xp3"}, QDir::Files).isEmpty()) {
        c.engine = "Kirikiri candidate"; c.evidence = "XP3 archive found. Engine/version and setup remain unverified; generic setup is used.";
    } else if (QFileInfo::exists(d.filePath("SiglusEngine.exe"))) {
        c.engine = "Siglus candidate"; c.evidence = "SiglusEngine.exe found. No tested engine preset yet; generic setup is used.";
    } else {
        c.evidence = "No verified engine signature. Generic Proton setup is used.";
    }
    return c;
}
QList<Candidate> discover(const QString &root) {
    if (!QFileInfo(root).isDir()) fail("Games folder is unavailable: " + root);
    QList<Candidate> out;
    QDir d(root);
    for (auto f : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (f.fileName().startsWith('.')) continue;
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
QList<Game> Library::games() const {
    QList<Game> out; QSqlQuery q(db_);
    if (!q.exec("SELECT config FROM games ORDER BY json_extract(config, '$.name') COLLATE NOCASE")) fail(q.lastError().text());
    while (q.next()) out.append(Game::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object()));
    return out;
}
Game Library::game(const QString &id) const {
    QSqlQuery q(db_); q.prepare("SELECT config FROM games WHERE id=?"); q.addBindValue(id); check(q);
    if (!q.next()) fail("Game not found: " + id);
    return Game::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
}
void Library::save(Game g) {
    const bool creating = g.id.isEmpty();
    if (g.id.isEmpty()) g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!safeId(g.id) || g.name.trimmed().isEmpty()) fail("A game needs a valid ID and name.");
    if (g.backend == "umu") resolveSetupProfile(g);
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry saving shortly.");
    if (!creating) game(g.id); // A stale Manage window must not recreate a removed installation.
    if (busy(g.id)) fail("Wait until the game's launch or preparation finishes before editing it.");
    if (!g.runner.isEmpty()) g.runnerRevision = runnerVersion(g.runner);
    g.folder = canonicalPath(g.folder);
    if (g.folderIdentity.isEmpty()) g.folderIdentity = folderIdentity(g.folder);
    g.source = "manual";
    store(g);
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
    if (selected.isSymLink() || !selected.isFile() || selected.suffix().compare("exe", Qt::CaseInsensitive) != 0 ||
        !path.startsWith(g.folder + '/'))
        fail("Choose an existing game .exe inside this game's folder.");
    g.executable = path;
    g.workingDirectory = QFileInfo(path).absolutePath();
    const auto nested = inspect(g.workingDirectory);
    if (nested.engine != "Generic" && g.engine == "Generic") {
        if (g.setupProfile == defaultSetupProfile(g.engine)) g.setupProfile = defaultSetupProfile(nested.engine);
        g.engine = nested.engine; g.evidence = nested.evidence;
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
    for (const auto &path : {g.folder, g.executable, g.workingDirectory, g.runner, g.installer})
        if (!path.isEmpty() && removalReservedFolder(path)) fail("This path is reserved by interrupted removal. Use Rescan to recover it first.");
    // Preserve the older adapter's untracked Steam request across backend changes.
    rememberSteamRequest(*this, g.id);
    QSqlQuery q(db_);
    q.prepare("INSERT INTO games(id,folder,config) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET folder=excluded.folder,config=excluded.config");
    q.addBindValue(g.id); q.addBindValue(g.folder); q.addBindValue(QString::fromUtf8(QJsonDocument(g.json()).toJson(QJsonDocument::Compact))); check(q);
}
RelinkPlan Library::planRelink(const QString &id, const QString &folder, const QString &executable, const QString &cwd) const {
    RelinkPlan plan; plan.original = game(id); plan.updated = plan.original;
    if (plan.original.backend != "umu") fail("Steam owns this game's shortcut paths. Folder relinking for Steam is a later increment.");
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
    if (!safeId(id) || plan.updated.id != id || plan.original.backend != "umu") fail("Invalid folder relink plan.");
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
        sql(db_, "COMMIT");
    } catch (...) { db_.rollback(); throw; }
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
            q.addBindValue(QString::fromUtf8(QJsonDocument(updated.json()).toJson(QJsonDocument::Compact))); q.addBindValue(g.id);
            // Compare JSON semantically; older records can have different whitespace/optional keys.
            QSqlQuery current(db_); current.prepare("SELECT config FROM games WHERE id=?"); current.addBindValue(g.id); check(current); current.next();
            const auto raw = current.value(0).toString();
            if (Game::fromJson(QJsonDocument::fromJson(raw.toUtf8()).object()).json() != g.json()) continue;
            q.addBindValue(raw); check(q); if (q.numRowsAffected() == 1) g = updated;
        }
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    for (const auto &g : existing) {
        if (g.backend != "umu" || g.folderIdentity.isEmpty() || busy(g.id)) continue;
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
        g.engine = c.engine; g.evidence = c.evidence; g.runner = runner; g.backend = backend;
        g.setupProfile = defaultSetupProfile(c.engine);
        if (c.executables.size() == 1) g.executable = c.executables.first();
        g.workingDirectory = g.executable.isEmpty() ? g.folder : QFileInfo(g.executable).absolutePath();
        g.runnerRevision = runnerVersion(g.runner); g.folderIdentity = folderIdentity(g.folder); g.source = "discovered";
        store(g); existing.append(g); ++added;
    }
    return added;
}
bool Library::busy(const QString &id) const {
    return supervisorActive(id) || !recoveryIssue(id).isEmpty();
}
QString Library::recoveryIssue(const QString &id) const {
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
    const auto identity = folderIdentity(g.folder);
    if (!g.folderIdentity.isEmpty() && identity != g.folderIdentity) return "Folder changed";
    if (!launchPathIssue(g).isEmpty()) return "Path needs rename";
    const auto last = readJson(gamePath(g.id, "status.json"));
    if (last["state"].toString() == "Needs attention") return "Needs attention";
    if (QStringList{"Preparing", "Running", "Starting", "Configuring"}.contains(last["state"].toString())) return "Interrupted";
    if (g.installPending) return "Installation pending";
    if (g.backend == "steam") return g.steamId.isEmpty() ? "Needs Steam shortcut" : "Steam shortcut";
    if (!findSetupProfile(g.setupProfile)) return "Choose setup profile";
    if (g.executable.isEmpty()) return "Choose executable";
    if (!QFileInfo(QDir(g.runner).filePath("proton")).isFile()) return "Choose runner";
    if (!g.runnerRevision.isEmpty() && runnerVersion(g.runner) != g.runnerRevision) return "Runner changed";
    auto umu = settings()["umu"].toString("umu-run");
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
    if (!q.next()) return {{"umu", "umu-run"}, {"steam", "steam"}, {"backend", "umu"}};
    return QJsonDocument::fromJson(q.value(0).toByteArray()).object();
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
    q.addBindValue(QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact))); check(q);
}
} // namespace novelty
