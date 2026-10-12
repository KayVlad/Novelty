#include "lan_sync.h"
#include "devices.h"
#include "lan_connection.h"
#include "lan_saves.h"
#include "saves.h"
#include <QtCore/qeventloop.h>
#include <QtCore/qset.h>
#include <QtCore/qtimer.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {

PeerRefreshResult refreshLinkedPeerSaves(Library &library, const QString &gameId,
                                         int connectTimeoutMs, const QString &selectedFingerprint) {
    if (connectTimeoutMs < 100 || connectTimeoutMs > 30000)
        throw std::runtime_error("Invalid peer connection timeout.");
    if (library.busy(gameId)) throw std::runtime_error("Wait for the game to finish before checking peer saves.");
    const auto config = loadSaveConfig(library, gameId);
    PeerRefreshResult result;
    if (config.mappings.isEmpty()) return result;
    const auto peers = trustedPeers(library);
    for (const auto &peer : peers) {
        if (!selectedFingerprint.isEmpty() && peer.fingerprint != selectedFingerprint) continue;
        QString linkedId;
        for (const auto &link : config.peerLinks)
            if (link.fingerprint == peer.fingerprint) { linkedId = link.profileGameId; break; }
        if (linkedId.isEmpty()) linkedId = config.profileGameId;
        if (peer.address.isEmpty() || peer.port == 0) continue;
        ++result.peersChecked;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QString issue;
        bool complete = false;
        QStringList remaining;
        const auto finish = [&](const QString &message = {}) {
            if (complete) return;
            complete = true; issue = message; loop.quit();
        };
        QObject::connect(&timeout, &QTimer::timeout, &loop,
                         [&] { finish("Peer save check timed out."); });
        LanConnection connection(library, false);
        connection.onError = [&](const QString &message) { finish(message); };
        connection.onReady = [&](const QString &fingerprint) {
            if (fingerprint != peer.fingerprint) { finish("Peer identity changed."); return; }
            try {
                auto exchange = new LanSaveExchange(library, connection, &connection);
                exchange->onError = [&](const QString &message) { finish(message); };
                exchange->onProgress = [&](qint64, qint64) { timeout.start(120000); };
                const auto fetchNext = [&, exchange] {
                    if (remaining.isEmpty()) { finish(); return; }
                    try {
                        timeout.start(120000);
                        exchange->fetch(gameId, linkedId, remaining.first());
                    } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
                };
                exchange->onMatches = [&, fetchNext](const QJsonArray &matches) {
                    QJsonObject match;
                    for (const auto &value : matches) {
                        const auto item = value.toObject();
                        if (item["profile_game_id"].toString() == linkedId) {
                            if (!match.isEmpty()) { finish("Peer reported duplicate game identities."); return; }
                            match = item;
                        }
                    }
                    if (match.isEmpty() && matches.size() == 1 && linkedId == config.profileGameId) {
                        match = matches.first().toObject();
                        const auto remoteId = match["profile_game_id"].toString();
                        if (QUuid(remoteId).isNull()) { finish("Peer reported an invalid game identity."); return; }
                        try {
                            linkPeerSaveGame(library, gameId, peer.fingerprint, remoteId);
                            linkedId = remoteId;
                        } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); return; }
                    } else if (match.isEmpty() && matches.size() > 1) {
                        finish("Several peer games share this folder name. Choose the matching game in Saves."); return;
                    }
                    if (match["compatibility_issue"].toBool()) {
                        finish("This linked game's saved edition or compatibility labels differ. Review them before syncing.");
                        return;
                    }
                    const auto snapshotId = match["snapshot_id"].toString();
                    if (!selectedFingerprint.isEmpty()) result.latestSnapshotId = snapshotId;
                    if (snapshotId.isEmpty() || snapshotId == appliedSaveSnapshotId(library, gameId)) {
                        finish(); return;
                    }
                    try {
                        if (!selectedFingerprint.isEmpty()) {
                            if (QUuid(snapshotId).isNull()) throw std::runtime_error("Peer save head is invalid.");
                            const auto advertised = match["snapshot_ids"].toArray();
                            if (!advertised.isEmpty() && advertised.last().toString() != snapshotId)
                                throw std::runtime_error("Peer save head does not match its history.");
                            bool known = false;
                            for (const auto &snapshot : listSaveSnapshots(library, gameId))
                                if (snapshot.id == snapshotId) { known = true; break; }
                            if (known) { verifiedSaveSnapshotBundle(library, gameId, snapshotId); finish(); return; }
                            remaining = {snapshotId};
                            fetchNext();
                            return;
                        }
                        QStringList history;
                        const auto advertised = match["snapshot_ids"].toArray();
                        if (advertised.size() > 64) throw std::runtime_error("Peer save history is too long.");
                        QSet<QString> seen;
                        for (const auto &value : advertised) {
                            const auto id = value.toString();
                            if (QUuid(id).isNull() || seen.contains(id))
                                throw std::runtime_error("Peer save history is invalid.");
                            seen.insert(id); history.append(id);
                        }
                        if (history.isEmpty()) history.append(snapshotId);
                        if (history.last() != snapshotId)
                            throw std::runtime_error("Peer save head does not match its history.");
                        QSet<QString> known;
                        for (const auto &snapshot : listSaveSnapshots(library, gameId)) known.insert(snapshot.id);
                        remaining.clear();
                        for (const auto &id : history) {
                            if (known.contains(id)) verifiedSaveSnapshotBundle(library, gameId, id);
                            else remaining.append(id);
                        }
                        fetchNext();
                    } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
                };
                exchange->onImported = [&, fetchNext](const SaveImportResult &imported) {
                    if (remaining.isEmpty() || imported.snapshotId != remaining.first()) {
                        finish("Peer sent an unexpected save snapshot."); return;
                    }
                    remaining.removeFirst();
                    if (imported.added) ++result.snapshotsImported;
                    fetchNext();
                };
                exchange->locate(config);
            } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
        };
        timeout.start(connectTimeoutMs);
        connection.connectTo(peer.address, peer.port);
        if (!complete) loop.exec();
        timeout.stop(); connection.close();
        if (!issue.isEmpty()) result.issues.append(peer.name + ": " + issue);
    }
    return result;
}

} // namespace novelty
