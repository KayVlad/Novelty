#include "core.h"
#include "filesystem_checks.h"
#include "filesystem_durability.h"
#include "save_bundle_archive.h"
#include "save_server_store.h"
#include <QtCore/qcommandlineparser.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qmap.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtimer.h>
#include <QtCore/qthread.h>
#include <functional>
#include <QtNetwork/qhostaddress.h>
#include <QtNetwork/qtcpserver.h>
#include <QtNetwork/qtcpsocket.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

using namespace novelty;
namespace {
constexpr qint64 maxArchive = 512LL * 1024 * 1024 + 1024 * 1024 + 12;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QByteArray tokenFor(const QString &root) {
    const auto path = QFileInfo(root).absoluteFilePath() + "/access.token";
    const QFileInfo info(path);
    if (info.isSymbolicLink()) fail("Server access token cannot be a symbolic link.");
    if (!info.exists()) {
        unsigned char random[32];
        if (RAND_bytes(random, sizeof(random)) != 1) fail("Cannot generate server access token.");
        const auto value = QByteArray(reinterpret_cast<const char *>(random), sizeof(random))
                               .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const auto encoded = QFile::encodeName(path);
        const int descriptor = ::open(encoded.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (descriptor < 0) fail("Cannot create private server access token.");
        const auto written = ::write(descriptor, value.constData(), value.size());
        const bool durable = written == value.size() && ::fsync(descriptor) == 0;
        ::close(descriptor);
        if (!durable) { QFile::remove(path); fail("Cannot store server access token."); }
        persistParent(path);
        return value;
    }
    if (!info.isFile() || (info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
         QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)))
        fail("Server access token permissions must allow only its owner.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read server access token.");
    const auto value = file.readAll().trimmed();
    if (value.size() != 43) fail("Server access token is invalid.");
    return value;
}
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
// At most two heavyweight jobs, even if clients disconnect during verification.
// Worker ownership is independent of Request; disconnect never joins on the event loop.
int activeWork = 0;
class ServerWork final : public QThread {
public:
    std::function<ServerSaveRevision()> action;
    ServerSaveRevision result;
    std::exception_ptr error;
    void run() override { try { result = action(); } catch (...) { error = std::current_exception(); } }
};
class Request final : public QObject {
public:
    Request(QTcpSocket *socket, std::shared_ptr<SaveServerStore> store, const QByteArray &token)
        : socket_(socket), store_(store), token_(token),
          stage_(std::make_shared<QTemporaryDir>(store->rootPath() + "/.request-XXXXXX")) {
        socket_->setParent(this);
        socket_->setReadBufferSize(128 * 1024);
        timeout_.setSingleShot(true);
        timeout_.setInterval(30000);
        connect(&timeout_, &QTimer::timeout, this, [this] {
            if (answered_) socket_->abort(); else reply(408, "Request Timeout", {});
        });
        connect(socket_, &QTcpSocket::readyRead, this, [this] { consume(); });
        connect(socket_, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
            if (bytes > 0) timeout_.start(30000);
            sendDownload();
        });
        connect(socket_, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        timeout_.start();
        deadline_.setSingleShot(true);
        connect(&deadline_, &QTimer::timeout, this, [this] { socket_->abort(); });
        deadline_.start(10000); // Header deadline is absolute, even if bytes keep arriving.
        if (!stage_->isValid()) reply(500, "Internal Server Error", {});
        else try {
            writeJson(stage_->path() + "/request-owner.json", {{"schema", 1},
                {"server_id", store_->serverId()}, {"directory", QFileInfo(stage_->path()).fileName()}});
        } catch (const std::exception &) { reply(500, "Internal Server Error", {}); }
    }
private:
    void reply(int status, const QByteArray &label, const QByteArray &body,
               const QByteArray &type = "application/json", bool head = false,
               const QByteArray &extraHeaders = {}) {
        if (answered_) return;
        answered_ = true;
        timeout_.stop();
        deadline_.stop();
        QByteArray message = "HTTP/1.1 " + QByteArray::number(status) + ' ' + label + "\r\n";
        message += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
        message += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        message += extraHeaders + "\r\n";
        socket_->write(message);
        if (!head) socket_->write(body);
        socket_->disconnectFromHost();
    }
    void error(int status, const QByteArray &label, const QByteArray &code) {
        reply(status, label, json({{"error", QString::fromLatin1(code)}}));
    }
    void consume() {
        if (answered_) { socket_->readAll(); return; }
        timeout_.start();
        if (!headersReady_) {
            while (socket_->bytesAvailable() && header_.size() <= 16384) {
                header_ += socket_->read(std::min<qint64>(socket_->bytesAvailable(), 16385 - header_.size()));
                const auto end = header_.indexOf("\r\n\r\n");
                if (end >= 0) {
                    const auto surplus = header_.mid(end + 4);
                    header_.truncate(end);
                    headersReady_ = true;
                    if (!parseHeader()) return;
                    if (!surplus.isEmpty()) acceptBody(surplus);
                    break;
                }
            }
            if (!headersReady_ && header_.size() > 16384) { error(431, "Request Header Fields Too Large", "headers_too_large"); return; }
        }
        if (headersReady_ && upload_.isOpen() && !answered_) {
            while (socket_->bytesAvailable() && remaining_ > 0) {
                const auto part = socket_->read(std::min<qint64>(remaining_, 1024 * 1024));
                acceptBody(part);
                if (answered_) return;
            }
            if (remaining_ == 0) finishUpload();
        }
    }
    bool parseHeader() {
        const auto lines = header_.split('\n');
        if (lines.isEmpty()) { error(400, "Bad Request", "bad_request"); return false; }
        const auto first = lines.first().trimmed().split(' ');
        if (first.size() != 3 || first[2] != "HTTP/1.1" ||
            (first[0] != "GET" && first[0] != "HEAD" && first[0] != "PUT")) {
            error(400, "Bad Request", "bad_request"); return false;
        }
        const auto method = first[0], target = first[1];
        QMap<QByteArray, QByteArray> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const auto line = lines[i].trimmed();
            const auto colon = line.indexOf(':');
            if (colon <= 0 || line.contains('\0')) { error(400, "Bad Request", "bad_header"); return false; }
            const auto name = line.left(colon).toLower();
            if (headers.contains(name)) { error(400, "Bad Request", "duplicate_header"); return false; }
            headers.insert(name, line.mid(colon + 1).trimmed());
        }
        const auto authorization = headers.value("authorization");
        const auto presented = authorization.mid(7);
        if (!authorization.startsWith("Bearer ") || presented.size() != token_.size() ||
            CRYPTO_memcmp(presented.constData(), token_.constData(), token_.size()) != 0) {
            error(401, "Unauthorized", "unauthorized"); return false;
        }
        deadline_.start(600000); // Bound authenticated transfers as well as idle time.
        if (headers.contains("transfer-encoding") || headers.contains("expect")) {
            error(400, "Bad Request", "unsupported_encoding"); return false;
        }
        if (target == "/v1/identity" && method == "GET") {
            reply(200, "OK", json({{"schema", 1}, {"server_id", store_->serverId()}})); return false;
        }
        constexpr char prefix[] = "/v1/games/";
        const auto encoded = target.mid(sizeof(prefix) - 1);
        if (!target.startsWith(prefix) || encoded.isEmpty() || encoded.contains('/') ||
            encoded.contains('?') || encoded.contains('#')) {
            error(404, "Not Found", "not_found"); return false;
        }
        const auto decoded = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding |
                                                      QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.isEmpty() || decoded.size() > 256 ||
            decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != encoded) {
            error(400, "Bad Request", "bad_game_key"); return false;
        }
        gameKey_ = QString::fromUtf8(decoded);
        if (gameKey_.toUtf8() != decoded) { error(400, "Bad Request", "bad_game_key"); return false; }
        try {
            if (method == "GET" || method == "HEAD") {
                const auto current = store_->current(gameKey_, false);
                if (method == "HEAD") {
                    QByteArray fields = "X-Novelty-Server-Id: " + current.serverId.toLatin1() + "\r\n";
                    fields += "X-Novelty-Revision: " + QByteArray::number(current.revision) + "\r\n";
                    fields += "X-Novelty-Snapshot-Id: " + current.snapshotId.toLatin1() + "\r\n";
                    fields += "X-Novelty-Files: " + QByteArray::number(current.files) + "\r\n";
                    reply(200, "OK", {}, "application/json", true, fields);
                    return false;
                }
                if (current.revision == 0) {
                    QByteArray fields = "X-Novelty-Server-Id: " + current.serverId.toLatin1() + "\r\n";
                    fields += "X-Novelty-Revision: 0\r\nX-Novelty-Snapshot-Id: \r\nX-Novelty-Files: 0\r\n";
                    reply(200, "OK", json({{"schema", 1}, {"server_id", current.serverId},
                                           {"revision", 0}, {"snapshot_id", ""}, {"files", 0}}),
                          "application/json", false, fields);
                    return false;
                }
                const auto stage = stage_;
                startWork([stage, current] {
                    writePortableSaveArchive(current.bundleDirectory, stage->path() + "/download.nvsb");
                    return current;
                }, [this](const ServerSaveRevision &revision) { beginDownload(revision); });
                return false;
            }
            if (!headers.contains("content-length") || !headers.contains("x-expected-revision")) {
                error(400, "Bad Request", "missing_upload_header"); return false;
            }
            bool sizeOk = false, revisionOk = false;
            remaining_ = headers.value("content-length").toLongLong(&sizeOk);
            expected_ = headers.value("x-expected-revision").toInt(&revisionOk);
            if (!sizeOk || remaining_ < 12 || remaining_ > maxArchive || !revisionOk || expected_ < 0) {
                error(413, "Content Too Large", "bad_upload_size_or_revision"); return false;
            }
            if (headers.contains("x-novelty-reconcile")) {
                if (headers.value("x-novelty-reconcile") != store_->serverId().toLatin1()) {
                    error(409, "Conflict", "server_identity_changed"); return false;
                }
                reconcile_ = true;
            }
            requireAvailableSpace(store_->rootPath(), remaining_ * 3);
            upload_.setFileName(stage_->path() + "/upload.nvsb");
            if (!upload_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("Cannot stage server upload.");
            return true;
        } catch (const std::exception &) {
            error(500, "Internal Server Error", "storage_error"); return false;
        }
    }
    void acceptBody(const QByteArray &data) {
        if (!upload_.isOpen()) return;
        if (data.size() > remaining_) {
            error(400, "Bad Request", "bad_upload_body"); return;
        }
        qsizetype written = 0;
        while (written < data.size()) {
            const auto count = upload_.write(data.constData() + written, data.size() - written);
            if (count <= 0) { error(500, "Internal Server Error", "upload_storage_error"); return; }
            written += count;
        }
        remaining_ -= data.size();
    }
    void startWork(std::function<ServerSaveRevision()> action,
                   std::function<void(const ServerSaveRevision &)> done) {
        if (activeWork >= 2) { error(503, "Service Unavailable", "storage_busy"); return; }
        ++activeWork;
        auto *work = new ServerWork;
        work->action = std::move(action);
        connect(work, &QThread::finished, this, [this, work, done = std::move(done)] {
            if (answered_) return;
            try {
                if (work->error) std::rethrow_exception(work->error);
                done(work->result);
            } catch (const ServerRevisionConflict &conflict) {
                reply(409, "Conflict", json({{"error", "stale_revision"}, {"current_revision", conflict.currentRevision}}));
            } catch (const std::exception &) { error(422, "Unprocessable Content", "invalid_save_bundle"); }
        });
        connect(work, &QThread::finished, work, [work] { --activeWork; work->deleteLater(); });
        work->start();
    }
    void finishUpload() {
        upload_.close();
        const auto stage = stage_; const auto store = store_;
        const auto key = gameKey_; const auto expected = expected_; const auto reconcile = reconcile_;
        startWork([stage, store, key, expected, reconcile] {
            const auto destination = stage->path() + "/extracted";
            extractPortableSaveArchive(stage->path() + "/upload.nvsb", destination);
            return store->put(key, expected, destination, reconcile);
        }, [this](const ServerSaveRevision &current) {
            reply(200, "OK", json({{"schema", 1}, {"server_id", current.serverId},
                {"revision", current.revision}, {"snapshot_id", current.snapshotId}, {"files", current.files}}));
        });
    }
    void beginDownload(const ServerSaveRevision &current) {
        download_.setFileName(stage_->path() + "/download.nvsb");
        if (!download_.open(QIODevice::ReadOnly)) fail("Cannot open server download.");
        answered_ = true; timeout_.start(30000);
        QByteArray message = "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\n";
        message += "Content-Type: application/vnd.novelty.save-bundle-v1\r\n";
        message += "Content-Length: " + QByteArray::number(download_.size()) + "\r\n";
        message += "X-Novelty-Server-Id: " + current.serverId.toLatin1() + "\r\n";
        message += "X-Novelty-Revision: " + QByteArray::number(current.revision) + "\r\n";
        message += "X-Novelty-Snapshot-Id: " + current.snapshotId.toLatin1() + "\r\n";
        message += "X-Novelty-Files: " + QByteArray::number(current.files) + "\r\n\r\n";
        socket_->write(message); sendDownload();
    }
    void sendDownload() {
        if (!download_.isOpen()) return;
        while (socket_->bytesToWrite() < 1024 * 1024 && !download_.atEnd()) {
            const auto data = download_.read(1024 * 1024);
            if (data.isEmpty() || socket_->write(data) != data.size()) { socket_->abort(); return; }
        }
        if (download_.atEnd()) { download_.close(); socket_->disconnectFromHost(); }
    }
    QTcpSocket *socket_;
    std::shared_ptr<SaveServerStore> store_;
    QByteArray token_, header_;
    std::shared_ptr<QTemporaryDir> stage_;
    QTimer timeout_, deadline_;
    QFile upload_, download_;
    QString gameKey_;
    qint64 remaining_ = 0;
    int expected_ = 0;
    bool headersReady_ = false, answered_ = false, reconcile_ = false;
};
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("novelty-save-server");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"data", "Private server storage directory.", "path"});
    parser.addOption({"port", "Loopback TCP port (default 42428).", "port", "42428"});
    parser.process(app);
    try {
        if (!parser.isSet("data")) fail("Pass --data with a private storage directory.");
        bool okay = false;
        const auto port = parser.value("port").toUShort(&okay);
        if (!okay || port == 0) fail("Server port must be between 1 and 65535.");
        auto store = std::make_shared<SaveServerStore>(parser.value("data"));
        QLockFile service(store->rootPath() + "/service.lock"); service.setStaleLockTime(0);
        if (!service.tryLock()) fail("A save server is already using this storage directory.");
        store->recover();
        for (const auto &entry : checkedDirectoryEntries(store->rootPath(), 0)) {
            if (!QRegularExpression("^\\.request-[A-Za-z0-9]{6}$").match(entry.fileName()).hasMatch() ||
                entry.isSymbolicLink() || !entry.isDir()) continue;
            const auto marker = entry.absoluteFilePath() + "/request-owner.json";
            if (QFileInfo(marker).isSymbolicLink() || !QFileInfo(marker).isFile() || QFileInfo(marker).size() > 4096) continue;
            QJsonObject owner;
            try { owner = readJson(marker); } catch (const std::exception &) { continue; }
            if (owner["schema"].toInt() != 1 || owner["server_id"] != store->serverId() || owner["directory"] != entry.fileName()) continue;
            // Revision journals never reference a request directory. Store recovery ran first.
            if (!QDir(entry.absoluteFilePath()).removeRecursively()) fail("Cannot clean abandoned server request stage.");
            persistParent(entry.absoluteFilePath());
        }
        const auto token = tokenFor(parser.value("data"));
        int activeRequests = 0;
        QTcpServer listener;
        listener.setMaxPendingConnections(64);
        listener.setListenBacklogSize(128); // Accept a full request burst plus overflow before enforcing the active cap.
        if (!listener.listen(QHostAddress::LocalHost, port)) fail("Cannot bind local save server port.");
        QObject::connect(&listener, &QTcpServer::newConnection, &app, [&] {
            while (auto *socket = listener.nextPendingConnection()) {
                if (activeRequests >= 64) { socket->abort(); socket->deleteLater(); continue; }
                auto *request = new Request(socket, store, token);
                request->setParent(&listener);
                ++activeRequests;
                QObject::connect(request, &QObject::destroyed, &listener, [&] { --activeRequests; });
            }
        });
        return app.exec();
    } catch (const std::exception &error) {
        fprintf(stderr, "novelty-save-server: %s\n", error.what());
        return 1;
    }
}
