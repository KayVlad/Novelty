#include "app_settings.h"
#include "default_config.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qsavefile.h>
#include <stdexcept>

namespace novelty {
namespace {
constexpr qint64 maxConfigSize = 64 * 1024;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QJsonObject readObject(const QString &path) {
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) return {};
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > maxConfigSize)
        fail("Cannot read settings: " + path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || document.object()["schema"] != 1)
        fail("Invalid settings JSON: " + path);
    return document.object();
}
void directoryFor(const QString &path) {
    const auto directory = QFileInfo(path).absolutePath();
    if (QFileInfo(directory).isSymLink() || !QDir().mkpath(directory) || QFileInfo(path).isSymLink())
        fail("Cannot create settings file: " + path);
}
void writeObject(const QString &path, const QJsonObject &object) {
    directoryFor(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (bytes.size() > maxConfigSize || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail("Cannot save settings: " + path);
}
QJsonObject validated(const QJsonObject &input, const QJsonObject &defaults, const QString &path) {
    auto result = defaults;
    for (auto it = input.begin(); it != input.end(); ++it) {
        // Read old registrations until local-settings.json has been created.
        if (path == appSettingsPath() && it.key() == "externalProtonPaths") continue;
        if (!defaults.contains(it.key()) || it.value().type() != defaults[it.key()].type() ||
            (it.value().isString() && (it.value().toString().contains(QChar::Null) || it.value().toString().size() > 4096)))
            fail("Invalid settings field '" + it.key() + "' in " + path);
        result[it.key()] = it.value();
    }
    if (result["schema"] != 1) fail("Unsupported settings schema: " + path);
    if (result.contains("libraryView") && result["libraryView"] != "grid" && result["libraryView"] != "list")
        fail("libraryView must be grid or list.");
    if (result.contains("gamesRoot") && !result["gamesRoot"].toString().isEmpty() && !QDir::isAbsolutePath(result["gamesRoot"].toString()))
        fail("gamesRoot must be an absolute path.");
    if (result.contains("backend") && result["backend"] != "umu" && result["backend"] != "steam")
        fail("The fallback backend must be umu or steam.");
    if (result.contains("default_recipe") && result["default_recipe"].toString().isEmpty())
        fail("Choose a default recipe.");
    if (result.contains("externalProtonPaths")) {
        const auto paths = result["externalProtonPaths"].toArray();
        if (paths.size() > 64) fail("Register at most 64 external Proton locations.");
        for (const auto &path : paths)
            if (!path.isString() || !QDir::isAbsolutePath(path.toString()) || path.toString().contains(QChar::Null))
                fail("Invalid external Proton registration.");
    }
    return result;
}
QJsonObject loadConfig(const QString &path, const QJsonObject &defaults) {
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return validated(readObject(path), defaults, path);
    directoryFor(path);
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(1000)) fail("Settings are being changed. Retry shortly.");
    if (!QFileInfo::exists(path)) writeObject(path, defaults);
    return validated(readObject(path), defaults, path);
}
void saveConfig(const QString &path, const QJsonObject &settings, const QJsonObject &defaults) {
    const auto object = validated(settings, defaults, path);
    directoryFor(path);
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(1000)) fail("Settings are being changed. Retry shortly.");
    writeObject(path, object);
}
void setConfigValue(const QString &path, const QString &key, const QJsonValue &value, const QJsonObject &defaults) {
    directoryFor(path);
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(1000)) fail("Settings are being changed. Retry shortly.");
    auto object = validated(readObject(path), defaults, path);
    object[key] = value;
    writeObject(path, validated(object, defaults, path));
}
QJsonObject localDefaults() { return {{"schema", 1}, {"externalProtonPaths", QJsonArray{}}}; }
}
QString appDataDirectory() {
    // Child launch supervisors inherit the same isolated data folder in tests.
    const auto test = qEnvironmentVariable("NOVELTY_TEST_APP_DATA_DIR");
    return test.isEmpty() ? QCoreApplication::applicationDirPath() + "/data" : test;
}
QString appSettingsPath() { return appDataDirectory() + "/settings.json"; }
QJsonObject defaultAppSettings() { return QJsonDocument::fromJson(defaults::app).object(); }
QJsonObject loadAppSettings() {
    loadLocalSettings(); // Preserve older registrations before an app preference write normalizes its file.
    return loadConfig(appSettingsPath(), defaultAppSettings());
}
void saveAppSettings(const QJsonObject &settings) { saveConfig(appSettingsPath(), settings, defaultAppSettings()); }
void setAppSetting(const QString &key, const QJsonValue &value) {
    setConfigValue(appSettingsPath(), key, value, defaultAppSettings());
}
QJsonObject defaultLibrarySettings() { return QJsonDocument::fromJson(defaults::library).object(); }
QJsonObject loadLibrarySettings(const QString &libraryRoot) {
    auto result = loadConfig(libraryRoot + "/.novelty/settings.json", defaultLibrarySettings());
    result.remove("schema");
    return result;
}
void saveLibrarySettings(const QString &libraryRoot, const QJsonObject &settings) {
    saveConfig(libraryRoot + "/.novelty/settings.json", settings, defaultLibrarySettings());
}
QJsonObject loadLocalSettings() {
    const auto path = appDataDirectory() + "/local-settings.json";
    if (!QFileInfo::exists(path)) {
        auto initial = localDefaults();
        const auto prior = readObject(appSettingsPath());
        if (prior["externalProtonPaths"].isArray()) initial["externalProtonPaths"] = prior["externalProtonPaths"];
        // Initialize once under the same lock used by subsequent registration edits.
        directoryFor(path); QLockFile lock(path + ".lock");
        if (!lock.tryLock(1000)) fail("Settings are being changed. Retry shortly.");
        if (!QFileInfo::exists(path)) writeObject(path, validated(initial, localDefaults(), path));
    }
    return loadConfig(path, localDefaults());
}
void setLocalSetting(const QString &key, const QJsonValue &value) {
    loadLocalSettings();
    setConfigValue(appDataDirectory() + "/local-settings.json", key, value, localDefaults());
}
}
