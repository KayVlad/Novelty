#pragma once
#include <QtCore/qstring.h>
#include <QtCore/qjsonobject.h>

namespace novelty {
// Linux commit barriers. Errors must leave the transaction's recovery record intact.
void persistTree(const QString &path);
void persistParent(const QString &path);
void writeDurableJson(const QString &path, const QJsonObject &value);
void removeDurableFile(const QString &path);
}
