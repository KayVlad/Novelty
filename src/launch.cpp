#include "core.h"
#include "setup.h"
#include "profiles.h"
#include "recipes.h"
#include "runtime.h"
#include "saves.h"
#include "save_server_config.h"
#include "save_server_sync.h"
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
}
QProcessEnvironment externalToolEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    if (environment.contains("NOVELTY_BUNDLED_LIB_DIR")) {
        const auto original = environment.value("NOVELTY_ORIGINAL_LD_LIBRARY_PATH");
        const auto originalPlugins = environment.value("NOVELTY_ORIGINAL_QT_PLUGIN_PATH");
        if (original.isEmpty()) environment.remove("LD_LIBRARY_PATH");
        else environment.insert("LD_LIBRARY_PATH", original);
        if (originalPlugins.isEmpty()) environment.remove("QT_PLUGIN_PATH");
        else environment.insert("QT_PLUGIN_PATH", originalPlugins);
        environment.remove("NOVELTY_BUNDLED_LIB_DIR");
        environment.remove("NOVELTY_ORIGINAL_LD_LIBRARY_PATH");
        environment.remove("NOVELTY_ORIGINAL_QT_PLUGIN_PATH");
    }
    return environment;
}
Command launchCommand(const Library &lib, const Game &g, bool prepare) {
    if (!g.recipeCandidates.isEmpty()) fail("Several recipes match this game. Choose one in Manage → Runtime before Play.");
    if (QFileInfo::exists(g.folder + "/.novelty-transfer.json")) fail("This LAN installation is incomplete. Retry its transfer before Play.");
    if (g.installPending && !prepare) fail("Finish installation in Manage before using Play.");
    const auto pathIssue = launchPathIssue(g);
    if (!pathIssue.isEmpty()) fail(pathIssue);
    const auto identity = folderIdentity(g.folder);
    if (!g.folderIdentity.isEmpty() && identity != g.folderIdentity)
        fail("This folder is a different installation. Use Manage → Relink folder before launching it with this game's prefix.");
    Command c; c.environment = externalToolEnvironment(); c.cwd = g.workingDirectory;
    if (g.backend == "steam") {
        if (prepare) fail("Prepare this shortcut in Steam; the current adapter launches existing shortcuts only.");
        bool numeric = !g.steamId.isEmpty();
        for (auto ch : g.steamId) numeric &= ch >= '0' && ch <= '9';
        if (!numeric) fail("Set a numeric Steam launch ID in Manage. Use the ID from the Steam-created desktop shortcut.");
        c.program = resolveProgram(lib.settings()["steam"].toString("steam"));
        if (c.program.isEmpty()) fail("Steam executable was not found. Set its location in Settings.");
        c.arguments = {"steam://rungameid/" + g.steamId};
        return c;
    }
    if (g.backend == "native") {
        if (prepare) fail("Native Linux games do not need Proton preparation.");
        if (!nativeExecutable(g.executable) ||
            !canonicalPath(g.executable).startsWith(canonicalPath(g.folder) + '/'))
            fail("Choose an executable Linux game inside this game's folder.");
        if (!QFileInfo(c.cwd).isDir()) fail("The working directory is unavailable.");
        for (auto it = g.environment.begin(); it != g.environment.end(); ++it) {
            if (it.key().isEmpty() || it.key().contains('=') || it.key().contains(QChar::Null) || it.value().contains(QChar::Null))
                fail("Invalid environment variable.");
            c.environment.insert(it.key(), it.value());
        }
        if (!g.locale.isEmpty()) { c.environment.insert("LC_ALL", g.locale); c.environment.insert("LANG", g.locale); }
        c.program = canonicalPath(g.executable);
        c.arguments = g.arguments;
        return c;
    }
    if (g.backend != "umu") fail("Unsupported launch backend.");
    resolveSetupProfile(g); // Reject unavailable versions before starting a backend or changing a prefix.
    auto umu = g.umuLauncher.isEmpty() ? lib.settings()["umu"].toString("umu-run") :
               g.umuResolved;
    if (umu == "umu-run" && resolveProgram(umu).isEmpty()) {
        umu = lib.dataRoot() + "/tools/umu-run";
        if (resolveProgram(umu).isEmpty()) umu = managedUmuPath(lib.root());
    }
    c.program = resolveProgram(umu);
    if (c.program.isEmpty()) fail("umu-run was not found. Install umu-launcher or set its executable in Settings.");
    if (!QFileInfo(QDir(g.runner).filePath("proton")).isFile()) fail("Choose a valid Proton directory in Manage.");
    if (!g.runnerRevision.isEmpty() && runnerVersion(g.runner) != g.runnerRevision)
        fail("The selected Proton version changed. Review the runner in Manage and save to accept its current version.");
    if (!prepare && !QFileInfo(g.executable).isFile()) fail("Choose the game's executable in Manage.");
    if (!QFileInfo(c.cwd).isDir()) fail("The working directory is unavailable.");
    const bool managedRuntime = lib.managedRuntimeStorage();
    for (auto it = g.environment.begin(); it != g.environment.end(); ++it) {
        if (it.key().isEmpty() || it.key().contains('=') || it.key().contains(QChar::Null) || it.value().contains(QChar::Null)) fail("Invalid environment variable.");
        if (QStringList{"WINEPREFIX", "PROTONPATH", "GAMEID", "PROTON_VERB", "HOME"}.contains(it.key()) ||
            (managedRuntime && QStringList{"UMU_FOLDERS_PATH", "XDG_CACHE_HOME", "TMPDIR"}.contains(it.key())))
            fail("Novelty manages the runtime, cache and prefix path for " + it.key() + ".");
        c.environment.insert(it.key(), it.value());
    }
    if (!g.locale.isEmpty()) {
        c.environment.insert("HOST_LC_ALL", g.locale);
        c.environment.insert("LANG", g.locale);
    }
    c.environment.insert("WINEPREFIX", lib.prefix(g.id));
    c.environment.insert("PROTONPATH", canonicalPath(g.runner));
    c.environment.insert("GAMEID", g.umuGameId.isEmpty() ? "umu-default" : g.umuGameId);
    c.environment.insert("STORE", g.store.isEmpty() ? "none" : g.store);
    c.environment.insert("PROTON_VERB", "waitforexitandrun");
    c.environment.insert("UMU_RUNTIME_UPDATE", "0");
    // Pinned umu 1.4.4 honors these locations for its Steam Linux Runtime and downloads.
    // Existing libraries retain their prior runtime location until explicitly migrated.
    if (managedRuntime) {
        const auto cache = lib.dataRoot() + "/cache";
        const auto temporary = cache + "/tmp";
        for (const auto &path : {lib.dataRoot(), cache, temporary})
            if (QFileInfo(path).isSymbolicLink() || canonicalPath(path) != path)
                fail("Managed runtime/cache paths must stay inside Games.");
        if (!QDir().mkpath(temporary)) fail("Cannot create temporary runtime storage inside Games.");
        c.environment.insert("UMU_FOLDERS_PATH", lib.dataRoot() + "/runtime");
        c.environment.insert("XDG_CACHE_HOME", cache);
        c.environment.insert("TMPDIR", temporary);
    }
    // umu's documented empty-executable prefix creation starts Proton but then
    // returns a missing-file error. Run a harmless Wine command so preparation
    // has a meaningful exit status without starting the game.
    if (prepare) c.arguments = {"C:\\windows\\system32\\cmd.exe", "/c", "exit", "0"};
    else { c.arguments = {g.executable}; c.arguments.append(g.arguments); }
    return c;
}
Command prefixToolCommand(const Library &lib, const Game &g, const QString &tool) {
    if (g.backend != "umu") fail("Prefix tools are available for umu games. Steam manages its own prefixes.");
    if (tool != "winecfg" && tool != "regedit") fail("Unsupported prefix tool. Choose winecfg or regedit.");
    auto command = launchCommand(lib, g, true);
    // Use the prefix's own utility explicitly, never a same-named file in the game folder.
    // The file may not exist until supervised preparation completes.
    command.arguments = {lib.prefix(g.id) + "/drive_c/windows/" + (tool == "winecfg" ? "system32/winecfg.exe" : "regedit.exe")};
    return command;
}
Command winetricksCommand(const Library &lib, const Game &g, const QString &verb) {
    validateWinetricks({verb});
    if (g.backend != "umu") fail("Winetricks components require an umu game and a selected GE-Proton or UMU-Proton runner.");
    const auto runnerName = QFileInfo(g.runner).fileName();
    if (!runnerName.startsWith("GE-Proton", Qt::CaseInsensitive) &&
        !runnerName.startsWith("UMU-Proton", Qt::CaseInsensitive))
        fail("Winetricks integration requires a GE-Proton or UMU-Proton runner. Choose one in Manage → Runtime.");
    const auto bundledWinetricks = QDir(g.runner).filePath("protonfixes/winetricks");
    if (!QFileInfo(bundledWinetricks).isExecutable())
        fail("The selected Proton runner does not include executable Winetricks integration. "
             "Choose a complete GE-Proton or UMU-Proton installation; no system Wine or Protontricks is required.");
    auto command = launchCommand(lib, g, true);
    command.arguments = {"winetricks", verb};
    return command;
}
Command extraLaunchCommand(const Library &lib, const Game &g, const ExtraLaunch &extra) {
    if (g.backend != "umu") fail("Run another executable is available for umu games only.");
    if (!QDir::isAbsolutePath(extra.executable) || !QFileInfo(extra.executable).isFile() ||
        QFileInfo(extra.executable).suffix().compare("exe", Qt::CaseInsensitive) != 0)
        fail("Choose an existing Windows .exe using its full path.");
    const auto cwd = extra.workingDirectory.isEmpty() ? QFileInfo(extra.executable).absolutePath() : extra.workingDirectory;
    if (!QDir::isAbsolutePath(cwd) || !QFileInfo(cwd).isDir()) fail("Choose an existing working directory using its full path.");
    auto context = g;
    if (context.installPending) { context.installPending = false; context.executable = extra.executable; context.workingDirectory = cwd; }
    auto command = launchCommand(lib, context);
    auto paths = g; paths.executable = canonicalPath(extra.executable); paths.workingDirectory = canonicalPath(cwd);
    const auto issue = launchPathIssue(paths); if (!issue.isEmpty()) fail(issue);
    for (const auto &path : {paths.executable, paths.workingDirectory})
        if (lib.removalReservedFolder(path)) fail("The selected path is reserved by interrupted removal. Use Rescan to recover it first.");
    qsizetype size = 0;
    for (const auto &argument : extra.arguments) {
        if (argument.contains(QChar::Null)) fail("Arguments cannot contain a null character.");
        size += argument.toUtf8().size() + 1;
    }
    if (size > 64 * 1024) fail("Arguments exceed the 64 KiB limit.");
    command.cwd = paths.workingDirectory;
    command.arguments = {paths.executable}; command.arguments.append(extra.arguments);
    return command;
}
QStringList activeExtraPaths(const Library &lib, const Game &g) {
    if (!lib.busy(g.id)) return {};
    const auto record = readJson(lib.gamePath(g.id, "extra-launch.json"));
    if (record.isEmpty()) return {};
    if (record["schema"].toInt() != 1) fail("An active executable operation record cannot be read; wait for it to finish before removal.");
    if (record["state"] == "preparing" || record["state"] == "running") {
        const auto executable = record["executable"].toString(), cwd = record["cwd"].toString();
        if (!QDir::isAbsolutePath(executable) || !QDir::isAbsolutePath(cwd)) fail("An active executable operation has invalid paths; removal was blocked.");
        return {executable, cwd};
    }
    if (record["state"] != "complete" && record["state"] != "failed" && record["state"] != "interrupted") fail("An active executable operation has an unknown state; removal was blocked.");
    return {};
}
int supervise(Library &lib, const QString &id, bool prepareOnly, const QString &tool, const std::optional<ExtraLaunch> &request,
              bool installing, bool debugWine, const QString &winetricksApprovalToken) {
    auto extra = request;
    if (!lib.recoveryIssue(id).isEmpty()) return 2;
    QDir().mkpath(QFileInfo(lib.gamePath(id, "launch.lock")).absolutePath());
    QLockFile lock(lib.gamePath(id, "launch.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock() || !lib.recoveryIssue(id).isEmpty()) return 2;
    QJsonObject operation;
    const auto operationPath = lib.gamePath(id, "extra-launch.json");
    try {
        const auto g = lib.game(id);
        const auto requiredApproval = winetricksApproval(lib, g);
        if (!requiredApproval.isEmpty() && requiredApproval != winetricksApprovalToken)
            fail("Winetricks components need approval before preparation: " + pendingWinetricks(lib, g).join(", ") +
                 ". Open this game in Novelty and choose Play or Prepare to review the changes.");
        if (debugWine && (installing || prepareOnly || !tool.isEmpty() || extra || g.backend != "umu"))
            fail("Wine debug is available for a single umu game launch only.");
        if (installing) {
            if (!g.installPending || prepareOnly || !tool.isEmpty() || extra) fail("Choose a pending installation to run its installer.");
            extra = ExtraLaunch{g.installer, {}, {}};
        }
        if (prepareOnly && !tool.isEmpty()) fail("Choose preparation or a prefix tool, not both.");
        if (extra && (prepareOnly || !tool.isEmpty())) fail("Choose one executable operation at a time.");
        auto cmd = extra ? extraLaunchCommand(lib, g, *extra) : tool.isEmpty() ? launchCommand(lib, g, prepareOnly) : prefixToolCommand(lib, g, tool);
        if (debugWine) cmd.environment.insert("WINEDEBUG", "+seh,+loaddll");
        if (extra) {
            // Publish temporary path references while removal/import/edit mutations are excluded.
            QLockFile mutation(lib.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
            if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry the executable shortly.");
            cmd = extraLaunchCommand(lib, g, *extra);
            operation = {{"schema", 1}, {"attempt", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"state", "preparing"}, {"kind", installing ? "installer" : "extra"}, {"started_at", now()}, {"executable", cmd.arguments.first()},
                {"cwd", cmd.cwd}, {"arguments", QJsonArray::fromStringList(extra->arguments)},
                {"umu", cmd.program}, {"runner", canonicalPath(g.runner)}, {"runner_version", runnerVersion(g.runner)},
                {"prefix", lib.prefix(id)}, {"profile", g.setupProfile}};
            const auto previous = readJson(operationPath);
            if (!previous.isEmpty()) writeJson(operationPath + ".previous", previous);
            writeJson(operationPath, operation);
        } else {
            // The lock and crash barrier above prove no prior operation remains active.
            // A normal launch after reboot must not revive old temporary path references.
            auto previous = readJson(operationPath);
            if (previous["schema"].toInt() == 1 && (previous["state"] == "preparing" || previous["state"] == "running")) {
                previous["state"] = "interrupted"; previous["finished_at"] = now();
                writeJson(operationPath, previous);
            }
        }
        const auto logPath = lib.gamePath(id, "launch.log");
        QFile::remove(logPath + ".previous"); QFile::rename(logPath, logPath + ".previous");
        QFile log(logPath);
        if (!log.open(QIODevice::WriteOnly)) fail("Cannot create launch log: " + log.errorString());
        log.write(("Novelty 0.1 | " + now() + "\nGame: " + g.name + "\nBackend: " + g.backend + "\n" +
                   (debugWine ? "One-launch Wine debug: +seh,+loaddll (log capped at 1 MiB)\n" : "")).toUtf8());
        if (g.backend == "steam") {
            // Persist before dispatch: a crash immediately after sending must not enable deletion.
            writeJson(lib.gamePath(id, "steam-request.json"), {{"state", "untracked"}, {"updated", now()}});
            QProcess p; p.setProgram(cmd.program); p.setArguments(cmd.arguments); p.setWorkingDirectory(cmd.cwd);
            if (!p.startDetached()) fail("Steam could not receive the launch request.");
            lib.recordStatus(id, "Requested in Steam", "Steam owns the game process. This prototype cannot confirm shortcut startup/exit.");
            return 0;
        }
        // Explicit Prepare also repairs an already-completed Proton prefix without deleting it.
        if (g.backend == "umu") log.write(("Setup profile: " + g.setupProfile + "\n").toUtf8());
        if (g.backend == "umu" && (prepareOnly || !setupComplete(lib, g))) {
            lib.recordStatus(id, "Preparing");
            preparePrefix(lib, g, [&] { return executeTracked(lib, id, launchCommand(lib, g, true), log); });
        }
        if (g.backend == "umu" && !pendingWinetricks(lib, g).isEmpty()) {
            lib.recordStatus(id, "Preparing", "Installing approved Winetricks components.");
            prepareWinetricks(lib, g, [&](const QString &verb) {
                log.write(("Winetricks: " + verb + "\n").toUtf8()); log.flush();
                return executeTracked(lib, id, winetricksCommand(lib, g, verb), log);
            });
        }
        SaveServerConfig saveServer;
        bool serverMode = false;
        QString serverSetupIssue;
        ServerSyncResult beforeSync{ServerSyncStatus::Unavailable, {}};
        if (!extra && tool.isEmpty()) {
            try {
                saveServer = loadSaveServerConfig(lib);
                serverMode = saveServer.enabled;
                if (serverMode) {
                    beforeSync = syncServerBeforePlay(lib, id, saveServer);
                    if (!beforeSync.detail.isEmpty())
                        log.write(("Save server before Play: " + beforeSync.detail + "\n").toUtf8());
                }
            } catch (const std::exception &error) {
                serverSetupIssue = QString::fromUtf8(error.what());
                log.write(("Save server settings need attention: " + serverSetupIssue + "\n").toUtf8());
            }
        }
        if (prepareOnly) {
            if (serverMode && beforeSync.status == ServerSyncStatus::Conflict)
                lib.recordStatus(id, "Save conflict", beforeSync.detail);
            else if (serverMode && beforeSync.status == ServerSyncStatus::Pending)
                lib.recordStatus(id, "Sync pending", beforeSync.detail);
            else if (!serverSetupIssue.isEmpty())
                lib.recordStatus(id, "Sync pending", serverSetupIssue);
            else lib.recordStatus(id, "Ready");
            return 0;
        }
        if (!tool.isEmpty()) {
            if (!QFileInfo(cmd.arguments.first()).isFile())
                fail("The selected runner did not provide " + tool + " in this prefix. Review the runner or use Prepare in Manage.");
            log.write(("Prefix tool: " + tool + "\n").toUtf8());
        }
        if (installing) {
            const auto mapping = lib.prefix(id) + "/dosdevices/n:";
            if (!QDir().mkpath(QFileInfo(mapping).absolutePath())) fail("Cannot create installer drive mapping.");
            if (QFileInfo::exists(mapping) || QFileInfo(mapping).isSymLink()) {
                if (canonicalPath(mapping) != canonicalPath(g.folder)) fail("N: already maps to another location. Review the prefix in Manage before retrying.");
            } else if (!QFile::link(g.folder, mapping)) fail("Cannot map N: to the installation folder.");
            log.write(("Installer destination: N:\\ → " + g.folder + "\n").toUtf8());
        }
        if (extra) {
            // Recheck after preparation: the selected file or its mount may have disappeared.
            cmd = extraLaunchCommand(lib, g, *extra);
            if (cmd.arguments.first() != operation["executable"].toString() || cmd.cwd != operation["cwd"].toString() || cmd.program != operation["umu"].toString())
                fail("An executable path changed during preparation. Review the selection and Settings before retrying.");
            operation["state"] = "running"; writeJson(operationPath, operation);
            log.write(("One-time executable: " + cmd.arguments.first() + "\nWorking directory: " + cmd.cwd + "\n").toUtf8());
        }
        lib.recordStatus(id, extra || !tool.isEmpty() ? "Configuring" : "Running",
            extra ? "Running another executable" : debugWine ? "Wine debug enabled for this launch" : tool.isEmpty() ? QString() : "Prefix tool: " + tool);
        const bool ordinaryPlay = !extra && tool.isEmpty();
        QString discoveryIssue, snapshotIssue;
        if (ordinaryPlay) {
            try { finishPostExitSaveSnapshot(lib, g); }
            catch (const std::exception &e) { snapshotIssue = QString::fromUtf8(e.what()); log.write(("Earlier save snapshot pending: " + snapshotIssue + "\n").toUtf8()); }
            try { beginSaveObservation(lib, g); }
            catch (const std::exception &e) { discoveryIssue = QString::fromUtf8(e.what()); log.write(("Save discovery needs attention: " + discoveryIssue + "\n").toUtf8()); }
            try { queuePostExitSaveSnapshot(lib, g); }
            catch (const std::exception &e) { snapshotIssue = QString::fromUtf8(e.what()); log.write(("Save snapshot needs attention: " + snapshotIssue + "\n").toUtf8()); }
        }
        const auto finishSaves = [&] {
            if (!ordinaryPlay) return;
            try { finishSaveObservation(lib, g); discoveryIssue.clear(); }
            catch (const std::exception &e) { discoveryIssue = QString::fromUtf8(e.what()); log.write(("Save discovery needs attention: " + discoveryIssue + "\n").toUtf8()); }
            try { queuePostExitSaveSnapshot(lib, g); finishPostExitSaveSnapshot(lib, g); snapshotIssue.clear(); }
            catch (const std::exception &e) { snapshotIssue = QString::fromUtf8(e.what()); log.write(("Save snapshot pending: " + snapshotIssue + "\n").toUtf8()); }
        };
        int result = 0;
        try { result = executeTracked(lib, id, cmd, log); }
        catch (...) {
            finishSaves();
            throw;
        }
        finishSaves();
        ServerSyncResult afterSync{ServerSyncStatus::Unavailable, {}};
        if (serverMode && ordinaryPlay && snapshotIssue.isEmpty()) {
            try {
                const auto currentServer = loadSaveServerConfig(lib);
                if (currentServer.enabled && currentServer.url == saveServer.url &&
                    currentServer.token == saveServer.token && currentServer.serverId == saveServer.serverId)
                    afterSync = syncServerAfterPlay(lib, id, saveServer);
                else afterSync = {ServerSyncStatus::Pending, "Save server settings changed during Play. Review them before syncing."};
            } catch (const std::exception &error) {
                afterSync = {ServerSyncStatus::Pending, QString::fromUtf8(error.what())};
            }
            if (!afterSync.detail.isEmpty())
                log.write(("Save server after Play: " + afterSync.detail + "\n").toUtf8());
        }
        if (extra) operation["exit_code"] = result;
        if (result != 0) fail((extra ? QString("One-time executable") : tool.isEmpty() ? QString("Launch") : tool) + " exited with code " + QString::number(result) + ". See the launch log.");
        if (extra) { operation["state"] = "complete"; operation["finished_at"] = now(); writeJson(operationPath, operation); }
        if (installing) {
            // A completed operation remains durable if detection fails or the process dies here.
            operation = {}; lib.finishInstallation(id); return 0;
        }
        if (!discoveryIssue.isEmpty() || !snapshotIssue.isEmpty()) {
            QStringList issues;
            if (!discoveryIssue.isEmpty()) issues.append(discoveryIssue);
            if (!snapshotIssue.isEmpty() && snapshotIssue != discoveryIssue) issues.append(snapshotIssue);
            lib.recordStatus(id, "Save needs attention", "Progress was not backed up: " + issues.join(" "));
            return 0; // Backup failure must not turn a successful offline Play into a launch failure.
        }
        if (serverMode && ordinaryPlay) {
            if (afterSync.status == ServerSyncStatus::Conflict)
                lib.recordStatus(id, "Save conflict", afterSync.detail);
            else if (afterSync.status == ServerSyncStatus::Pending)
                lib.recordStatus(id, "Sync pending", afterSync.detail);
            else lib.recordStatus(id, "Ready", "Saves synced with the configured server.");
            return 0;
        }
        if (!serverSetupIssue.isEmpty() && ordinaryPlay) {
            lib.recordStatus(id, "Sync pending", serverSetupIssue); return 0;
        }
        lib.recordStatus(id, "Ready", extra ? "Executable closed. The game's saved launch settings are unchanged." :
            tool.isEmpty() ? QString() : tool + " closed. Changes made in the tool stay in this game's prefix."); return 0;
    } catch (const std::exception &e) {
        if (!operation.isEmpty()) {
            operation["state"] = "failed"; operation["message"] = QString::fromUtf8(e.what()); operation["finished_at"] = now();
            writeJson(operationPath, operation);
        }
        lib.recordStatus(id, "Needs attention", QString::fromUtf8(e.what())); return 1;
    }
}
} // namespace novelty
