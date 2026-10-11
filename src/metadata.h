#pragma once
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qurl.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <functional>

namespace novelty {

// Stored data is deliberately small; the cover is a separate bounded cache file.
QJsonObject parseVnMetadata(const QJsonObject &vn);
bool validVndbId(const QString &id);
QString bestVndbMatch(const QString &query, const QJsonArray &hits);
QStringList parsePublishers(const QJsonObject &releases);
QString metadataCoverPath(const QString &dataRoot, const QString &gameId);
bool saveMetadataCover(const QString &path, const QByteArray &bytes);

class VndbClient : public QObject {
public:
    explicit VndbClient(QObject *parent, QUrl base = QUrl("https://api.vndb.org/kana/"));
    void search(const QString &query, std::function<void(QJsonArray, QString)> done);
    void fetch(const QString &id, std::function<void(QJsonObject, QByteArray, QString)> done);
private:
    void post(const QString &path, const QJsonObject &body, std::function<void(QJsonObject, QString)> done);
    void publisherPage(const QString &id, int page, QStringList collected,
                       std::function<void(QStringList, QString)> done);
    void cover(const QJsonObject &metadata, std::function<void(QJsonObject, QByteArray, QString)> done);
    QUrl base_;
    QNetworkAccessManager network_;
};
} // namespace novelty
