#pragma once

#include <QtCore/qjsonobject.h>
#include <QtCore/qstring.h>

namespace novelty {
QString appDataDirectory();
QString appSettingsPath();
QJsonObject defaultAppSettings();
QJsonObject defaultLibrarySettings();
QJsonObject loadAppSettings();
void saveAppSettings(const QJsonObject &settings);
void setAppSetting(const QString &key, const QJsonValue &value);
QJsonObject loadLibrarySettings(const QString &libraryRoot);
void saveLibrarySettings(const QString &libraryRoot, const QJsonObject &settings);
QJsonObject loadLocalSettings();
void setLocalSetting(const QString &key, const QJsonValue &value);
}
