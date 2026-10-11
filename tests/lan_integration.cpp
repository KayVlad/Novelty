#include "core.h"
#include "devices.h"
#include "lan_connection.h"
#include "lan_saves.h"
#include "lan_sync.h"
#include "lan_games.h"
#include "saves.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qdir.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtimer.h>
#include <QtNetwork/qhostaddress.h>
#include <cstdio>
#include <stdexcept>

using namespace novelty;

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
        QString firstPhrase, secondPhrase, error;
        bool serverReady = false, clientReady = false;
        QEventLoop pairing;
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &pairing, &QEventLoop::quit);
        auto checkReady = [&] { if (serverReady && clientReady) pairing.quit(); };
        listener.onConnection = [&](LanConnection *connection) {
            connection->onPhrase = [&, connection](const QString &, const QString &phrase) {
                firstPhrase = phrase; connection->approvePairing("Desktop");
            };
            connection->onReady = [&](const QString &) { serverReady = true; checkReady(); };
            connection->onError = [&](const QString &message) { error = message; pairing.quit(); };
            return true;
        };
        LanConnection client(desktop, true);
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
        std::puts("PASS: two-sided TLS pairing and matching verification phrase");
        client.close(); listener.close();

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
            connection->onError = [&](const QString &message) { transferError = message; transfer.quit(); };
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
            exchange->onInstalled = [&](const QString &id) { installedId = id; gameTransfer.quit(); };
            try { exchange->catalogue(); }
            catch (const std::exception &e) { gameError = QString::fromUtf8(e.what()); gameTransfer.quit(); }
        };
        gameClient.connectTo("127.0.0.1", trustedListener.serverPort());
        gameTimeout.start(15000); gameTransfer.exec(); gameTimeout.stop();
        gameClient.close();
        if (!gameError.isEmpty() || installedId.isEmpty() ||
            !QFileInfo(newDevice.game(installedId).executable).isFile() ||
            newDevice.game(installedId).folder != newDevice.root() + "/Game")
            throw std::runtime_error(("LAN game install failed: " + gameError).toStdString());
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
