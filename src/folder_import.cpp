#include "core.h"
#include "recipes.h"
#include "runtime.h"
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qprocess.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/quuid.h>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool within(const QString &path, const QString &parent) { return path == parent || path.startsWith(parent + '/'); }
bool exists(const QString &path) { const QFileInfo info(path); return info.exists() || info.isSymLink(); }
QString movedPath(const QString &path, const QString &oldFolder, const QString &newFolder) {
    if (path.isEmpty()) return {};
    const auto relative = QDir(oldFolder).relativeFilePath(path);
    return relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)
        ? path : QDir::cleanPath(QDir(newFolder).filePath(relative));
}
QMap<QString, QFileInfo> tree(const QString &root) {
    QMap<QString, QFileInfo> entries;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto info = it.nextFileInfo();
        entries.insert(QDir(root).relativeFilePath(info.absoluteFilePath()), info);
    }
    return entries;
}
QByteArray linkText(const QString &path) {
    QByteArray result(4096, '\0');
    const auto length = readlink(QFile::encodeName(path).constData(), result.data(), result.size());
    if (length < 0 || length == result.size()) fail("Cannot verify symbolic link: " + path);
    result.resize(length); return result;
}
bool sameContents(const QString &a, const QString &b) {
    const auto left = tree(a), right = tree(b);
    if (left.keys() != right.keys()) return false;
    for (auto it = left.cbegin(); it != left.cend(); ++it) {
        const auto x = it.value(), y = right.value(it.key());
        if (x.isSymLink() != y.isSymLink() || x.isDir() != y.isDir() || x.isFile() != y.isFile()) return false;
        if (x.isSymLink()) { if (linkText(x.absoluteFilePath()) != linkText(y.absoluteFilePath())) return false; continue; }
        if (!x.isFile()) continue;
        if (x.size() != y.size()) return false;
        QFile first(x.absoluteFilePath()), second(y.absoluteFilePath());
        if (!first.open(QIODevice::ReadOnly) || !second.open(QIODevice::ReadOnly)) fail("Cannot verify copied game files.");
        while (!first.atEnd()) if (first.read(1024 * 1024) != second.read(1024 * 1024)) return false;
        if (!second.atEnd()) return false;
    }
    return true;
}
bool sameDevice(const QString &a, const QString &b) {
    struct stat left{}, right{};
    if (stat(QFile::encodeName(a).constData(), &left) || stat(QFile::encodeName(b).constData(), &right))
        fail("Cannot inspect the source or Games filesystem.");
    return left.st_dev == right.st_dev;
}
void persistImportedTree(const QString &path) {
    const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) fail("Cannot open the imported filesystem for synchronization. The source was kept.");
    // Linux syncfs persists copied file data, symlinks and the destination rename
    // together, before another filesystem is allowed to delete the original.
    const int result = syncfs(fd);
    close(fd);
    if (result != 0) fail("Cannot persist the imported game. The source was kept.");
}
}

