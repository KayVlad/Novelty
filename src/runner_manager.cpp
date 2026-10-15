#include "runner_manager.h"
#include "recipes.h"
#include "app_settings.h"
#include <QtCore/qdir.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qtimer.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool validVersion(const QString &version) {
    static const QRegularExpression pattern("^GE-Proton[0-9]+-[0-9]+$");
    return pattern.match(version).hasMatch();
}
QString pathFor(const Library &lib, const QString &version) {
    return lib.dataRoot() + "/runners/" + version;
}
bool verified(const QString &path, const QString &version) {
    if (canonicalPath(path) != QDir::cleanPath(QFileInfo(path).absoluteFilePath()) ||
        !QFileInfo(path).isDir() || QFileInfo(path).isSymLink() ||
        QFileInfo(path + "/novelty-install.json").isSymLink()) return false;
    try {
        const auto receipt = readJson(path + "/novelty-install.json");
        static const QRegularExpression checksum("^[0-9a-f]{128}$");
        return receipt["schema"] == 1 && receipt["version"] == version && receipt["architecture"] == "x86_64" &&
            checksum.match(receipt["sha512"].toString()).hasMatch() &&
            QFileInfo(path + "/proton").isFile() && QFileInfo(path + "/proton").isExecutable() &&
            QFileInfo(path + "/version").isFile();
    } catch (...) { return false; }
}
QString issue(const Library &lib, const QString &version, bool checkBusy) {
    if (!validVersion(version)) return "Choose an exact GE-Proton version.";
    const auto path = pathFor(lib, version);
    if (!verified(path, version)) return "This managed GE build is missing or fails its install receipt checks.";
    const auto settings = lib.settings();
    if (settings["runner"] == path || settings["runner"] == version ||
        resolveRunnerSelector(lib.root(), settings["runner"].toString()) == path)
        return "This build is the library's selected default runner.";
    if (readJson(lib.dataRoot() + "/ge-proton.json")["version"] == version)
        return "This build is the library's GE-Proton pin.";
    for (const auto &game : lib.games()) {
        if (checkBusy && (lib.busy(game.id) || !lib.recoveryIssue(game.id).isEmpty()))
            return "A game is active or needs recovery.";
        if (game.runner == path || game.runnerSelector == version ||
            (!game.runnerSelector.isEmpty() && resolveRunnerSelector(lib.root(), game.runnerSelector) == path))
            return "This build is selected by game " + game.name + ".";
    }
    for (const auto &recipe : loadRecipes(lib.root())) {
        const auto selector = recipe.settings["runner"].toString();
        if (selector == version || (!selector.isEmpty() && resolveRunnerSelector(lib.root(), selector) == path))
            return "This build is referenced by recipe " + recipe.id + ".";
    }
    return {};
}
}
QStringList externalProtonLocations() {
    QStringList paths;
    for (const auto &value : loadLocalSettings()["externalProtonPaths"].toArray()) {
        if (!value.isString() || !QDir::isAbsolutePath(value.toString())) fail("Invalid registered Proton path.");
        paths.append(value.toString());
    }
    paths.removeDuplicates();
    return paths;
}
namespace {
QStringList externalVersions(const QString &directory) {
    QStringList paths;
    if (!QFileInfo(directory).isDir()) return paths;
    if (QFileInfo(directory + "/proton").isFile()) paths.append(canonicalPath(directory));
    for (const auto &entry : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!entry.fileName().startsWith('.') && QFileInfo(entry.filePath() + "/proton").isFile())
            paths.append(canonicalPath(entry.filePath()));
    paths.removeDuplicates();
    return paths;
}
}
void addExternalProtonLocation(const QString &input) {
    auto path = input.trimmed();
    if (path.startsWith("~/")) path = QDir::homePath() + path.mid(1);
    if (!QDir::isAbsolutePath(path) || !QFileInfo(path).isDir() || !QFileInfo(path).isReadable() ||
        externalVersions(path).isEmpty()) fail("Choose a Proton installation, or a directory containing Proton installations.");
    auto paths = externalProtonLocations();
    path = canonicalPath(path);
    if (!paths.contains(path)) paths.append(path);
    if (paths.size() > 64) fail("Register at most 64 external Proton locations.");
    setLocalSetting("externalProtonPaths", QJsonArray::fromStringList(paths));
}
void removeExternalProtonLocation(const QString &path) {
    auto paths = externalProtonLocations(); paths.removeAll(path);
    setLocalSetting("externalProtonPaths", QJsonArray::fromStringList(paths));
}
bool runnerInstallationReady(const QString &libraryRoot, const QString &path) {
    if (path.isEmpty() || !QFileInfo(path + "/proton").isFile()) return false;
    const auto managed = QDir(canonicalPath(libraryRoot)).absoluteFilePath(".novelty/runners") + '/';
    const auto target = canonicalPath(path);
    if (!libraryRoot.isEmpty()) {
        if (QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(managed))
            return validVersion(QFileInfo(path).fileName()) && verified(path, QFileInfo(path).fileName());
        if (target.startsWith(managed))
            return validVersion(QFileInfo(target).fileName()) && verified(target, QFileInfo(target).fileName());
    }
    return true;
}
QStringList protonInventory(const QString &libraryRoot) {
    QStringList paths;
    if (!libraryRoot.isEmpty()) {
        const auto directory = QDir(libraryRoot).filePath(".novelty/runners");
        for (const auto &entry : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
            if (validVersion(entry.fileName()) && verified(entry.filePath(), entry.fileName())) paths.append(canonicalPath(entry.filePath()));
    }
    QStringList locations;
    for (const auto &steam : {QDir::homePath() + "/.local/share/Steam", QDir::homePath() + "/.steam/steam",
                             QDir::homePath() + "/.var/app/com.valvesoftware.Steam/data/Steam"})
        locations.append({steam + "/compatibilitytools.d", steam + "/steamapps/common"});
    locations.append(externalProtonLocations());
    for (const auto &location : locations)
        for (const auto &path : externalVersions(location))
            if (runnerInstallationReady(libraryRoot, path)) paths.append(path);
    paths.removeDuplicates(); return paths;
}
bool geInstallInProgress(const QString &libraryRoot, const QString &version) {
    const auto directory = libraryRoot + "/.novelty/runners";
    const auto names = version.isEmpty() ? QDir(directory).entryList({".GE-Proton*.install.lock"}, QDir::Files | QDir::Hidden) :
        QStringList{"." + version + ".install.lock"};
    for (const auto &name : names) {
        const int fd = open(QFile::encodeName(directory + '/' + name).constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        const bool locked = flock(fd, LOCK_EX | LOCK_NB) != 0;
        if (!locked) flock(fd, LOCK_UN);
        close(fd);
        if (locked) return true;
    }
    return false;
}
bool newerGeVersion(const QString &a, const QString &b) {
    if (!validVersion(a)) return false;
    if (!validVersion(b)) return true;
    const auto parts = [](const QString &v) { const auto p = v.mid(9).split('-'); return std::pair(p[0].toInt(), p[1].toInt()); };
    return parts(a) > parts(b);
}
bool isRunnerAlias(const QString &value) {
    return value == "ge-proton-latest" || value == "proton-latest" || value == "proton-experimental";
}
QList<GeRelease> cachedGeCatalog(const QString &libraryRoot) {
    const auto cached = readJson(libraryRoot + "/.novelty/ge-catalog.json");
    return cached["schema"] == 1 ? parseGeCatalog(QJsonDocument(cached["releases"].toArray()).toJson()) : QList<GeRelease>{};
}
void cacheGeCatalog(const QString &libraryRoot, const QList<GeRelease> &releases) {
    QJsonArray compact;
    for (const auto &entry : releases)
        compact.append(QJsonObject{{"tag_name", entry.version}, {"published_at", entry.published},
            {"assets", QJsonArray{QJsonObject{{"name", entry.version + ".tar.gz"}, {"size", entry.archiveSize}},
                                  QJsonObject{{"name", entry.version + ".sha512sum"}}}}});
    writeJson(libraryRoot + "/.novelty/ge-catalog.json", {{"schema", 1}, {"fetched", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                                        {"releases", compact}});
}
QList<GeRelease> parseGeCatalog(const QByteArray &json) {
    if (json.size() > 2 * 1024 * 1024) fail("GE release catalog is too large.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) fail("Invalid GE release catalog response.");
    QList<GeRelease> releases;
    for (const auto &value : document.array()) {
        const auto item = value.toObject();
        const auto version = item["tag_name"].toString();
        if (!validVersion(version) || item["draft"].toBool() || item["prerelease"].toBool()) continue;
        qint64 size = 0;
        for (const auto &name : {version + "-x86_64.tar.gz", version + ".tar.gz"}) {
            int archives = 0, checksums = 0;
            qint64 archiveSize = 0;
            for (const auto &asset : item["assets"].toArray()) {
                const auto file = asset.toObject();
                if (file["name"] == name) { ++archives; archiveSize = file["size"].toInteger(); }
                if (file["name"] == name.chopped(7) + ".sha512sum") ++checksums;
            }
            if (archives) {
                if (archives == 1 && checksums == 1) size = archiveSize;
                break; // Match the installer: never fall back after finding an architecture-specific archive.
            }
        }
        if (size > 0 && size <= 4LL * 1024 * 1024 * 1024)
            releases.append({version, item["published_at"].toString(), size});
    }
    std::sort(releases.begin(), releases.end(), [](const auto &a, const auto &b) { return newerGeVersion(a.version, b.version); });
    return releases;
}
GeCatalog fetchGeCatalog(const QString &libraryRoot, const QUrl &endpoint, int timeoutMs) {
    GeCatalog result;
    const auto cachePath = QDir(libraryRoot).filePath(".novelty/ge-catalog.json");
    if (endpoint.scheme() != "https" && endpoint.scheme() != "file")
        result.error = "Catalog URL must use HTTPS.";
    else {
        QNetworkAccessManager manager;
        QNetworkRequest request(endpoint);
        request.setRawHeader("User-Agent", "Novelty/1");
        request.setRawHeader("Accept", "application/vnd.github+json");
        auto reply = manager.get(request);
        QEventLoop loop;
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(qBound(100, timeoutMs, 30000)); loop.exec();
        if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
            try {
                result.releases = parseGeCatalog(reply->read(2 * 1024 * 1024 + 1));
                if (result.releases.isEmpty()) result.error = "The service returned no supported GE builds.";
                else if (endpoint.host() == "api.github.com") cacheGeCatalog(libraryRoot, result.releases);
            } catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
        } else result.error = reply->isFinished() ? reply->errorString() : "GE release service timed out.";
        reply->abort(); reply->deleteLater();
    }
    if (!result.releases.isEmpty()) return result;
    try {
        const auto cached = readJson(cachePath);
        if (cached["schema"] == 1 && cached["releases"].isArray()) {
            result.releases = parseGeCatalog(QJsonDocument(cached["releases"].toArray()).toJson());
            result.fromCache = !result.releases.isEmpty();
        }
    } catch (...) { /* Preserve the original service error. */ }
    return result;
}
QList<ManagedRunner> managedGeRunners(const Library &lib) {
    QList<ManagedRunner> entries;
    for (const auto &info : QDir(lib.dataRoot() + "/runners").entryInfoList(
             QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (!validVersion(info.fileName())) continue;
        qint64 size = 0;
        try { size = readJson(info.filePath() + "/novelty-install.json")["size"].toInteger(); }
        catch (...) { /* Listed as needing repair. */ }
        entries.append({info.fileName(), info.filePath(), verified(info.filePath(), info.fileName()), size});
    }
    return entries;
}
void syncGeProvisioningPolicy(const Library &lib, const QString &selectedRunner) {
    QLockFile mutation(lib.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress. Retry saving Settings shortly.");
    const auto policyPath = lib.dataRoot() + "/ge-proton.json";
    auto policy = readJson(policyPath);
    QString version;
    for (const auto &entry : managedGeRunners(lib)) {
        if (selectedRunner != entry.path) continue;
        if (!entry.verified) fail("The selected managed GE build needs repair before it can be the library default.");
        version = entry.version;
        break;
    }
    if (version.isEmpty() && policy.isEmpty()) return;
    if (version.isEmpty()) {
        policy.remove("version");
        policy.remove("sha512");
    } else {
        if (policy["version"] != version) policy.remove("sha512");
        policy["schema"] = 1;
        policy["version"] = version;
        policy["architecture"] = "x86_64";
        if (!policy.contains("auto_download")) policy["auto_download"] = true;
    }
    writeJson(policyPath, policy);
}
QString managedRunnerRemovalIssue(const Library &lib, const QString &version) { return issue(lib, version, true); }
void removeManagedRunner(Library &lib, const QString &version) {
    if (!validVersion(version)) fail("Choose an exact GE-Proton version.");
    QLockFile mutation(lib.dataRoot() + "/mutation.lock"); mutation.setStaleLockTime(0);
    if (!mutation.tryLock()) fail("Library maintenance is in progress.");
    if (const auto reason = issue(lib, version, true); !reason.isEmpty()) fail(reason);
    std::vector<std::unique_ptr<QLockFile>> locks;
    for (const auto &game : lib.games()) {
        auto lock = std::make_unique<QLockFile>(lib.gamePath(game.id, "launch.lock"));
        lock->setStaleLockTime(0);
        if (!QDir().mkpath(QFileInfo(lock->fileName()).absolutePath()) || !lock->tryLock()) fail("A game started while checking this runner. Retry after it exits.");
        locks.push_back(std::move(lock));
    }
    if (const auto reason = issue(lib, version, false); !reason.isEmpty()) fail(reason);
    const auto lockPath = lib.dataRoot() + "/runners/." + version + ".install.lock";
    const auto encoded = QFile::encodeName(lockPath);
    const int fd = ::open(encoded.constData(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) fail("Cannot lock GE installer state.");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); fail("This GE build is being installed."); }
    const bool removed = QDir(pathFor(lib, version)).removeRecursively();
    ::flock(fd, LOCK_UN); ::close(fd);
    if (!removed) fail("Could not remove the managed GE build completely; inspect its folder.");
}
}
