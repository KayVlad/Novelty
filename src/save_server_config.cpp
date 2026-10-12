#include "save_server_config.h"
#include "save_server_client.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qsavefile.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString directory() { return QCoreApplication::applicationDirPath() + "/data/server-profiles"; }
QString configPath(const Library &library) {
    const auto id = library.profileId();
    if (QUuid(id).isNull()) fail("Library profile identity is invalid.");
    return directory() + '/' + QString::fromLatin1(QCryptographicHash::hash(
        id.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
}
void noLinks(const QString &path) {
    auto current = QString("/");
    for (const auto &part : QDir::cleanPath(QFileInfo(path).absoluteFilePath()).split('/', Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymbolicLink()) fail("Save server settings path is a symbolic link.");
    }
}
}

SaveServerConfig loadSaveServerConfig(const Library &library) {
    const auto path = configPath(library);
    noLinks(path);
    if (!QFileInfo::exists(path)) return {};
    const QFileInfo info(path);
    if (!info.isFile() || info.size() < 1 || info.size() > 8192 ||
        (info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                               QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)))
        fail("Save server settings are not private or are invalid.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read save server settings.");
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    SaveServerConfig result;
    result.enabled = object["enabled"].toBool();
    result.url = QUrl(object["url"].toString());
    result.token = object["token"].toString().toLatin1();
    result.serverId = object["server_id"].toString();
    if (object["schema"].toInt() != 1 || object["profile_id"].toString() != library.profileId() ||
        (result.enabled && (QUuid(result.serverId).isNull() || result.token.isEmpty())))
        fail("Save server settings are invalid.");
    if (result.enabled) SaveServerClient client(result.url, result.token);
    return result;
}
void saveSaveServerConfig(const Library &library, const SaveServerConfig &config) {
    if (config.enabled) {
        if (QUuid(config.serverId).isNull()) fail("Confirm the save server identity before enabling sync.");
        SaveServerClient client(config.url, config.token);
    }
    const auto path = configPath(library);
    noLinks(path);
    if (!QDir().mkpath(directory()) || !QFile::setPermissions(directory(),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        fail("Cannot create private save server settings directory.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail("Cannot write save server settings.");
    const QJsonObject object{{"schema", 1}, {"profile_id", library.profileId()},
                             {"enabled", config.enabled}, {"url", config.url.toString()},
                             {"token", QString::fromLatin1(config.token)}, {"server_id", config.serverId}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > 8192 || file.write(bytes) != bytes.size() || !file.commit() ||
        !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        fail("Cannot publish private save server settings.");
}

} // namespace novelty
