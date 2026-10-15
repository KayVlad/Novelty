#include "core.h"
#include "saves.h"
#include "save_server_sync.h"
#include "save_server_state.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qprocess.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qthread.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qtcpserver.h>
#include <QtNetwork/qtcpsocket.h>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace novelty;
namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void put(const QString &path, const QByteArray &bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture mkdir");
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(), "fixture write");
}
QByteArray get(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "fixture read"); return file.readAll();
}
struct Fixture {
    QTemporaryDir temp;
    Library library;
    Game game;
    Fixture() : library(temp.path()) {
        require(temp.isValid(), "fixture root");
        put(temp.path() + "/Game/play.sh", "#!/bin/sh\ntouch played\n");
        QFile::setPermissions(temp.path() + "/Game/play.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        library.import({inspect(temp.path() + "/Game")}, {});
        game = library.games().first();
        auto saves = loadSaveConfig(library, game.id); saves.mappings = {{"game", "save"}};
        saveSaveConfig(library, game.id, saves);
        put(game.folder + "/save/slot", "baseline");
    }
    QString bundle(const QString &id) const {
        return QFileInfo(library.gamePath(game.id, "saves.json")).absolutePath() + "/snapshots/" + id;
    }
};
struct Server {
    QTemporaryDir temp;
    QProcess process;
    QString executable;
    quint16 port;
    SaveServerConfig config;
    explicit Server(const QString &binary) : executable(binary) {
        QTcpServer reservation; require(reservation.listen(QHostAddress::LocalHost), "reserve port");
        port = reservation.serverPort(); reservation.close();
        start();
        config.enabled = true; config.url = QUrl("http://127.0.0.1:" + QString::number(port));
        QElapsedTimer clock; clock.start();
        while (!QFileInfo::exists(temp.path() + "/access.token") && clock.elapsed() < 3000) QThread::msleep(10);
        config.token = get(temp.path() + "/access.token").trimmed();
        config.serverId = SaveServerClient(config.url, config.token).identity();
    }
    void start() {
        process.start(executable, {"--data", temp.path(), "--port", QString::number(port)});
        require(process.waitForStarted(), "start server");
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 3000) {
            QTcpSocket probe; probe.connectToHost(QHostAddress::LocalHost, port);
            if (probe.waitForConnected(50)) { probe.abort(); return; }
            QThread::msleep(10);
        }
        throw std::runtime_error("server did not listen");
    }
    void stop() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); } }
    ~Server() { stop(); }
};
void retryAndConflict(const QString &binary) {
    Server server(binary); Fixture f;
    const auto &config = server.config;
    const auto key = defaultServerGameKey(f.library, f.game.id);
    SaveServerClient client(config.url, config.token);
    const auto initial = retryPendingServerSave(f.library, f.game.id, config);
    if (initial.status != ServerSyncStatus::Synced) throw std::runtime_error(("initial sync: " + initial.detail).toStdString());
    require(client.current(key).revision == 1, "initial retry publishes verified saves");
    put(f.game.folder + "/save/slot", "after play");
    queuePostExitSaveSnapshot(f.library, f.game);
    f.library.recordStatus(f.game.id, "Sync pending");
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Synced &&
            pendingSaveSnapshotIssue(f.library, f.game.id).isEmpty() && client.current(key).revision == 2,
            "retry finishes pending capture while owning mutation.lock and uploads it");
    put(f.game.folder + "/save/slot", "remote change");
    const auto remote = createSaveSnapshot(f.library, f.game.id);
    client.upload(key, 2, f.bundle(remote.id));
    put(f.game.folder + "/save/slot", "after play");
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Synced &&
            get(f.game.folder + "/save/slot") == "remote change", "unchanged local saves adopt a newer server state");
    put(f.game.folder + "/save/slot", "next remote");
    const auto next = createSaveSnapshot(f.library, f.game.id);
    client.upload(key, 3, f.bundle(next.id));
    put(f.game.folder + "/save/slot", "local change");
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Conflict &&
            f.library.status(f.game) == "Save conflict", "idle conflict remains visible to Manage");
    const auto preview = reviewServerSaveConflict(f.library, f.game.id, config);
    resolveServerSaveConflict(f.library, f.game.id, config, preview, true);
    require(f.library.status(f.game) == "Ready" && client.current(key).revision == 5,
            "explicit conflict resolution publishes the chosen local state");
    auto changed = loadSaveConfig(f.library, f.game.id);
    changed.mappings = {{"game", "different-save"}}; changed.gameIdentity = "new-edition";
    saveSaveConfig(f.library, f.game.id, changed);
    put(f.game.folder + "/different-save/slot", "new mapping progress");
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Conflict,
            "mapping and identity changes require explicit reconciliation");
    const auto changedPreview = reviewServerSaveConflict(f.library, f.game.id, config);
    require(!changedPreview.remoteCompatible && !changedPreview.remoteIssue.isEmpty(),
            "incompatible remote action has a clear explanation before user confirmation");
    resolveServerSaveConflict(f.library, f.game.id, config, changedPreview, true);
    require(client.current(key).revision == 6 && get(f.game.folder + "/different-save/slot") == "new mapping progress" &&
            get(f.game.folder + "/save/slot") == "local change", "explicit local choice reconciles identity and mappings without deleting either local location");
    auto changedIdentity = config; changedIdentity.serverId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(syncServerBeforePlay(f.library, f.game.id, changedIdentity).status == ServerSyncStatus::Conflict,
            "an unrecognized server identity blocks automatic synchronization");
    const auto identityPreview = reviewServerSaveConflict(f.library, f.game.id, changedIdentity);
    resolveServerSaveConflict(f.library, f.game.id, changedIdentity, identityPreview, true);
    require(client.current(key).revision == 7 && loadSaveServerConfig(f.library).serverId == config.serverId,
            "explicit reconciliation rebinds the accepted server identity");
    server.stop();
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Pending &&
            f.library.status(f.game) == "Sync pending" && !f.library.busy(f.game.id),
            "offline sync remains eligible for idle retry");
    server.start();
    require(retryPendingServerSave(f.library, f.game.id, config).status == ServerSyncStatus::Synced &&
            f.library.status(f.game) == "Ready", "idle retry recovers after server restart");
}
void recoveryDuringSync() {
    Fixture f;
    QTcpServer endpoint; require(endpoint.listen(QHostAddress::LocalHost), "local endpoint");
    bool leaveRecovery = true;
    QObject::connect(&endpoint, &QTcpServer::newConnection, &endpoint, [&] {
        auto *socket = endpoint.nextPendingConnection();
        auto header = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, header] {
            *header += socket->readAll();
            if (!header->contains("\r\n\r\n")) return;
            // Fault injection at the network boundary: sync leaves an unfinished
            // local restore after supervise's initial recovery check.
            if (leaveRecovery) put(f.library.gamePath(f.game.id, "save-restore.json"), "{}");
            socket->write("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
        });
    });
    SaveServerConfig config{true, QUrl("http://127.0.0.1:" + QString::number(endpoint.serverPort())),
                            "test-token", QUuid::createUuid().toString(QUuid::WithoutBraces)};
    const auto profilePath = QCoreApplication::applicationDirPath() + "/data/server-profiles/" +
        QString::fromLatin1(QCryptographicHash::hash(f.library.profileId().toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
    struct Cleanup { QString path; ~Cleanup() { QFile::remove(path); } } cleanup{profilePath};
    saveSaveServerConfig(f.library, config);
    require(supervise(f.library, f.game.id, false) != 0 && !QFileInfo::exists(f.game.folder + "/played") &&
            f.library.status(f.game) == "Recovery required", "Play stops when sync leaves local recovery pending");
    QFile::remove(f.library.gamePath(f.game.id, "save-restore.json"));
    leaveRecovery = false;
    require(supervise(f.library, f.game.id, false) == 0 && QFileInfo::exists(f.game.folder + "/played") &&
            f.library.status(f.game) == "Sync pending", "transport failure alone still permits offline Play");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir appData; qputenv("NOVELTY_TEST_APP_DATA_DIR", QFile::encodeName(appData.path()));
    try {
        require(argc == 2, "server executable argument");
        retryAndConflict(QString::fromLocal8Bit(argv[1]));
        recoveryDuringSync();
        std::puts("PASS: server sync lifecycle and recovery barriers"); return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
