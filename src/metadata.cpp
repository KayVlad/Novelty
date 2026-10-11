#include "metadata.h"
#include <QtCore/qbuffer.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <QtGui/qimagereader.h>
#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>
#include <algorithm>

namespace novelty {
namespace {
const QRegularExpression vnIdPattern("^v[1-9][0-9]*$");
QStringList names(const QJsonArray &items) {
    QSet<QString> found;
    for (const auto &value : items) {
        const auto name = value.toObject()["name"].toString().trimmed();
        if (!name.isEmpty()) found.insert(name);
    }
    auto result = found.values(); result.sort(Qt::CaseInsensitive); return result;
}
QJsonArray asArray(const QStringList &list) {
    QJsonArray array; for (const auto &item : list) array.append(item); return array;
}
QString plainDescription(QString source) {
    source.remove('\r');
    // VNDB descriptions use BBCode. Keep plain text in the launcher, never render remote markup.
    static const QRegularExpression markup("\\[/?[a-z][a-z0-9]*(?:=[^\\]]*)?\\]", QRegularExpression::CaseInsensitiveOption);
    source.remove(markup);
    return source.trimmed().left(12000);
}
QString expectedTime(const QJsonObject &vn) {
    const int minutes = vn["length_minutes"].toInt();
    if (minutes > 0) {
        const auto hours = double(minutes) / 60.0;
        return hours < 10 ? QString("About %1 hours").arg(hours, 0, 'f', 1) : QString("About %1 hours").arg(qRound(hours));
    }
    const QStringList lengths{"", "Very short", "Short", "Medium", "Long", "Very long"};
    return lengths.value(vn["length"].toInt());
}
QString normalizedTitle(const QString &title) {
    QString result;
    for (const auto ch : title.normalized(QString::NormalizationForm_KC).toCaseFolded())
        if (ch.isLetterOrNumber()) result.append(ch);
    return result;
}
}

QJsonObject parseVnMetadata(const QJsonObject &vn) {
    const auto id = vn["id"].toString(), title = vn["title"].toString().trimmed();
    if (!vnIdPattern.match(id).hasMatch() || title.isEmpty()) return {};
    QString original;
    for (const auto &value : vn["titles"].toArray()) {
        const auto titleRow = value.toObject();
        if (titleRow["main"].toBool()) { original = titleRow["title"].toString().trimmed(); break; }
    }
    struct Tag { QString name; double rating; };
    QList<Tag> tags;
    for (const auto &value : vn["tags"].toArray()) {
        const auto tag = value.toObject();
        if (tag["category"] != "cont" || tag["spoiler"].toInt() != 0) continue;
        const auto name = tag["name"].toString().trimmed();
        if (!name.isEmpty()) tags.append({name, tag["rating"].toDouble()});
    }
    std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) {
        return a.rating == b.rating ? a.name.compare(b.name, Qt::CaseInsensitive) < 0 : a.rating > b.rating;
    });
    QStringList contentTags; for (const auto &tag : tags) if (!contentTags.contains(tag.name)) contentTags.append(tag.name);
    return {{"schema", 1}, {"id", id}, {"title", title}, {"original_title", original},
            {"description", plainDescription(vn["description"].toString())},
            {"expected_play_time", expectedTime(vn)}, {"developers", asArray(names(vn["developers"].toArray()))},
            {"publishers", QJsonArray()}, {"tags", asArray(contentTags)},
            {"thumbnail_url", vn["image"].toObject()["thumbnail"].toString()}};
}
bool validVndbId(const QString &id) { return vnIdPattern.match(id).hasMatch(); }
QString bestVndbMatch(const QString &query, const QJsonArray &hits) {
    const auto wanted = normalizedTitle(query);
    QString first;
    for (const auto &value : hits) {
        const auto hit = value.toObject();
        const auto id = hit["id"].toString();
        if (!validVndbId(id) || hit["title"].toString().trimmed().isEmpty()) continue;
        if (first.isEmpty()) first = id; // VNDB searchrank order is the fallback.
        if (!wanted.isEmpty() && (normalizedTitle(hit["title"].toString()) == wanted ||
                                  normalizedTitle(hit["alttitle"].toString()) == wanted)) return id;
    }
    return first;
}
QStringList parsePublishers(const QJsonObject &releases) {
    QSet<QString> found;
    for (const auto &value : releases["results"].toArray())
        for (const auto &producerValue : value.toObject()["producers"].toArray()) {
            const auto producer = producerValue.toObject();
            const auto name = producer["name"].toString().trimmed();
            if (producer["publisher"].toBool() && !name.isEmpty()) found.insert(name);
        }
    auto result = found.values(); result.sort(Qt::CaseInsensitive); return result;
}
QString metadataCoverPath(const QString &dataRoot, const QString &gameId) {
    return dataRoot + "/installations/" + gameId + "/cover.png";
}
bool saveMetadataCover(const QString &path, const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > 5 * 1024 * 1024) return false;
    QBuffer buffer; buffer.setData(bytes); if (!buffer.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
    const auto size = reader.size();
    if (size.isValid() && (size.width() > 4096 || size.height() > 4096)) return false;
    auto image = reader.read();
    if (image.isNull()) return false;
    if (image.width() > 400 || image.height() > 560) image = image.scaled(400, 560, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
}

VndbClient::VndbClient(QObject *parent, QUrl base) : QObject(parent), base_(std::move(base)), network_(this) {}
void VndbClient::post(const QString &path, const QJsonObject &body, std::function<void(QJsonObject, QString)> done) {
    QNetworkRequest request(base_.resolved(QUrl(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", "Novelty/0.1 metadata client");
    request.setTransferTimeout(15000);
    auto reply = network_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 bytes, qint64) { if (bytes > 2 * 1024 * 1024) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        QString error;
        if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
        const auto bytes = reply->readAll(); reply->deleteLater();
        if (!error.isEmpty()) { done({}, "VNDB request failed: " + error); return; }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (!document.isObject() || parseError.error != QJsonParseError::NoError) { done({}, "VNDB returned invalid data."); return; }
        done(document.object(), {});
    });
}
void VndbClient::search(const QString &query, std::function<void(QJsonArray, QString)> done) {
    const auto text = query.trimmed();
    if (text.isEmpty()) { done({}, "Enter a title to search."); return; }
    post("vn", {{"filters", QJsonArray{"search", "=", text}}, {"fields", "id,title,alttitle"},
                {"sort", "searchrank"}, {"results", 20}},
         [done = std::move(done)](const QJsonObject &response, const QString &error) {
             if (!error.isEmpty()) { done({}, error); return; }
             QJsonArray hits;
             for (const auto &item : response["results"].toArray()) {
                 const auto hit = item.toObject();
                 if (vnIdPattern.match(hit["id"].toString()).hasMatch() && !hit["title"].toString().isEmpty()) hits.append(hit);
             }
             done(hits, {});
         });
}
void VndbClient::publisherPage(const QString &id, int page, QStringList collected,
                               std::function<void(QStringList, QString)> done) {
    post("release", {{"filters", QJsonArray{"vn", "=", QJsonArray{"id", "=", id}}},
                     {"fields", "producers{name,publisher}"}, {"results", 100}, {"page", page}},
         [=, this](const QJsonObject &response, const QString &error) mutable {
             if (!error.isEmpty()) { done(collected, error); return; }
             for (const auto &name : parsePublishers(response)) if (!collected.contains(name)) collected.append(name);
             if (response["more"].toBool()) {
                 if (page >= 30) { done(collected, "VNDB has too many release pages to collect all publishers in one request."); return; }
                 publisherPage(id, page + 1, collected, done);
             } else { collected.sort(Qt::CaseInsensitive); done(collected, {}); }
         });
}
void VndbClient::cover(const QJsonObject &metadata, std::function<void(QJsonObject, QByteArray, QString)> done) {
    const QUrl url(metadata["thumbnail_url"].toString());
    if (!url.isValid() || url.isEmpty()) { done(metadata, {}, {}); return; }
    const bool official = url.scheme() == "https" && url.host().endsWith(".vndb.org");
    const bool localTest = base_.host() == "127.0.0.1" && url.host() == base_.host();
    if (!official && !localTest) { done(metadata, {}, "VNDB supplied an unexpected cover URL."); return; }
    QNetworkRequest request(url); request.setTransferTimeout(15000);
    auto reply = network_.get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 bytes, qint64) { if (bytes > 5 * 1024 * 1024) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [reply, metadata, done = std::move(done)] {
        const auto error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const auto bytes = reply->readAll(); reply->deleteLater();
        done(metadata, error.isEmpty() ? bytes : QByteArray(), error.isEmpty() ? QString() : "Cover unavailable: " + error);
    });
}
void VndbClient::fetch(const QString &id, std::function<void(QJsonObject, QByteArray, QString)> done) {
    if (!vnIdPattern.match(id).hasMatch()) { done({}, {}, "Choose a valid VNDB entry."); return; }
    post("vn", {{"filters", QJsonArray{"id", "=", id}},
                {"fields", "id,title,titles{title,main},description,length_minutes,length,developers{name},tags{name,category,spoiler,rating},image{thumbnail}"},
                {"results", 1}},
         [=, this](const QJsonObject &response, const QString &error) {
             if (!error.isEmpty()) { done({}, {}, error); return; }
             const auto results = response["results"].toArray();
             const auto metadata = results.isEmpty() ? QJsonObject() : parseVnMetadata(results.first().toObject());
             if (metadata.isEmpty() || metadata["id"] != id) { done({}, {}, "VNDB entry was not found."); return; }
             publisherPage(id, 1, {}, [=, this](QStringList publishers, const QString &publisherError) {
                 auto result = metadata; result["publishers"] = asArray(publishers);
                 cover(result, [=](const QJsonObject &data, const QByteArray &image, const QString &coverError) {
                     QStringList warnings; if (!publisherError.isEmpty()) warnings.append(publisherError);
                     if (!coverError.isEmpty()) warnings.append(coverError);
                     done(data, image, warnings.join('\n'));
                 });
             });
         });
}
} // namespace novelty