FolderImportResult Library::addFolder(const QString &chosen, const QString &nativeLauncher) {
    if (chosen.trimmed().isEmpty() || !QFileInfo(chosen).isDir()) fail("Choose an existing game folder.");
    if (QFileInfo(chosen).isSymLink()) fail("Select the actual game folder, not a symbolic link.");
    const auto source = canonicalPath(chosen), destination = QDir(root()).filePath(QFileInfo(source).fileName());
    QString selectedNative;
    if (!nativeLauncher.isEmpty()) {
        selectedNative = canonicalPath(nativeLauncher);
        if (!selectedNative.startsWith(source + '/') || !nativeExecutable(nativeLauncher))
            fail("Choose an executable Linux game inside the selected folder.");
    }
    if (source == root() || within(root(), source) || source == dataRoot() || within(source, dataRoot()))
        fail("Select a game's own folder, outside Novelty's library data.");
    if (source != destination && within(source, root()))
        fail("Select a direct child of Games, or a game folder outside Games.");
    QStringList protectedFolders{QDir::homePath(), "/tmp", "/var", "/usr", "/etc", "/opt", "/home", "/run", "/media", "/mnt"};
    for (auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
        protectedFolders.append(QStandardPaths::standardLocations(location));
    for (const auto &protectedPath : protectedFolders) if (source == canonicalPath(protectedPath))
        fail("Select a game's own folder, not a personal or system directory.");
    struct stat sourceStat{}, parentStat{};
    if (stat(QFile::encodeName(source).constData(), &sourceStat) ||
        stat(QFile::encodeName(QFileInfo(source).absolutePath()).constData(), &parentStat))
        fail("Cannot inspect the selected folder.");
    if (sourceStat.st_dev != parentStat.st_dev)
        fail("Select a game folder inside the mounted drive, not the drive's mount point.");

    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry adding the folder shortly.");
    if (removalReservedFolder(source) || removalReservedFolder(destination))
        fail("This folder is reserved by interrupted removal. Use Rescan first.");
    const auto existing = games(); Game previous;
    for (const auto &g : existing) {
        if (g.folder == source) previous = g;
        else if (within(source, g.folder) || within(g.folder, source))
            fail("This folder overlaps another registered game: " + g.name);
        QStringList references{g.runner, g.installer};
        if (g.folder != source) references.append({g.executable, g.workingDirectory});
        references.append(activeExtraPaths(*this, g));
        for (const auto &reference : references) if (!reference.isEmpty() &&
            (within(canonicalPath(reference), source) || within(source, canonicalPath(reference))))
            fail("This folder overlaps files used by " + g.name);
    }
    if (source == destination) {
        if (!previous.id.isEmpty()) {
            if (!selectedNative.isEmpty()) fail("This game is already registered. Change its executable in Manage → Launch.");
            return {previous, {}, false};
        }
    } else {
        if (exists(destination)) fail("A folder with this name already exists in Games: " + destination);
        if (!previous.id.isEmpty() && (previous.backend == "steam" || busy(previous.id)))
            fail("This registered game is running or uses Steam; move it with Manage after it is idle.");
    }
    std::unique_ptr<QLockFile> gameLock;
    if (!previous.id.isEmpty() && source != destination) {
        const auto path = gamePath(previous.id, "launch.lock");
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) fail("Cannot lock the registered game.");
        gameLock = std::make_unique<QLockFile>(path); gameLock->setStaleLockTime(0);
        if (!gameLock->tryLock()) fail("Wait until the game finishes before moving it.");
    }

    const auto identity = folderIdentity(source);
    if (identity.isEmpty()) fail("Cannot verify this folder's identity before moving it.");
    const bool move = source != destination;
    const bool direct = !move || sameDevice(source, root());
    const auto transferId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto stage = root() + "/.novelty-importing-" + transferId;
    const auto journal = dataRoot() + "/imports/" + transferId + ".json";
    bool placed = false;
    if (move) {
        if (direct) {
            if (!QDir().rename(source, destination)) fail("Could not move the folder into Games. The source was kept: " + source);
        } else {
            // The intent precedes staging. Recovery only removes a stage whose
            // recorded directory identity still matches and whose source survives.
            writeJson(journal, {{"schema", 1}, {"source", source}, {"source_identity", identity},
                {"destination", destination}, {"stage", stage}});
            if (!QDir().mkdir(stage)) fail("Could not create a new import staging folder. The source was kept.");
            const auto stageIdentity = folderIdentity(stage);
            if (stageIdentity.isEmpty()) fail("Could not identify the import staging folder. The source was kept.");
            writeJson(journal, {{"schema", 1}, {"source", source}, {"source_identity", identity},
                {"destination", destination}, {"stage", stage}, {"stage_identity", stageIdentity}});
            QProcess copy;
            copy.start("cp", {"-a", "--reflink=auto", "--", source + "/.", stage});
            if (!copy.waitForStarted() || !copy.waitForFinished(-1) || copy.exitStatus() != QProcess::NormalExit || copy.exitCode() != 0)
                fail("Could not copy the game into Games. The source was kept. Temporary copy: " + stage + "\n" + QString::fromUtf8(copy.readAllStandardError()));
            if (folderIdentity(source) != identity || !sameContents(source, stage))
                fail("The source changed during transfer or the copy differs. The source was kept. Temporary copy: " + stage);
            if (exists(destination) || !QDir().rename(stage, destination))
                fail("Could not place the copied game. The source was kept. Temporary copy: " + stage);
        }
        placed = true;
    }
    try {
        const auto path = move ? destination : source;
        if (!QFileInfo(path).isDir()) fail("The game folder disappeared before it could be scanned.");
        Game g;
        if (previous.id.isEmpty()) {
            const auto candidate = inspect(path, root());
            g.id = QUuid::createUuid().toString(QUuid::WithoutBraces); g.name = candidate.name; g.folder = path;
            g.runner = settings()["runner"].toString(); g.backend = candidate.native ? "native" : settings()["backend"].toString("umu");
            g.runnerRevision = runnerVersion(g.runner); g.source = "discovered";
            const auto choice = selectRecipe(root(), path, candidate.executables,
                settings()["default_recipe"].toString("general-game"));
            auto executables = candidate.executables;
            if (choice.ambiguous()) {
                for (const auto &recipe : choice.matches) g.recipeCandidates.append(recipe.id);
            } else {
                applyRecipeToGame(g, choice.selected());
                if (choice.selected().tier == "general" && candidate.native) {
                    g.backend = "native"; g.runner.clear(); g.runnerRevision.clear(); g.runnerSelector.clear();
                }
                if (choice.selected().settings.contains("runner") &&
                    ((choice.selected().tier != "general" && choice.selected().runnerExplicit) ||
                     !QFileInfo(QDir(g.runner).filePath("proton")).isFile())) {
                    g.runner.clear(); g.runnerRevision.clear();
                }
                executables = recipeExecutables(choice.selected(), path, executables);
            }
            if (executables.size() == 1) g.executable = executables.first();
            g.workingDirectory = g.executable.isEmpty() ? path : QFileInfo(g.executable).absolutePath();
            if (!choice.ambiguous() && !choice.selected().settings["working_directory"].toString().isEmpty())
                g.workingDirectory = canonicalPath(QDir(path).filePath(choice.selected().settings["working_directory"].toString()));
            if (!selectedNative.isEmpty()) {
                g.executable = movedPath(selectedNative, source, path);
                if (!nativeExecutable(g.executable)) fail("The selected Linux executable changed during import.");
                g.workingDirectory = QFileInfo(g.executable).absolutePath();
                g.backend = "native"; g.runner.clear(); g.runnerRevision.clear();
            }
            if (!choice.ambiguous()) g.recipeBase = portableGameSettings(g);
        } else {
            g = previous; g.folder = path; g.executable = movedPath(g.executable, source, path);
            g.workingDirectory = movedPath(g.workingDirectory, source, path);
            if (!g.executable.isEmpty() && !QFileInfo(g.executable).isFile()) fail("The saved executable is missing at the destination.");
            if (!QFileInfo(g.workingDirectory).isDir()) fail("The saved working directory is missing at the destination.");
        }
        g.folderIdentity = folderIdentity(path);
        if (g.folderIdentity.isEmpty()) fail("Cannot verify the moved folder's identity.");
        if (move && !direct) persistImportedTree(destination);
        store(g);
        QString warning;
        if (move && !direct) {
            const auto retired = QFileInfo(source).absolutePath() + "/.novelty-moved-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (folderIdentity(source) != identity || !QDir().rename(source, retired))
                warning = "Game added, but the original could not be retired. Remove it manually after checking: " + source;
            else {
                // Registration has committed. Cleanup failure must never move the
                // published destination away from the database's saved path.
                try {
                    if (!sameContents(retired, destination) || !QDir(retired).removeRecursively())
                        warning = "Game added, but the original copy remains for review: " + retired;
                } catch (const std::exception &error) {
                    warning = "Game added, but the original copy remains for review: " + retired + "\n" + QString::fromUtf8(error.what());
                }
            }
            QFile::remove(journal);
        }
        return {g, warning, move};
    } catch (...) {
        if (placed) {
            if (direct) { if (!QDir().rename(destination, source)) fail("Import failed; the moved folder remains in Games: " + destination); }
            else if (QFileInfo(destination).isDir() && !QDir().rename(destination, stage))
                fail("Import failed; the copied folder remains in Games: " + destination);
        }
        throw;
    }
}

