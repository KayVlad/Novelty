#include "app_settings.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qsavefile.h>
#include <stdexcept>

namespace novelty {
QString appSettingsPath() {
    // Tests use an isolated directory; the application always uses its own folder.
    const auto test = QCoreApplication::applicationName().endsWith("Tests")
        ? qEnvironmentVariable("NOVELTY_TEST_APP_DATA_DIR") : QString();
    const auto base = test.isEmpty() ? QCoreApplication::applicationDirPath() + "/data" : test;
    return base + "/settings.json";
}
QJsonObject loadAppSettings() {
    const auto path = appSettingsPath();
    if (!QFileInfo::exists(path)) return {};
    QFile file(path);
    if (QFileInfo(path).isSymbolicLink() || !file.open(QIODevice::ReadOnly) || file.size() > 16384)
        throw std::runtime_error("Cannot read Novelty application settings.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.object()["schema"].toInt() != 1)
        throw std::runtime_error("Novelty application settings JSON is invalid.");
    return document.object();
}
void saveAppSettings(const QJsonObject &settings) {
    const auto path = appSettingsPath();
    const auto directory = QFileInfo(path).absolutePath();
    if (QFileInfo(directory).isSymbolicLink() || !QDir().mkpath(directory) || QFileInfo(path).isSymbolicLink())
        throw std::runtime_error("Cannot create Novelty application settings folder.");
    QJsonObject object{{"schema", 1}};
    if (settings["gamesRoot"].isString()) object["gamesRoot"] = settings["gamesRoot"];
    if (settings["libraryView"].isString()) object["libraryView"] = settings["libraryView"];
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save Novelty application settings beside its executable.");
}
void setAppSetting(const QString &key, const QJsonValue &value) {
    auto settings = loadAppSettings();
    settings[key] = value;
    saveAppSettings(settings);
}
}
