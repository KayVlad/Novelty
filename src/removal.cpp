#include "core.h"
#include "runtime.h"
#include "saves.h"
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/quuid.h>
#include <QtSql/qsqlerror.h>
#include <QtSql/qsqlquery.h>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void check(QSqlQuery &q) { if (!q.exec()) fail(q.lastError().text()); }
void sql(QSqlDatabase db, const QString &text) { QSqlQuery q(db); if (!q.exec(text)) fail(q.lastError().text()); }
bool safeId(const QString &id) { return !QUuid(id).isNull(); }
}
namespace {
bool within(const QString &path, const QString &directory) {
    return path == directory || path.startsWith(directory == "/" ? directory : directory + '/');
}
void checkFolder(const Library &lib, const QString &folder) {
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder) || folder == "/") fail("This is not a dedicated game folder.");
    QStringList protectedFolders{QDir::homePath(), lib.root(), lib.dataRoot(), "/tmp", "/var", "/usr", "/etc", "/opt", "/home", "/run", "/media", "/mnt"};
    for (auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation,
                          QStandardPaths::PicturesLocation, QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation,
                          QStandardPaths::GenericConfigLocation, QStandardPaths::GenericDataLocation, QStandardPaths::GenericCacheLocation})
        protectedFolders.append(QStandardPaths::standardLocations(location));
    for (const auto &path : protectedFolders) if (within(canonicalPath(path), folder)) fail("Full removal requires a dedicated game folder; this path contains a library, system or personal folder: " + folder);
    if (within(folder, canonicalPath(lib.dataRoot()))) fail("A game inside Novelty's library data cannot be removed this way.");
    if (canonicalPath(lib.dataRoot()) != QDir::cleanPath(lib.dataRoot())) fail("Library data is linked to another location; removal recovery is unavailable.");
}
RemovalPlan removalPlan(const Library &lib, const Game &g) {
    const auto issue = lib.removalIssue(g);
    if (!issue.isEmpty()) fail(issue);
    RemovalPlan plan; plan.original = g;
    const auto folder = QDir::cleanPath(g.folder); checkFolder(lib, folder);
    for (const auto &other : lib.games()) {
        auto extraPaths = activeExtraPaths(lib, other);
        if (other.installPending && !other.installer.isEmpty()) extraPaths.append(other.installer);
        if (other.id != g.id) for (const auto &path : extraPaths)
            for (const auto &target : {folder, lib.prefix(g.id), QFileInfo(lib.gamePath(g.id, "launch.lock")).absolutePath()})
                if (within(path, target) || within(target, path)) fail("This folder is being used by an executable running for " + other.name + ". Wait until it finishes.");
        if (other.id != g.id && (within(other.folder, folder) || within(folder, other.folder) ||
            (!other.executable.isEmpty() && within(canonicalPath(other.executable), folder)) ||
            (!other.workingDirectory.isEmpty() && within(canonicalPath(other.workingDirectory), folder))))
            fail("This folder is shared with " + other.name + ". Relink games into separate folders before removal.");
        if (other.id != g.id && !other.runner.isEmpty() && within(canonicalPath(other.runner), folder)) fail("This folder contains a configured Proton runner. Move the runner before removing the game.");
    }
    if (canonicalPath(lib.dataRoot()) != QDir::cleanPath(lib.dataRoot())) fail("Library data is linked to another location; full removal is unavailable.");
    plan.paths = {folder, lib.prefix(g.id), QFileInfo(lib.gamePath(g.id, "launch.lock")).absolutePath()};
    for (const auto &path : plan.paths) {
        const QFileInfo info(path);
        if (info.isSymLink() || canonicalPath(path) != path) fail("A removal path is a symbolic link or has changed: " + path);
        if (info.exists() && !info.isDir()) fail("A removal path is not a directory: " + path);
        plan.identities.insert(path, folderIdentity(path));
        if (info.exists() && plan.identities.value(path).isEmpty())
            fail("This filesystem cannot provide the directory identity needed for recoverable removal: " + path);
    }
    const auto identity = plan.identities.value(folder);
    if (QFileInfo::exists(folder) && !g.folderIdentity.isEmpty() && identity != g.folderIdentity)
        fail("This folder belongs to a different installation. Relink it before removal.");
    return plan;
}
void syncDirectory(const QString &path) {
    const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) fail("Cannot open directory for durable removal: " + path);
    const int result = fsync(fd); close(fd);
    if (result != 0) fail("Cannot persist directory changes; recovery record retained: " + path);
}
void verifyDirectory(const QString &path, const QString &identity) {
    if (identity.isEmpty() || QFileInfo(path).isSymLink() || canonicalPath(path) != path || folderIdentity(path) != identity)
        fail("A removal directory changed or cannot be verified. Files were kept: " + path);
}
QJsonObject decode(const QString &text) {
    QJsonParseError error; const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Removal recovery record is damaged; inspect the library database.");
    return document.object();
}
QJsonObject manifestFor(const Library &lib, const Game &g) {
    QJsonArray moves;
    for (const auto &path : {g.folder, lib.prefix(g.id)}) {
        const auto parent = QFileInfo(path).absolutePath();
        const bool present = QFileInfo::exists(path);
        const auto identity = present ? folderIdentity(path) : QString();
        const auto parentIdentity = folderIdentity(parent);
        if (present) { verifyDirectory(path, identity); verifyDirectory(parent, parentIdentity); syncDirectory(parent); }
        moves.append(QJsonObject{{"original", path}, {"staged", parent + "/.novelty-removing-" + QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                 {"identity", identity}, {"parent_identity", parentIdentity}});
    }
    const auto state = QFileInfo(lib.gamePath(g.id, "launch.lock")).absolutePath();
    verifyDirectory(state, folderIdentity(state));
    return {{"schema", 1}, {"game", g.json()}, {"moves", moves}, {"state_identity", folderIdentity(state)},
            {"state_parent_identity", folderIdentity(QFileInfo(state).absolutePath())}};
}
void checkReferences(const Library &lib, const QString &path, const QString &excludedId = {}) {
    for (const auto &g : lib.games()) if (g.id != excludedId) {
        auto references = activeExtraPaths(lib, g);
        references.append({g.folder, g.executable, g.workingDirectory, g.runner, g.installer});
        for (const auto &reference : references)
            if (!reference.isEmpty() && (within(canonicalPath(reference), path) || within(path, canonicalPath(reference))))
                fail("Recovery path is referenced by another game; files were kept: " + path);
    }
}
void finishJob(const Library &lib, QSqlDatabase db, const QString &id, const QString &phase, const QJsonObject &manifest, const std::function<void(const QString &)> &progress = {}) {
    if (manifest["schema"].toInt() != 1 || !safeId(id)) fail("Unsupported removal recovery record.");
    const auto g = Game::fromJson(manifest["game"].toObject());
    if (g.id != id || (phase != "staging" && phase != "committed")) fail("Invalid removal recovery phase/identity.");
    checkFolder(lib, g.folder);
    QSqlQuery current(db); current.prepare("SELECT config FROM games WHERE id=?"); current.addBindValue(id); check(current);
    const bool exists = current.next();
    if ((phase == "staging" && (!exists || lib.game(id).json() != g.json())) ||
        (phase == "committed" && exists)) fail("The library entry conflicts with removal recovery. Files were kept.");
    current.finish();
    const auto moves = manifest["moves"].toArray();
    if (moves.size() != 2) fail("Invalid removal recovery paths.");
    const QStringList expected{g.folder, lib.prefix(id)};
    // Validate ALL paths before restoring/deleting any of them. Never follow a replacement/link.
    for (int i = 0; i < moves.size(); ++i) {
        const auto move = moves[i].toObject(); const auto original = move["original"].toString(), staged = move["staged"].toString();
        const auto parent = QFileInfo(original).absolutePath(), identity = move["identity"].toString();
        const auto filename = QFileInfo(staged).fileName();
        if (original != expected[i] || QFileInfo(staged).absolutePath() != parent ||
            !filename.startsWith(".novelty-removing-") || !safeId(filename.mid(18)) || QDir::cleanPath(staged) != staged)
            fail("Invalid staging path in removal record.");
        if (identity.isEmpty()) {
            if (QFileInfo::exists(staged) || QFileInfo(staged).isSymLink()) fail("Unexpected staged folder; files were kept: " + staged);
            continue;
        }
        verifyDirectory(parent, move["parent_identity"].toString());
        if (QFileInfo::exists(staged) || QFileInfo(staged).isSymLink()) {
            verifyDirectory(staged, identity); checkReferences(lib, staged);
            if (phase == "staging" && (QFileInfo::exists(original) || QFileInfo(original).isSymLink()))
                fail("Both original and staged folders exist. Resolve the conflict, then Rescan: " + original + "\n" + staged);
        } else if (phase == "staging") verifyDirectory(original, identity);
        if (phase == "staging") checkReferences(lib, original, id);
    }
    const auto state = QFileInfo(lib.gamePath(id, "launch.lock")).absolutePath();
    verifyDirectory(QFileInfo(state).absolutePath(), manifest["state_parent_identity"].toString());
    if (phase == "staging" || QFileInfo::exists(state) || QFileInfo(state).isSymLink()) verifyDirectory(state, manifest["state_identity"].toString());
    checkReferences(lib, state, id);
    for (int i = moves.size() - 1; i >= 0; --i) {
        const auto move = moves[i].toObject(); const auto original = move["original"].toString(), staged = move["staged"].toString();
        if (QFileInfo::exists(staged)) {
            verifyDirectory(QFileInfo(staged).absolutePath(), move["parent_identity"].toString());
            verifyDirectory(staged, move["identity"].toString());
            if (phase == "staging") {
                if (QFileInfo::exists(original) || QFileInfo(original).isSymLink()) fail("Restore destination appeared; files kept: " + original);
                if (!QDir().rename(staged, original)) fail("Cannot restore folder; recovery record retained: " + staged);
            } else {
                if (!QDir(staged).removeRecursively()) fail("Cleanup incomplete; use Rescan to retry: " + staged);
                if (progress) progress("folder-cleaned");
            }
        }
        if (!move["identity"].toString().isEmpty()) syncDirectory(QFileInfo(original).absolutePath());
    }
    if (phase == "committed") {
        if (QFileInfo::exists(state)) {
            verifyDirectory(state, manifest["state_identity"].toString());
            if (!QDir(state).removeRecursively()) fail("State cleanup incomplete; use Rescan to retry: " + state);
        }
        syncDirectory(QFileInfo(state).absolutePath());
        if (progress) progress("state-cleaned");
    }
    QSqlQuery erase(db); erase.prepare("DELETE FROM removals WHERE game_id=?"); erase.addBindValue(id); check(erase);
}
}
RemovalPlan Library::planRemoval(const QString &id) const {
    if (busy(id)) fail("Wait until the game finishes before removing it.");
    return removalPlan(*this, game(id));
}
QString Library::removalIssue(const Game &g) const {
    const auto recovery = recoveryIssue(g.id);
    if (!recovery.isEmpty()) return recovery;
    const auto pendingSaves = pendingSaveSnapshotIssue(*this, g.id);
    if (!pendingSaves.isEmpty()) return pendingSaves;
    if (g.backend == "steam" || QFileInfo::exists(gamePath(g.id, "steam-request.json")) ||
        readJson(gamePath(g.id, "status.json"))["state"] == "Requested in Steam")
        return "Full removal is unavailable because Novelty cannot confirm that a Steam game has stopped. "
               "Steam lifecycle tracking is required before Novelty can safely remove its files.";
    return {};
}
QString Library::removalRecoveryIssue(const QString &id) const {
    QSqlQuery q(db_); q.prepare("SELECT 1 FROM removals WHERE game_id=?"); q.addBindValue(id); check(q);
    return q.next() ? "This game has interrupted removal work. Use Rescan to restore its folders or finish the confirmed cleanup. Conflicting files will be kept." : QString();
}
bool Library::removalReservedFolder(const QString &folder) const {
    QSqlQuery q(db_); if (!q.exec("SELECT manifest FROM removals")) fail(q.lastError().text());
    const auto path = canonicalPath(folder);
    while (q.next()) {
        const auto manifest = decode(q.value(0).toString());
        if (manifest["schema"].toInt() != 1 || manifest["moves"].toArray().size() != 2)
            fail("Damaged removal record blocks library edits until recovery.");
        const auto id = manifest["game"].toObject()["id"].toString();
        if (!safeId(id)) fail("Damaged removal identity blocks library edits until recovery.");
        const auto state = QFileInfo(gamePath(id, "launch.lock")).absolutePath();
        if (within(path, state) || within(state, path)) return true;
        for (const auto &value : manifest["moves"].toArray()) {
            const auto move = value.toObject();
            for (const auto &reserved : {move["original"].toString(), move["staged"].toString()}) {
                if (reserved.isEmpty()) fail("Damaged removal record blocks library edits until recovery.");
                if (within(path, reserved) || within(reserved, path)) return true;
            }
        }
    }
    return false;
}
QStringList Library::recoverRemovals() {
    QStringList messages;
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) return {"Library maintenance is active elsewhere. Use Rescan to retry recovery."};
    struct Job { QString id, phase, manifest; }; QList<Job> jobs;
    QSqlQuery q(db_); if (!q.exec("SELECT game_id, phase, manifest FROM removals")) fail(q.lastError().text());
    while (q.next()) jobs.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    q.finish();
    for (const auto &job : jobs) try {
        if (!safeId(job.id)) fail("Invalid removal installation ID.");
        std::unique_ptr<QLockFile> lock;
        if (QFileInfo(QFileInfo(gamePath(job.id, "launch.lock")).absolutePath()).isDir()) {
            lock = std::make_unique<QLockFile>(gamePath(job.id, "launch.lock")); lock->setStaleLockTime(0);
            if (!lock->tryLock()) fail("Game operation is still active; retry recovery after it finishes.");
        }
        if (!runtimeRecoveryIssue(*this, job.id).isEmpty()) fail(runtimeRecoveryIssue(*this, job.id));
        const auto manifest = decode(job.manifest);
        finishJob(*this, db_, job.id, job.phase, manifest);
        messages.append((job.phase == "staging" ? "Restored interrupted removal: " : "Finished confirmed removal: ") + manifest["game"].toObject()["name"].toString());
    } catch (const std::exception &e) { messages.append("Removal recovery: " + QString::fromUtf8(e.what())); }
    return messages;
}
RemovalResult Library::remove(const RemovalPlan &plan, const std::function<void(const QString &)> &progress) {
    const auto id = plan.original.id;
    if (!safeId(id)) fail("Invalid installation ID.");
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry removal shortly.");
    if (!recoveryIssue(id).isEmpty()) fail(recoveryIssue(id));
    const auto state = QFileInfo(gamePath(id, "launch.lock")).absolutePath();
    if (!QDir().mkpath(state)) fail("Cannot lock this game for removal.");
    QLockFile lock(gamePath(id, "launch.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock()) fail("This game is running or preparing. Its files were kept.");
    QJsonObject manifest;
    sql(db_, "BEGIN IMMEDIATE");
    try {
        const auto current = game(id);
        if (current.json() != plan.original.json()) fail("The game settings changed. Review removal again.");
        const auto checked = removalPlan(*this, current);
        if (checked.paths != plan.paths) fail("The removal paths changed. Review removal again.");
        for (const auto &path : plan.paths)
            if ((path != state || !plan.identities.value(state).isEmpty()) && checked.identities.value(path) != plan.identities.value(path))
                fail("A removal folder changed. Review removal again: " + path);
        manifest = manifestFor(*this, current);
        QSqlQuery intent(db_); intent.prepare("INSERT INTO removals VALUES(?, 'staging', ?)"); intent.addBindValue(id);
        intent.addBindValue(QString::fromUtf8(QJsonDocument(manifest).toJson(QJsonDocument::Compact))); check(intent);
        sql(db_, "COMMIT"); // Intent survives rollback of the later game deletion transaction.
        if (progress) progress("intent");
        sql(db_, "BEGIN IMMEDIATE");
        int index = 0;
        for (const auto &value : manifest["moves"].toArray()) {
            const auto move = value.toObject(); const auto original = move["original"].toString(), staged = move["staged"].toString();
            if (!move["identity"].toString().isEmpty()) {
                verifyDirectory(original, move["identity"].toString());
                if (QFileInfo::exists(staged) || !QDir().rename(original, staged)) fail("Cannot stage removal; files kept: " + original);
                syncDirectory(QFileInfo(original).absolutePath());
            }
            if (progress) progress(index++ == 0 ? "game-staged" : "prefix-staged");
        }
        QSqlQuery erase(db_); erase.prepare("DELETE FROM games WHERE id=?"); erase.addBindValue(id); check(erase);
        QSqlQuery committed(db_); committed.prepare("UPDATE removals SET phase='committed' WHERE game_id=?"); committed.addBindValue(id); check(committed);
        sql(db_, "COMMIT"); // Record deletion and cleanup authorization commit together.
    } catch (const std::exception &e) {
        db_.rollback();
        QSqlQuery pending(db_); pending.prepare("SELECT phase, manifest FROM removals WHERE game_id=?"); pending.addBindValue(id); check(pending);
        if (pending.next()) {
            const auto phase = pending.value(0).toString(), raw = pending.value(1).toString(); pending.finish();
            try { finishJob(*this, db_, id, phase, decode(raw)); }
            catch (const std::exception &recovery) { fail(QString::fromUtf8(e.what()) + "\nRecovery retained for Rescan: " + QString::fromUtf8(recovery.what())); }
        }
        throw;
    }
    if (progress) progress("committed");
    RemovalResult result;
    try { finishJob(*this, db_, id, "committed", manifest, progress); }
    catch (const std::exception &e) {
        result.recoveryMessage = QString::fromUtf8(e.what());
        for (const auto &value : manifest["moves"].toArray()) {
            const auto path = value.toObject()["staged"].toString(); if (QFileInfo::exists(path)) result.cleanupPaths.append(path);
        }
        if (QFileInfo::exists(state)) result.cleanupPaths.append(state);
    }
    return result;
}
}