QStringList Library::recoverFolderImports() {
    QStringList messages;
    QLockFile mutation(dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) return {"Library maintenance is active elsewhere. Use Rescan to retry folder import recovery."};
    const QDir jobs(dataRoot() + "/imports");
    for (const auto &name : jobs.entryList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        const auto path = jobs.filePath(name);
        try {
            const auto record = readJson(path);
            const auto id = name.left(name.size() - 5);
            const auto stage = root() + "/.novelty-importing-" + id;
            const auto source = record["source"].toString(), destination = record["destination"].toString();
            const auto sourceIdentity = record["source_identity"].toString();
            const auto stageIdentity = record["stage_identity"].toString();
            if (QUuid(id).isNull() || record["schema"].toInt() != 1 || record["stage"].toString() != stage ||
                source.isEmpty() || !QDir::isAbsolutePath(source) || sourceIdentity.isEmpty() ||
                destination != QDir(root()).filePath(QFileInfo(source).fileName()) || destination == root())
                fail("Invalid folder import recovery record: " + path);
            if (exists(destination)) fail("The import destination exists; review both copies before clearing: " + path);
            if (!exists(stage)) {
                if (!QFile::remove(path)) fail("Could not clear finished import record: " + path);
                continue;
            }
            if (stageIdentity.isEmpty() || QFileInfo(stage).isSymLink() || folderIdentity(stage) != stageIdentity ||
                folderIdentity(source) != sourceIdentity)
                fail("An import copy or its original changed; review it before clearing: " + stage);
            for (const auto &game : games()) if (within(game.folder, stage) || within(stage, game.folder) ||
                within(game.runner, stage) || within(game.installer, stage))
                fail("A registered game refers to an interrupted import copy: " + stage);
            if (!QDir(stage).removeRecursively() || !QFile::remove(path))
                fail("Could not clear the interrupted import copy: " + stage);
            messages.append("Removed an incomplete imported copy; the original was kept: " + source);
        } catch (const std::exception &e) { messages.append(QString::fromUtf8(e.what())); }
    }
    return messages;
}
} // namespace novelty
