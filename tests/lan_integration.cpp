#include "core.h"
#include "devices.h"
#include "lan_connection.h"
#include "lan_saves.h"
#include "lan_sync.h"
#include "lan_games.h"
#include "saves.h"
#include "pairing.h"
#include "peer_tls.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qdir.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtimer.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qthread.h>
#include <QtCore/qjsondocument.h>
#include <QtSql/qsqlquery.h>
#include <QtSql/qsqlerror.h>
#include <QtNetwork/qhostaddress.h>
#include <cstdio>
#include <stdexcept>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

using namespace novelty;

static void codePairingTests(const QString &root) {
    QDir().mkpath(root + "/host"); QDir().mkpath(root + "/guest");
    Library host(root + "/host"), guest(root + "/guest");
    const auto hostId = ensureDeviceIdentity(host), guestId = ensureDeviceIdentity(guest);
    for (int scenario = 0; scenario < 6; ++scenario) {
        QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QString hostCode, guestCode, error;
        bool remoteApprovalWithoutTrust = false;
        bool hostReady = false, guestReady = false, submitted = false;
        LanListener listener(host, true);
        if (!listener.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("Cannot listen for code pairing.");
        LanConnection client(guest, true); client.usePairingCode("Bazzite desk");
        const auto submit = [&] {
            if (hostCode.isEmpty() || guestCode.isEmpty() || submitted) return;
            submitted = true;
            if (hostCode != guestCode || hostCode.size() != 6 || !trustedPeers(host).isEmpty() || !trustedPeers(guest).isEmpty())
                throw std::runtime_error("Code pairing trusted a device before verification.");
            const auto wrong = hostCode == "000000" ? "000001" : "000000";
            if (client.submitPairingCode(wrong)) throw std::runtime_error("Wrong pairing code was accepted.");
            if (scenario == 1) { client.submitPairingCode(wrong); client.submitPairingCode(wrong); }
            else if (!client.submitPairingCode(hostCode)) throw std::runtime_error("Matching code was rejected.");
        };
        listener.onConnection = [&](LanConnection *connection) {
            connection->usePairingCode("Steam Deck");
            connection->onPhrase = [&, connection](const QString &, const QString &code) {
                if (connection->peerName() != "Bazzite desk") throw std::runtime_error("Pairing lost the device name.");
                hostCode = code; submit();
            };
            connection->onRemoteApproval = [&, connection] {
                if (!trustedPeers(host).isEmpty() || !trustedPeers(guest).isEmpty())
                    throw std::runtime_error("Displaying device must confirm before trust is saved.");
                if (scenario == 5) {
                    remoteApprovalWithoutTrust = true; loop.quit(); return;
                }
                connection->approvePairing(connection->peerName());
            };
            connection->onReady = [&](const QString &) { hostReady = true; if (guestReady) loop.quit(); };
            connection->onError = [&](const QString &message) { error = message; loop.quit(); };
            return true;
        };
        client.onPhrase = [&](const QString &, const QString &code) { guestCode = code; submit(); };
        client.onReady = [&](const QString &) { guestReady = true; if (hostReady) loop.quit(); };
        client.onError = [&](const QString &message) { error = message; loop.quit(); };
        QSslSocket malformed;
        if (scenario >= 2 && scenario != 5) {
            configureDeviceTls(malformed, guestId);
            QObject::connect(&malformed, &QSslSocket::sslErrors, &loop, [&](const QList<QSslError> &errors) {
                acceptExpectedPeerTlsErrors(malformed, errors);
            });
            QObject::connect(&malformed, &QSslSocket::encrypted, &loop, [&] {
                if (scenario == 4) {
                    malformed.write(encodeControlFrame(QJsonDocument(QJsonObject{{"type", "pair-approve"}}).toJson()));
                    return;
                }
                const auto commitment = QCryptographicHash::hash(QByteArray(32, 'a'), QCryptographicHash::Sha256).toHex();
                malformed.write(encodeControlFrame(QJsonDocument(QJsonObject{{"type", "pair-hello"},
                    {"nonce", QString::fromLatin1(commitment)}, {"verification", "code-v1"}, {"name", scenario == 3 ? QString("Bazzite\u202e desk") : QString("Bazzite desk")}}).toJson()));
                malformed.write(encodeControlFrame(QJsonDocument(QJsonObject{{"type", "pair-code-nonce"},
                    {"nonce", QString::fromLatin1(QByteArray(32, 'b').toHex())}}).toJson()));
            });
            malformed.connectToHostEncrypted("127.0.0.1", listener.serverPort(), "novelty.local");
        } else client.connectTo("127.0.0.1", listener.serverPort());
        timeout.start(3000); loop.exec(); timeout.stop(); client.close(); malformed.abort();
        if (scenario == 0) {
            if (!error.isEmpty() || !hostReady || !guestReady || trustedPeers(host).first().name != "Bazzite desk" ||
                trustedPeers(guest).first().name != "Steam Deck") throw std::runtime_error(("Code pairing failed: " + error).toStdString());
            revokePeer(host, guestId.fingerprint); revokePeer(guest, hostId.fingerprint);
        } else if (scenario == 5) {
            if (!remoteApprovalWithoutTrust || hostReady || guestReady || !trustedPeers(host).isEmpty() || !trustedPeers(guest).isEmpty())
                throw std::runtime_error("Remote approval or cancellation bypassed local human confirmation.");
        } else if (error.isEmpty() || hostReady || guestReady || !trustedPeers(host).isEmpty() || !trustedPeers(guest).isEmpty())
            throw std::runtime_error("Invalid code or nonce commitment was not rejected safely.");
        if (scenario == 2 && !error.contains("changed during")) throw std::runtime_error("Nonce commitment was not checked.");
    }
    std::puts("PASS: six-digit pairing, device names, wrong-code limit, two-sided approval and nonce commitment verification");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        if (!temp.isValid()) throw std::runtime_error("Cannot create LAN fixture libraries.");
        if (!QDir().mkpath(temp.path() + "/deck") || !QDir().mkpath(temp.path() + "/desktop"))
            throw std::runtime_error("Cannot create LAN fixture Games folders.");
        QDir().mkpath(temp.path() + "/deck/Game/saves");
        QDir().mkpath(temp.path() + "/desktop/Game");
        QFile deckExe(temp.path() + "/deck/Game/game.exe"), desktopExe(temp.path() + "/desktop/Game/game.exe");
        if (!deckExe.open(QIODevice::WriteOnly) || !desktopExe.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot create LAN fixture executables.");
        deckExe.close(); desktopExe.close();
        Library deck(temp.path() + "/deck"), desktop(temp.path() + "/desktop");
        deck.import({inspect(temp.path() + "/deck/Game")}, {});
        desktop.import({inspect(temp.path() + "/desktop/Game")}, {});
        const auto deckGame = deck.games().first(), desktopGame = desktop.games().first();
        auto sourceConfig = loadSaveConfig(deck, deckGame.id);
        sourceConfig.mappings = {{"game", "saves"}};
        saveSaveConfig(deck, deckGame.id, sourceConfig);
        auto targetConfig = loadSaveConfig(desktop, desktopGame.id);
        targetConfig.mappings = sourceConfig.mappings;
        saveSaveConfig(desktop, desktopGame.id, targetConfig);
        QFile save(deckGame.folder + "/saves/slot1.sav");
        if (!save.open(QIODevice::WriteOnly) || save.write("LAN progress") != 12)
            throw std::runtime_error("Cannot write LAN fixture save.");
        save.close();
        const auto sourceSnapshot = createSaveSnapshot(deck, deckGame.id);
        LanListener listener(deck, true);
        if (!listener.listen(QHostAddress::LocalHost, 0)) {
            std::puts("SKIP: localhost TCP binding is unavailable here");
            return 77;
        }
        {
            QDir().mkpath(temp.path() + "/invalid-identity"); Library invalid(temp.path() + "/invalid-identity");
            const auto identity = ensureDeviceIdentity(invalid);
            QFile key(identity.privateKeyPath); if (!key.open(QIODevice::WriteOnly)) throw std::runtime_error("identity fault setup");
            key.write("invalid key"); key.close();
            int descriptors[2]; if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors)) throw std::runtime_error("descriptor fixture");
            LanConnection rejected(invalid, true); rejected.acceptDescriptor(descriptors[0]);
            if (::fcntl(descriptors[0], F_GETFD) != -1 || errno != EBADF) throw std::runtime_error("Failed TLS configuration leaked accepted descriptor.");
            ::close(descriptors[1]);
        }
        codePairingTests(temp.path() + "/code-pairing");
        QString firstPhrase, secondPhrase, error;
        bool serverReady = false, clientReady = false, serverEndpoint = false, clientEndpoint = false;
        QEventLoop pairing;
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &pairing, &QEventLoop::quit);
        auto checkReady = [&] { if (serverReady && clientReady && serverEndpoint && clientEndpoint) pairing.quit(); };
        listener.onConnection = [&](LanConnection *connection) {
            connection->setServicePort(41231);
            connection->onEndpoint = [&] { serverEndpoint = true; checkReady(); };
            connection->onPhrase = [&, connection](const QString &, const QString &phrase) {
                firstPhrase = phrase; connection->approvePairing("Desktop");
            };
            connection->onReady = [&](const QString &) { serverReady = true; checkReady(); };
            connection->onError = [&](const QString &message) { error = message; pairing.quit(); };
            return true;
        };
        LanConnection client(desktop, true);
        client.setServicePort(41232);
        client.onEndpoint = [&] { clientEndpoint = true; checkReady(); };
        client.onPhrase = [&](const QString &, const QString &phrase) {
            secondPhrase = phrase; client.approvePairing("Deck");
        };
        client.onReady = [&](const QString &) { clientReady = true; checkReady(); };
        client.onError = [&](const QString &message) { error = message; pairing.quit(); };
        client.connectTo("127.0.0.1", listener.serverPort());
        timeout.start(15000); pairing.exec(); timeout.stop();
        if (!error.isEmpty() || !serverReady || !clientReady || firstPhrase.isEmpty() || firstPhrase != secondPhrase ||
            !isTrustedPeer(deck, ensureDeviceIdentity(desktop).fingerprint) ||
            !isTrustedPeer(desktop, ensureDeviceIdentity(deck).fingerprint))
            throw std::runtime_error(("Pairing failed: " + error).toStdString());
        if (!serverEndpoint || !clientEndpoint || trustedPeers(deck).first().port != 41232 ||
            trustedPeers(desktop).first().port != 41231 || trustedPeers(deck).first().address != "127.0.0.1")
            throw std::runtime_error("Pairing did not remember the actual transfer listeners on both devices.");
        std::puts("PASS: two-sided TLS pairing, matching phrase and automatic transfer endpoints");
        client.close(); listener.close();

        {
            QEventLoop admission;
            LanListener bounded(deck, false);
            if (!bounded.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("Cannot listen for admission fixture.");
            int accepted = 0;
            bounded.onConnection = [&](LanConnection *) { ++accepted; return true; };
            QList<QTcpSocket *> idle;
            for (int i = 0; i < 12; ++i) {
                auto socket = new QTcpSocket(&admission); idle.append(socket);
                socket->connectToHost(QHostAddress::LocalHost, bounded.serverPort());
            }
            QTimer::singleShot(200, &admission, &QEventLoop::quit); admission.exec();
            for (auto socket : idle) socket->abort();
            if (accepted != 4) throw std::runtime_error("Unauthenticated sockets bypassed the listener's connection bound.");
        }
        std::puts("PASS: unauthenticated connection admission is bounded");

        {
            QEventLoop burst;
            LanListener burstListener(deck, false);
            if (!burstListener.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("Cannot listen for framing fixture.");
            int received = 0;
            QString burstError;
            burstListener.onConnection = [&](LanConnection *connection) {
                connection->onReady = [connection](const QString &) {
                    QByteArray frames;
                    for (int i = 0; i < 12; ++i) frames += encodeControlFrame(QJsonDocument(QJsonObject{
                        {"type", "audit-frame"}, {"index", i}, {"data", QString(60000, 'x')}}).toJson(QJsonDocument::Compact));
                    auto socket = connection->findChild<QSslSocket *>();
                    socket->write(frames.left(12000)); // Leave an incomplete frame in the application buffer.
                    QTimer::singleShot(30, connection, [socket, tail = frames.mid(12000)] { socket->write(tail); });
                };
                return true;
            };
            LanConnection burstClient(desktop, false);
            auto burstSocket = burstClient.findChild<QSslSocket *>();
            bool heldRead = false, coalesced = false;
            QObject::connect(burstSocket, &QSslSocket::readyRead, &burst, [&] {
                if (heldRead || received) return;
                heldRead = true; burstSocket->blockSignals(true);
                QTimer::singleShot(150, &burst, [&] {
                    coalesced = burstSocket->bytesAvailable() >= 128 * 1024;
                    burstSocket->blockSignals(false);
                    QMetaObject::invokeMethod(burstSocket, "readyRead", Qt::DirectConnection);
                });
            });
            burstClient.onError = [&](const QString &reason) { burstError = reason; burst.quit(); };
            burstClient.onMessage = [&](const QJsonObject &message) {
                if (message["index"].toInt(-1) != received || message["data"].toString().size() != 60000) {
                    burstError = "Framed burst changed order or contents."; burst.quit(); return;
                }
                if (++received == 12) burst.quit();
            };
            QTimer burstTimeout; burstTimeout.setSingleShot(true);
            QObject::connect(&burstTimeout, &QTimer::timeout, &burst, &QEventLoop::quit);
            burstClient.connectTo("127.0.0.1", burstListener.serverPort());
            burstTimeout.start(3000); burst.exec(); burstTimeout.stop(); burstClient.close();
            if (!coalesced || received != 12 || !burstError.isEmpty()) throw std::runtime_error(("Framed TLS burst failed: " + burstError).toStdString());
        }
        std::puts("PASS: large coalesced TLS frame burst");

        LanListener trustedListener(deck, false);
        if (!trustedListener.listen(QHostAddress::LocalHost, 0))
            throw std::runtime_error("Cannot listen for trusted save transfer.");
        QEventLoop transfer;
        QTimer transferTimeout; transferTimeout.setSingleShot(true);
        QObject::connect(&transferTimeout, &QTimer::timeout, &transfer, &QEventLoop::quit);
        bool imported = false;
        QString transferError;
        trustedListener.onConnection = [&](LanConnection *connection) {
            connection->onReady = [&, connection](const QString &) {
                auto router = new LanSaveExchange(deck, *connection, connection);
                new LanGameExchange(deck, *connection, *router, connection);
            };
            connection->onError = [&, connection](const QString &message) {
                transferError = message; transfer.quit(); connection->deleteLater();
            };
            return true;
        };
        LanConnection trustedClient(desktop, false);
        trustedClient.onError = [&](const QString &message) { transferError = message; transfer.quit(); };
        trustedClient.onReady = [&](const QString &fingerprint) {
            auto exchange = new LanSaveExchange(desktop, trustedClient, &trustedClient);
            exchange->onError = [&](const QString &message) { transferError = message; transfer.quit(); };
            exchange->onMatches = [&, exchange, fingerprint](const QJsonArray &matches) {
                if (matches.size() != 1) { transferError = "Expected one compatible game."; transfer.quit(); return; }
                const auto item = matches.first().toObject();
                try {
                    linkPeerSaveGame(desktop, desktopGame.id, fingerprint, item["profile_game_id"].toString());
                    exchange->fetch(desktopGame.id, item["profile_game_id"].toString(), item["snapshot_id"].toString());
                } catch (const std::exception &e) { transferError = QString::fromUtf8(e.what()); transfer.quit(); }
            };
            exchange->onImported = [&](const SaveImportResult &result) {
                if (!result.added || result.relation != "available" || result.snapshotId != sourceSnapshot.id) {
                    transferError = "Imported snapshot has the wrong ancestry."; transfer.quit(); return;
                }
                try {
                    restoreSaveSnapshot(desktop, desktopGame.id, result.snapshotId);
                    QFile restored(desktopGame.folder + "/saves/slot1.sav");
                    if (!restored.open(QIODevice::ReadOnly) || restored.readAll() != "LAN progress")
                        throw std::runtime_error("Transferred save did not restore.");
                    imported = true; transfer.quit();
                } catch (const std::exception &e) { transferError = QString::fromUtf8(e.what()); transfer.quit(); }
            };
            try { exchange->locate(loadSaveConfig(desktop, desktopGame.id)); }
            catch (const std::exception &e) { transferError = QString::fromUtf8(e.what()); transfer.quit(); }
        };
        trustedClient.connectTo("127.0.0.1", trustedListener.serverPort());
        transferTimeout.start(15000); transfer.exec(); transferTimeout.stop();
        if (!transferError.isEmpty() || !imported)
            throw std::runtime_error(("Save transfer failed: " + transferError).toStdString());
        std::puts("PASS: authenticated snapshot transfer, import and fresh-install restore");
        QFile newer(deckGame.folder + "/saves/slot1.sav");
        if (!newer.open(QIODevice::WriteOnly | QIODevice::Truncate) || newer.write("new LAN progress") != 16)
            throw std::runtime_error("Cannot write newer LAN fixture save.");
        newer.close();
        createSaveSnapshot(deck, deckGame.id);
        QFile newest(deckGame.folder + "/saves/slot1.sav");
        if (!newest.open(QIODevice::WriteOnly | QIODevice::Truncate) || newest.write("newest LAN progress") != 19)
            throw std::runtime_error("Cannot write latest LAN fixture save.");
        newest.close();
        const auto nextSnapshot = createSaveSnapshot(deck, deckGame.id);
        rememberPeerEndpoint(desktop, ensureDeviceIdentity(deck).fingerprint,
                             "127.0.0.1", trustedListener.serverPort());
        const auto refresh = refreshLinkedPeerSaves(desktop, desktopGame.id);
        const auto candidates = importedSaveCandidates(desktop, desktopGame.id);
        if (!refresh.issues.isEmpty() || refresh.snapshotsImported != 2 || candidates.size() != 1 ||
            candidates.first().snapshotId != nextSnapshot.id || candidates.first().relation != "ahead")
            throw std::runtime_error("Automatic before-Play peer check did not find the newer save.");
        restoreSaveSnapshot(desktop, desktopGame.id, nextSnapshot.id);
        std::puts("PASS: remembered peer endpoint supplies an ahead save before Play");

        QDir().mkpath(temp.path() + "/new-device");
        Library newDevice(temp.path() + "/new-device");
        approvePairedPeer(newDevice, ensureDeviceIdentity(deck).fingerprint, "Deck");
        approvePairedPeer(deck, ensureDeviceIdentity(newDevice).fingerprint, "New device");
        QEventLoop gameTransfer;
        QTimer gameTimeout; gameTimeout.setSingleShot(true);
        QObject::connect(&gameTimeout, &QTimer::timeout, &gameTransfer, &QEventLoop::quit);
        QString gameError, installedId;
        bool senderProtected = false;
        const auto registrationSql = [&](const QString &statement) {
            const auto connection = QStringLiteral("lan-registration-fault");
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(newDevice.dataRoot() + "/library.sqlite");
                if (!db.open()) throw std::runtime_error("Cannot open LAN registration fault fixture.");
                QSqlQuery query(db);
                if (!query.exec(statement)) throw std::runtime_error(query.lastError().text().toStdString());
            }
            QSqlDatabase::removeDatabase(connection);
        };
        const auto runGameTransfer = [&] {
        gameError.clear(); installedId.clear();
        QElapsedTimer wait; wait.start();
        while (deck.supervisorActive(deckGame.id) && wait.elapsed() < 2000) {
            QCoreApplication::processEvents(); QThread::msleep(5);
        }
        LanConnection gameClient(newDevice, false);
        gameClient.onError = [&](const QString &reason) { gameError = reason; gameTransfer.quit(); };
        gameClient.onReady = [&](const QString &) {
            auto router = new LanSaveExchange(newDevice, gameClient, &gameClient);
            auto exchange = new LanGameExchange(newDevice, gameClient, *router, &gameClient);
            exchange->onError = [&](const QString &reason) { gameError = reason; gameTransfer.quit(); };
            exchange->onCatalogue = [&, exchange](const QJsonArray &games) {
                try {
                    if (games.size() != 1) throw std::runtime_error("Expected one peer game.");
                    exchange->install(games.first().toObject());
                } catch (const std::exception &e) { gameError = QString::fromUtf8(e.what()); gameTransfer.quit(); }
            };
            exchange->onInstalled = [&](const QString &id) {
                installedId = id;
                auto changed = loadSaveConfig(deck, deckGame.id); changed.mappings = {{"game", "ChangedDuringTransfer"}};
                try { saveSaveConfig(deck, deckGame.id, changed); }
                catch (const std::exception &) { senderProtected = true; }
                gameTransfer.quit();
            };
            try { exchange->catalogue(); }
            catch (const std::exception &e) { gameError = QString::fromUtf8(e.what()); gameTransfer.quit(); }
        };
        gameClient.connectTo("127.0.0.1", trustedListener.serverPort());
        gameTimeout.start(15000); gameTransfer.exec(); gameTimeout.stop();
        gameClient.close();
        };
        registrationSql("CREATE TRIGGER fail_game_update BEFORE UPDATE ON games BEGIN SELECT RAISE(ABORT,'injected registration failure'); END");
        runGameTransfer();
        if (gameError.isEmpty() || !installedId.isEmpty() ||
            !QFileInfo::exists(newDevice.root() + "/Game/.novelty-transfer.json") || !discover(newDevice.root()).isEmpty())
            throw std::runtime_error("Failed LAN registration did not retain its retry marker and hide incomplete discovery.");
        registrationSql("DROP TRIGGER fail_game_update");
        runGameTransfer();
        if (!gameError.isEmpty() || installedId.isEmpty() ||
            !QFileInfo(newDevice.game(installedId).executable).isFile() ||
            newDevice.game(installedId).folder != newDevice.root() + "/Game")
            throw std::runtime_error(("LAN game install failed: " + gameError).toStdString());
        if (!senderProtected || newDevice.games().size() != 1 || QFileInfo::exists(newDevice.root() + "/Game/.novelty-transfer.json") ||
            QFileInfo::exists(newDevice.game(installedId).folder + "/saves/slot1.sav"))
            throw std::runtime_error("Retry duplicated the game or transferred saves with installation files.");
        QElapsedTimer unlockWait; unlockWait.start();
        while (deck.supervisorActive(deckGame.id) && unlockWait.elapsed() < 2000) {
            QCoreApplication::processEvents(); QThread::msleep(5);
        }
        QLockFile unlocked(deck.gamePath(deckGame.id, "launch.lock")); unlocked.setStaleLockTime(0);
        if (!unlocked.tryLock()) throw std::runtime_error("Successful/disconnected game transfer retained the source launch lock.");
        unlocked.unlock();
        rememberPeerEndpoint(newDevice, ensureDeviceIdentity(deck).fingerprint,
                             "127.0.0.1", trustedListener.serverPort());
        const auto gameSaves = refreshLinkedPeerSaves(newDevice, installedId);
        if (!gameSaves.issues.isEmpty() || gameSaves.snapshotsImported != 3 ||
            importedSaveCandidates(newDevice, installedId).size() != 1)
            throw std::runtime_error("New LAN game did not reconcile its source save history.");
        restoreSaveSnapshot(newDevice, installedId, nextSnapshot.id);
        std::puts("PASS: trusted LAN game install and saved-progress reconciliation");
        // These callbacks capture local event loops, so destroy the listener's connections first.
        trustedClient.close(); trustedListener.close();
        const auto trustedConnections = trustedListener.children();
        for (auto child : trustedConnections)
            if (auto connection = dynamic_cast<LanConnection *>(child)) delete connection;
        const auto pairingConnections = listener.children();
        for (auto child : pairingConnections)
            if (auto connection = dynamic_cast<LanConnection *>(child)) delete connection;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
    return 0;
}
