#include "core.h"
#include "recipes.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qlockfile.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
}
Game Library::createInstallation(const QString &name, const QString &installer, const QString &runner, const QString &locale) {
    const auto title = name.trimmed();
    if (title.isEmpty() || title.startsWith('.') || title.contains('/') || title.contains('\\'))
        fail("Choose a name for a new folder directly inside your Games folder.");
    Game g; g.id = QUuid::createUuid().toString(QUuid::WithoutBraces); g.name = title;
    g.folder = QDir(root()).filePath(title); g.workingDirectory = g.folder;
    g.runner = runner; g.runnerRevision = runnerVersion(runner); g.locale = locale;
    g.installer = canonicalPath(installer); g.installPending = true; g.source = "installer";
    // Validate the destination, executable and backend before creating any game data.
    const auto issue = launchPathIssue(g); if (!issue.isEmpty()) fail(issue);
    auto preflight = g; preflight.folder = root(); preflight.workingDirectory = root();
    extraLaunchCommand(*this, preflight, {installer, {}, {}});
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry shortly.");
    if (QFileInfo::exists(g.folder) || QFileInfo(g.folder).isSymLink()) fail("That folder already exists. Choose a new name or use Select folder.");
    for (const auto &existing : games()) if (existing.folder == g.folder) fail("That folder belongs to an existing game entry.");
    for (const auto &path : {g.folder, g.installer, g.runner})
        if (removalReservedFolder(path)) fail("This path is reserved by interrupted removal. Use Rescan first.");
    if (!QDir(root()).mkdir(title)) fail("Cannot create the installation folder.");
    g.folderIdentity = folderIdentity(g.folder);
    store(g);
    return g;
}
QString Library::finishInstallation(const QString &id) {
    qint64 owner = 0;
    QLockFile lock(gamePath(id, "launch.lock"));
    if (!lock.getLockInfo(&owner, nullptr, nullptr) || owner != QCoreApplication::applicationPid())
        fail("Installation detection requires the game's operation lock.");
    if (!recoveryIssue(id).isEmpty()) fail(recoveryIssue(id));
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry detection in Manage.");
    auto g = game(id);
    if (!g.installPending) return {};
    if (!QFileInfo(g.folder).isDir() || (!g.folderIdentity.isEmpty() && folderIdentity(g.folder) != g.folderIdentity))
        fail("The installation folder is missing or changed. Relink it before detecting files.");
    auto candidate = inspect(g.folder, root());
    if (candidate.executables.isEmpty()) {
        const QString message = "No game executable found in " + g.folder + ". In the installer choose N:\\ as the destination, then use Detect installed files in Manage. Detection checks two subfolder levels.";
        recordStatus(id, "Installation pending", message); return message;
    }
    if (candidate.executables.size() == 1) {
        g.executable = candidate.executables.first();
        const auto nested = inspect(QFileInfo(g.executable).absolutePath(), root());
        if (nested.engine != "Generic") { candidate.engine = nested.engine; candidate.evidence = nested.evidence; }
    }
    g.workingDirectory = g.executable.isEmpty() ? g.folder : QFileInfo(g.executable).absolutePath();
    const auto choice = selectRecipe(root(), g.folder, candidate.executables,
        settings()["default_recipe"].toString("general-game"));
    if (choice.ambiguous()) {
        for (const auto &recipe : choice.matches) g.recipeCandidates.append(recipe.id);
    } else {
        const auto installerLocale = g.locale;
        applyRecipeToGame(g, choice.selected());
        if (choice.selected().tier != "general" && choice.selected().runnerExplicit &&
            g.runner == settings()["runner"].toString())
            { g.runner.clear(); g.runnerRevision.clear(); }
        g.recipeBase = portableGameSettings(g);
        if (!installerLocale.isEmpty()) g.locale = installerLocale; // Installer language is an explicit saved choice.
    }
    g.installPending = false; g.installer.clear(); store(g);
    const QString message = g.executable.isEmpty() ? "Installation detected. Choose the game's executable in Manage → Launch." : "Installation detected. Your game is ready to launch.";
    recordStatus(id, "Ready", message); return message;
}
QString Library::detectInstallation(const QString &id) {
    if (!recoveryIssue(id).isEmpty()) fail(recoveryIssue(id));
    QDir().mkpath(QFileInfo(gamePath(id, "launch.lock")).absolutePath());
    QLockFile lock(gamePath(id, "launch.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock()) fail("Wait for the installer to close before detecting files.");
    return finishInstallation(id);
}
QStringList Library::recoverInstallations() {
    QStringList messages;
    for (const auto &g : games()) if (g.installPending && !busy(g.id)) {
        try {
            const auto operation = readJson(gamePath(g.id, "extra-launch.json"));
            if (operation["kind"] == "installer" && operation["state"] == "complete") {
                const auto message = detectInstallation(g.id);
                if (!message.isEmpty()) messages.append(message);
            }
        } catch (const std::exception &e) { messages.append(QString::fromUtf8(e.what())); }
    }
    return messages;
}
} // namespace novelty
