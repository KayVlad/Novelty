#pragma once

#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qprocess.h>
#include <QtCore/qstringlist.h>
#include <QtSql/qsqldatabase.h>
#include <memory>
#include <functional>
#include <optional>

namespace novelty {

struct Game {
    QString id, name, folder, executable, workingDirectory, runner;
    QString locale, backend = "umu", steamId;
    QString runnerRevision;
    QString installer;
    bool installPending = false;
    QString folderIdentity, source = "manual";
    QString engine = "Generic", category = "game", evidence;
    QString recipeId, recipeRevision, runnerSelector, umuLauncher, umuResolved, umuGameId, store;
    QStringList recipeCandidates, winetricks;
    QJsonArray recipeSaves;
    QJsonObject recipeBase, recipeOverrides; // Saved recommendation and explicit changes; launch uses materialized fields.
    QJsonObject metadata; // Automatically or manually linked VNDB record, cached for offline display.
    QStringList arguments;
    QMap<QString, QString> environment;
    QJsonObject json() const;
    void selectRunner(const QString &path); // Explicit Manage choice; an empty value restores the recipe selector.
    static Game fromJson(const QJsonObject &);
};

struct Candidate {
    QString folder, name, engine = "Generic", evidence;
    QStringList executables;
    bool native = false;
    QStringList watchDirectories;
    QByteArray fingerprint;
};

struct DiscoverySnapshot {
    QList<Candidate> candidates;
    QStringList watchDirectories;
    QMap<QString, QString> folderIdentities;
    QStringList recoveryMessages;
};

struct FolderImportResult { Game game; QString warning; bool moved = false; };

struct RelinkPlan {
    Game original, updated;
    QString duplicateId, duplicateName;
    QJsonObject duplicateConfig;
};

struct RemovalPlan {
    Game original;
    QStringList paths;
    QMap<QString, QString> identities;
};
struct RemovalResult { QStringList cleanupPaths; QString recoveryMessage; };

// A quiet metadata interval is a copy heuristic, not proof of completeness.
class CopyTracker {
public:
    QList<Candidate> observe(const QList<Candidate> &, qint64 elapsedMs, qint64 quietMs = 4000);
    int waiting() const { return pending_.size(); }
    void clear() { pending_.clear(); }
private:
    struct Observation { QByteArray fingerprint; qint64 changedAt; };
    QMap<QString, Observation> pending_;
};

class Library;
struct SaveConfig;
QString canonicalPath(const QString &path);
bool nativeExecutable(const QString &path);
QString folderIdentity(const QString &path);
QString launchPathIssue(const Game &);
QString resolveProgram(const QString &program);
QString managedUmuPath(const QString &libraryRoot);
QProcessEnvironment externalToolEnvironment();
QList<Candidate> discover(const QString &root);
DiscoverySnapshot scanDiscovery(const QString &root, const QStringList &knownFolders);
Candidate inspect(const QString &folder, const QString &libraryRoot = {});
QStringList installedRunners(const QString &libraryRoot = {});
QString runnerVersion(const QString &runner);
QJsonObject readJson(const QString &path);
QJsonObject diagnosticReport(const Library &, const QString &id);
void writeJson(const QString &path, const QJsonObject &data);

class Library {
public:
    explicit Library(QString root);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    QString root() const { return root_; }
    QString dataRoot() const;
    QString profileId() const;
    QString gamePath(const QString &id, const QString &file) const;
    QString prefix(const QString &id) const;
    QList<Game> games() const;
    Game game(const QString &id) const;
    void save(Game game, const SaveConfig *saveConfig = nullptr);
    QStringList recoverSettingsChanges();
    void setMetadata(const QString &id, const QJsonObject &metadata);
    RelinkPlan planRelink(const QString &id, const QString &folder, const QString &executable = {}, const QString &cwd = {}) const;
    void relink(const RelinkPlan &);
    RemovalPlan planRemoval(const QString &id) const;
    QString removalIssue(const Game &) const;
    RemovalResult remove(const RemovalPlan &, const std::function<void(const QString &)> &progress = {});
    QStringList recoverRemovals();
    QString removalRecoveryIssue(const QString &id) const;
    bool removalReservedFolder(const QString &folder) const;
    int reconcileFolders(const DiscoverySnapshot &);
    int import(const QList<Candidate> &candidates, const QString &runner);
    FolderImportResult addFolder(const QString &source, const QString &nativeLauncher = {});
    QStringList recoverFolderImports();
    Game selectExecutable(const QString &id, const QString &executable);
    Game previewRecipe(const QString &id, const QString &recipeId) const;
    Game applyRecipe(const QString &id, const QString &recipeId);
    Game createInstallation(const QString &name, const QString &installer, const QString &runner, const QString &locale = {});
    QString detectInstallation(const QString &id);
    QString finishInstallation(const QString &id); // Requires the caller's launch lock.
    QStringList recoverInstallations();
    bool busy(const QString &id) const;
    bool supervisorActive(const QString &id) const;
    QString recoveryIssue(const QString &id) const;
    QString status(const Game &game) const;
    void recordStatus(const QString &id, const QString &state, const QString &message = {}) const;
    QJsonObject settings() const;
    bool managedRuntimeStorage() const;
    void enableManagedRuntimeStorage();
    void setSettings(const QJsonObject &settings);
private:
    void rebaseMovedLibrary();
    void store(const Game &);
    void stageSaveConfig(const Game &before, const Game &after, const QJsonObject &config);
    void recoverSettingsChange(const QString &id);
    QString root_, connection_;
    QSqlDatabase db_;
};

struct Command {
    QString program, cwd;
    QStringList arguments;
    QProcessEnvironment environment;
};
struct ExtraLaunch {
    QString executable, workingDirectory;
    QStringList arguments;
};

Command launchCommand(const Library &, const Game &, bool prepare = false);
Command prefixToolCommand(const Library &, const Game &, const QString &tool);
Command extraLaunchCommand(const Library &, const Game &, const ExtraLaunch &);
QStringList activeExtraPaths(const Library &, const Game &);
int supervise(Library &, const QString &gameId, bool prepareOnly, const QString &tool = {},
              const std::optional<ExtraLaunch> &extra = {}, bool installing = false, bool debugWine = false,
              const QString &winetricksApproval = {});

} // namespace novelty
