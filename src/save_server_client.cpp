#include "save_server_client.h"
#include "save_bundle_archive.h"
#include "saves.h"
#include <QtCore/qdir.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtimer.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>
#include <memory>
#include <climits>
#include <stdexcept>

namespace novelty {
namespace {
constexpr qint64 maxArchive = 512LL * 1024 * 1024 + 1024 * 1024 + 12;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool validKey(const QString &key) {
    if (key.isEmpty() || key.toUtf8().size() > 256) return false;
    for (const auto c : key) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
QNetworkRequest requestFor(const QUrl &url, const QByteArray &token, int timeoutMs) {
    if (timeoutMs < 1 || timeoutMs > 600000) fail("Save server timeout is invalid.");
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + token);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(timeoutMs);
    return request;
}
int status(const QNetworkReply &reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}
void await(QNetworkReply &reply, int timeoutMs, QFile *download = nullptr) {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool writeFailed = false;
    qint64 received = 0;
    const auto drain = [&] {
        while (reply.bytesAvailable()) {
            const auto data = reply.read(1024 * 1024);
            if (data.isEmpty()) break;
            received += data.size();
            if (received > maxArchive) { writeFailed = true; reply.abort(); return; }
            if (download) {
                qsizetype written = 0;
                while (written < data.size()) {
                    const auto count = download->write(data.constData() + written, data.size() - written);
                    if (count <= 0) { writeFailed = true; reply.abort(); return; }
                    written += count;
                }
            }
        }
    };
    if (download) QObject::connect(&reply, &QIODevice::readyRead, &loop, drain);
    else QObject::connect(&reply, &QIODevice::readyRead, &loop, [&] {
        if (reply.bytesAvailable() > 65536) { writeFailed = true; reply.abort(); }
    });
    QObject::connect(&reply, &QNetworkReply::finished, &loop, [&] { if (download) drain(); loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { reply.abort(); loop.quit(); });
    timeout.start(timeoutMs);
    if (!reply.isFinished()) loop.exec();
    else if (download) drain();
    if (writeFailed) fail("Save server download exceeded its limit or could not be staged.");
    if (!timeout.isActive()) fail("Save server request timed out.");
    if (status(reply) < 200 || status(reply) >= 300) {
        if (status(reply) == 409) {
            const auto object = QJsonDocument::fromJson(reply.readAll()).object();
            throw RemoteRevisionConflict(object["current_revision"].toInt(-1));
        }
        fail("Save server rejected the request (HTTP " + QString::number(status(reply)) + ").");
    }
    if (reply.error() != QNetworkReply::NoError)
        fail("Save server is unavailable: " + reply.errorString());
}
RemoteSaveRevision headers(const QNetworkReply &reply) {
    const auto serverId = QString::fromLatin1(reply.rawHeader("X-Novelty-Server-Id"));
    const auto snapshotId = QString::fromLatin1(reply.rawHeader("X-Novelty-Snapshot-Id"));
    bool okay = false;
    const auto revision = reply.rawHeader("X-Novelty-Revision").toInt(&okay);
    bool filesOkay = false;
    const auto files = reply.rawHeader("X-Novelty-Files").toInt(&filesOkay);
    if (QUuid(serverId).isNull() || !okay || revision < 0 ||
        !filesOkay || files < 0 || (revision > 0 && QUuid(snapshotId).isNull()) ||
        (revision == 0 && (!snapshotId.isEmpty() || files != 0)))
        fail("Save server returned invalid revision headers.");
    return {serverId, revision, snapshotId, files};
}
RemoteSaveRevision response(const QByteArray &bytes) {
    if (bytes.size() > 65536) fail("Save server response is too large.");
    const auto object = QJsonDocument::fromJson(bytes).object();
    const auto serverId = object["server_id"].toString(), snapshotId = object["snapshot_id"].toString();
    const auto revision = object["revision"].toInt(-1);
    const auto files = object["files"].toInt(-1);
    if (object["schema"].toInt() != 1 || QUuid(serverId).isNull() || revision < 0 ||
        files < 0 || (revision > 0 && QUuid(snapshotId).isNull()) ||
        (revision == 0 && (!snapshotId.isEmpty() || files != 0)))
        fail("Save server returned invalid revision metadata.");
    return {serverId, revision, snapshotId, files};
}
}

RemoteRevisionConflict::RemoteRevisionConflict(int revision)
    : std::runtime_error("Save server revision changed; local progress was preserved."), currentRevision(revision) {}

SaveServerClient::SaveServerClient(const QUrl &baseUrl, const QByteArray &bearerToken)
    : base_(baseUrl), token_(bearerToken) {
    const auto scheme = base_.scheme().toLower(), host = base_.host().toLower();
    const bool local = host == "localhost" || host == "127.0.0.1" || host == "::1";
    if (!base_.isValid() || host.isEmpty() ||
        !(scheme == "https" || (scheme == "http" && local)) ||
        !base_.userName().isEmpty() || !base_.password().isEmpty() ||
        !base_.query().isEmpty() || !base_.fragment().isEmpty() ||
        (base_.path() != "" && base_.path() != "/") ||
        token_.isEmpty() || token_.size() > 1024 || token_.contains('\n') || token_.contains('\r'))
        fail("Save server address or access token is invalid. Use HTTPS, or HTTP on this device only.");
}
QUrl SaveServerClient::endpoint(const QString &gameKey) const {
    if (!validKey(gameKey)) fail("Server game key is invalid.");
    auto url = base_;
    const auto encoded = gameKey.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                                  QByteArray::OmitTrailingEquals);
    url.setPath("/v1/games/" + QString::fromLatin1(encoded));
    return url;
}
QString SaveServerClient::identity(int timeoutMs) const {
    auto url = base_;
    url.setPath("/v1/identity");
    QNetworkAccessManager manager;
    auto request = requestFor(url, token_, timeoutMs);
    std::unique_ptr<QNetworkReply, void(*)(QNetworkReply *)> reply(manager.get(request), [](QNetworkReply *p) { p->deleteLater(); });
    await(*reply, timeoutMs);
    const auto bytes = reply->readAll();
    if (bytes.size() > 65536) fail("Save server identity response is too large.");
    const auto object = QJsonDocument::fromJson(bytes).object();
    const auto id = object["server_id"].toString();
    if (object["schema"].toInt() != 1 || QUuid(id).isNull())
        fail("Save server identity response is invalid.");
    return id;
}
RemoteSaveRevision SaveServerClient::current(const QString &gameKey, int timeoutMs) const {
    QNetworkAccessManager manager;
    auto request = requestFor(endpoint(gameKey), token_, timeoutMs);
    std::unique_ptr<QNetworkReply, void(*)(QNetworkReply *)> reply(manager.head(request), [](QNetworkReply *p) { p->deleteLater(); });
    await(*reply, timeoutMs);
    return headers(*reply);
}
RemoteSaveRevision SaveServerClient::download(const QString &gameKey, const QString &destinationDirectory,
                                              int timeoutMs) const {
    if (QFileInfo(destinationDirectory).exists()) fail("Save download destination already exists.");
    QTemporaryDir temporary(QFileInfo(destinationDirectory).absolutePath() + "/.server-download-XXXXXX");
    if (!temporary.isValid()) fail("Cannot stage save server download.");
    QFile archive(temporary.path() + "/received.nvsb");
    if (!archive.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("Cannot stage save server download.");
    QNetworkAccessManager manager;
    auto request = requestFor(endpoint(gameKey), token_, timeoutMs);
    std::unique_ptr<QNetworkReply, void(*)(QNetworkReply *)> reply(manager.get(request), [](QNetworkReply *p) { p->deleteLater(); });
    await(*reply, timeoutMs, &archive);
    if (!archive.flush()) fail("Cannot finish save server download.");
    archive.close();
    const auto metadata = headers(*reply);
    if (metadata.revision == 0) fail("Save server has no state to download.");
    extractPortableSaveArchive(archive.fileName(), destinationDirectory);
    const auto extracted = verifyPortableSaveBundle(destinationDirectory);
    if (extracted["id"].toString() != metadata.snapshotId ||
        extracted["files"].toInt(-1) != metadata.files) {
        QDir(destinationDirectory).removeRecursively();
        fail("Save server bundle does not match the advertised snapshot.");
    }
    return metadata;
}
RemoteSaveRevision SaveServerClient::upload(const QString &gameKey, int expectedRevision,
                                            const QString &bundleDirectory, int timeoutMs) const {
    if (expectedRevision < 0 || expectedRevision == INT_MAX) fail("Expected server revision is invalid.");
    QTemporaryDir temporary(QFileInfo(bundleDirectory).absolutePath() + "/.server-upload-XXXXXX");
    if (!temporary.isValid()) fail("Cannot stage save server upload.");
    const auto path = temporary.path() + "/upload.nvsb";
    writePortableSaveArchive(bundleDirectory, path);
    QFile archive(path);
    if (!archive.open(QIODevice::ReadOnly)) fail("Cannot read staged save server upload.");
    QNetworkAccessManager manager;
    auto request = requestFor(endpoint(gameKey), token_, timeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/vnd.novelty.save-bundle-v1");
    request.setRawHeader("X-Expected-Revision", QByteArray::number(expectedRevision));
    request.setHeader(QNetworkRequest::ContentLengthHeader, archive.size());
    std::unique_ptr<QNetworkReply, void(*)(QNetworkReply *)> reply(manager.put(request, &archive), [](QNetworkReply *p) { p->deleteLater(); });
    await(*reply, timeoutMs);
    const auto accepted = response(reply->readAll());
    const auto source = verifyPortableSaveBundle(bundleDirectory);
    if (accepted.revision != expectedRevision + 1 || accepted.snapshotId != source["id"].toString() ||
        accepted.files != source["files"].toInt(-1))
        fail("Save server acknowledgement does not match the uploaded state.");
    return accepted;
}

} // namespace novelty
