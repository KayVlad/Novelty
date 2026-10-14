#include "runner_manager.h"
#include "recipes.h"
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
    if (!QFileInfo(path).isDir() || QFileInfo(path).isSymLink() ||
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
    if (settings["runner"] == path || settings["runner"] == version)
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
        for (const auto &asset : item["assets"].toArray()) {
            const auto file = asset.toObject();
            if (file["name"] == version + ".tar.gz" || file["name"] == version + "-x86_64.tar.gz") {
                size = file["size"].toInteger(); break;
            }
        }
        if (size > 0 && size <= 4LL * 1024 * 1024 * 1024)
            releases.append({version, item["published_at"].toString(), size});
    }
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
                else if (endpoint.host() == "api.github.com") {
                    QJsonArray compact;
                    for (const auto &entry : result.releases)
                        compact.append(QJsonObject{{"tag_name", entry.version}, {"published_at", entry.published},
                            {"assets", QJsonArray{QJsonObject{{"name", entry.version + ".tar.gz"}, {"size", entry.archiveSize}}}}});
                    writeJson(cachePath, {{"schema", 1}, {"fetched", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                          {"releases", compact}});
                }
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
        if (!lock->tryLock()) fail("A game started while checking this runner. Retry after it exits.");
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
