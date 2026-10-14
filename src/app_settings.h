#pragma once

#include <QtCore/qjsonobject.h>
#include <QtCore/qstring.h>

namespace novelty {
QString appSettingsPath();
QJsonObject loadAppSettings();
void saveAppSettings(const QJsonObject &settings);
void setAppSetting(const QString &key, const QJsonValue &value);
}
