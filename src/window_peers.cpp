#include "window_internal.h"

namespace novelty {
void Window::refreshPeerGames() {
    if (!rootAvailable() || importing_ || removing_) return;
    try {
        const auto peers = trustedPeers(*library_);
        QSet<QString> trusted;
        for (const auto &peer : peers) trusted.insert(peer.fingerprint);
        bool changed = false;
        for (const auto &fingerprint : peerGames_.keys()) if (!trusted.contains(fingerprint)) {
            peerGames_.remove(fingerprint); peerLastAttempt_.remove(fingerprint); changed = true;
        }
        if (changed) refresh();
        for (const auto &peer : peers) queryPeerGames(peer);
        if (peerDiscovery_) peerDiscovery_->announce();
    } catch (const std::exception &e) { message("Could not check paired devices: " + QString::fromUtf8(e.what())); }
}
void Window::queryPeerGames(const TrustedPeer &peer) {
    if (!rootAvailable() || importing_ || removing_ || peer.address.isEmpty() || !peer.port ||
        catalogueConnections_.size() >= 2 || cataloguePeers_.contains(peer.fingerprint)) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (now - peerLastAttempt_.value(peer.fingerprint) < 5000) return;
    // Discovery packets cannot change trust or stored addresses. Only the matching
    // authenticated TLS connection below can publish this endpoint and its games.
    peerLastAttempt_[peer.fingerprint] = now;
    auto connection = new LanConnection(*library_, false, this);
    catalogueConnections_.insert(connection); cataloguePeers_.insert(peer.fingerprint);
    const auto finish = [this, connection, peer](const QString &error = {}) {
        if (!catalogueConnections_.remove(connection)) return;
        cataloguePeers_.remove(peer.fingerprint);
        connection->close(); connection->deleteLater();
        if (!error.isEmpty()) {
            bool changed = false;
            for (auto &game : peerGames_[peer.fingerprint]) if (game.online) { game.online = false; changed = true; }
            if (changed) refresh();
            message(peer.name + " is unavailable: " + error + " Retrying automatically.");
        } else peerLastAttempt_[peer.fingerprint] = QDateTime::currentMSecsSinceEpoch() + 25000;
    };
    connection->onError = finish;
    connection->onReady = [this, connection, peer, finish](const QString &fingerprint) {
        if (fingerprint != peer.fingerprint) { finish("Device identity did not match."); return; }
        try {
            rememberPeerEndpoint(*library_, fingerprint, peer.address, peer.port);
            auto router = new LanSaveExchange(*library_, *connection, connection);
            auto exchange = new LanGameExchange(*library_, *connection, *router, connection);
            auto remaining = std::make_shared<QStringList>();
            const auto next = [exchange, remaining, finish] {
                if (remaining->isEmpty()) { finish(); return; }
                try { exchange->preview(remaining->takeFirst()); }
                catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
            };
            exchange->onError = finish;
            exchange->onCatalogue = [this, peer, remaining, next](const QJsonArray &catalogue) {
                auto namedPeer = peer;
                for (const auto &known : trustedPeers(*library_)) if (known.fingerprint == peer.fingerprint) namedPeer.name = known.name;
                QList<PeerGame> games;
                for (const auto &item : catalogue) {
                    PeerGame game; game.peer = namedPeer; game.descriptor = item.toObject();
                    for (const auto &prior : peerGames_.value(peer.fingerprint)) if (prior.id() == game.id()) {
                        game.cover = prior.cover;
                        game.descriptor["metadata"] = prior.descriptor["metadata"];
                        break;
                    }
                    if (game.descriptor["preview_available"].toBool() && game.cover.isNull())
                        remaining->append(game.descriptor["profile_game_id"].toString());
                    games.append(game);
                }
                peerGames_[peer.fingerprint] = games;
                if (notice_->text().startsWith(peer.name + " is unavailable:")) message("Connected to " + namedPeer.name + ".");
                refresh(); next();
            };
            exchange->onPreview = [this, peer, next](const QString &id, const QJsonObject &preview) {
                for (auto &game : peerGames_[peer.fingerprint]) if (game.descriptor["profile_game_id"] == id) {
                    game.descriptor["metadata"] = preview["metadata"].toObject();
                    const auto encoded = preview["cover"].toString();
                    if (encoded.size() <= 40 * 1024) {
                        QBuffer buffer; buffer.setData(QByteArray::fromBase64(encoded.toLatin1())); buffer.open(QIODevice::ReadOnly);
                        QImageReader reader(&buffer, "JPEG");
                        const auto size = reader.size();
                        if (size.isValid() && size.width() <= 240 && size.height() <= 336) game.cover = reader.read();
                    }
                }
                refresh(); next();
            };
            exchange->catalogue();
        } catch (const std::exception &e) { finish(QString::fromUtf8(e.what())); }
    };
    QTimer::singleShot(30000, connection, [finish] { finish("Game catalogue timed out."); });
    connection->connectTo(peer.address, peer.port);
}
void Window::installFromPeer() {
    if (!rootAvailable() || importing_ || removing_ || !pendingLaunch_.isEmpty()) return;
    const auto selectedId = selected().id;
    if (!model_->remoteGames.contains(selectedId)) return;
    auto game = model_->remoteGames.value(selectedId);
    if (!game.online) { message("Open Novelty on " + game.peer.name + " to install this game."); refreshPeerGames(); return; }
    // The discovery catalogue is transient UI state, never an installed game row.
    try {
        bool trusted = false;
        for (const auto &peer : trustedPeers(*library_)) if (peer.fingerprint == game.peer.fingerprint) { game.peer = peer; trusted = true; }
        if (!trusted) { refreshPeerGames(); return; }
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    QDialog dialog(this); dialog.setObjectName("installFromPeerDialog");
    dialog.setWindowTitle("Installing " + game.descriptor["name"].toString()); dialog.resize(520, 150);
    auto layout = new QVBoxLayout(&dialog);
    auto status = new QLabel("Connecting to " + game.peer.name + "…", &dialog);
    status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    LanConnection connection(*library_, false, &dialog);
    QString installedId;
    const auto failed = [&](const QString &reason) {
        status->setText(reason + " Close this window and retry Install; completed files are kept.");
        buttons->setStandardButtons(QDialogButtonBox::Close);
    };
    connection.onError = failed;
    connection.onReady = [&](const QString &fingerprint) {
        if (fingerprint != game.peer.fingerprint) { failed("The device identity changed."); connection.close(); return; }
        try {
            auto router = new LanSaveExchange(*library_, connection, &connection);
            auto exchange = new LanGameExchange(*library_, connection, *router, &connection);
            exchange->onError = failed;
            exchange->onProgress = [&](qint64 done, qint64 total) {
                status->setText("Transferring verified game files: " + QString::number(double(done) / 1048576.0, 'f', 1) +
                    " / " + QString::number(double(total) / 1048576.0, 'f', 1) + " MiB");
            };
            exchange->onInstalled = [&](const QString &id) { installedId = id; dialog.accept(); };
            exchange->install(game.descriptor);
            status->setText("Preparing game files on " + game.peer.name + "…");
        } catch (const std::exception &e) { failed(QString::fromUtf8(e.what())); connection.close(); }
    };
    importing_ = true; selectGame();
    QTimer::singleShot(0, &dialog, [&] { connection.connectTo(game.peer.address, game.peer.port); });
    dialog.exec(); connection.close(); importing_ = false;
    if (installedId.isEmpty()) { selectGame(); return; }
    if (!game.cover.isNull()) {
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); game.cover.save(&buffer, "PNG");
        saveMetadataCover(metadataCoverPath(library_->dataRoot(), installedId), bytes);
    }
    refresh();
    try {
        if (!loadSaveServerConfig(*library_).enabled) {
            const auto root = library_->root();
            auto work = std::async(std::launch::async, [root, installedId, fingerprint = game.peer.fingerprint] {
                Library worker(root);
                const auto result = refreshLinkedPeerSaves(worker, installedId, 5000, fingerprint);
                return result;
            });
            RemovalProgress progress(this); progress.setWindowTitle("Checking initial saves");
            auto layout = new QVBoxLayout(&progress);
            layout->addWidget(new QLabel("Fetching the source device's latest verified save state…", &progress));
            progress.resize(440, 110);
            QTimer poll; poll.setInterval(60);
            connect(&poll, &QTimer::timeout, &progress, [&] {
                if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
            });
            poll.start(); progress.exec(); poll.stop();
            const auto result = work.get();
            message(result.issues.isEmpty() ?
                "Game installed. Initial saves checked; preparing local setup…" :
                "Game installed. Initial saves need attention: " + result.issues.join(" "));
        } else message("Game installed. Preparing local setup and checking server saves…");
    } catch (const std::exception &e) { message("Game installed. " + QString::fromUtf8(e.what())); }
    for (int row = 0; row < model_->games.size(); ++row) if (model_->games[row].id == installedId) {
        table_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, 0)));
        // Runner paths and prefixes belong to this device; finish its normal setup
        // after the transfer. Cancellation keeps the installation for a later Play.
        if (model_->games[row].backend == "umu") launch(true);
        break;
    }
}
void Window::syncSavesFromPeer(const QString &gameId, QWidget *parent) {
    if (!library_ || !rootAvailable()) throw std::runtime_error("Reconnect the Games folder before transferring saves.");
    if (loadSaveServerConfig(*library_).enabled)
        throw std::runtime_error("Turn off automatic server sync before transferring peer saves.");
    if (loadSaveConfig(*library_, gameId).mappings.isEmpty())
        throw std::runtime_error("Choose this game's save location before transferring saves.");
    auto peers = trustedPeers(*library_);
    if (peers.isEmpty()) {
        devices();
        peers = trustedPeers(*library_);
        if (peers.isEmpty()) return;
    }
    QStringList choices;
    for (const auto &peer : peers)
        choices.append(peer.name + " · " + peer.fingerprint.left(12) +
                       (peer.address.isEmpty() || peer.port == 0 ? " · no remembered address" : QString()));
    QDialog chooser(parent); chooser.setWindowTitle("Transfer saves");
    auto chooserLayout = new QVBoxLayout(&chooser);
    chooserLayout->addWidget(new QLabel("Choose the source device:", &chooser));
    auto source = new QComboBox(&chooser); source->addItems(choices); chooserLayout->addWidget(source);
    auto chooserButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
    chooserLayout->addWidget(chooserButtons);
    connect(chooserButtons, &QDialogButtonBox::accepted, &chooser, &QDialog::accept);
    connect(chooserButtons, &QDialogButtonBox::rejected, &chooser, &QDialog::reject);
    if (chooser.exec() != QDialog::Accepted) return;
    const auto selected = source->currentIndex();
    if (selected < 0) return;
    const auto peer = peers[selected];
    if (peer.address.isEmpty() || peer.port == 0) {
        QMessageBox::information(parent, "Transfer saves", "Open Devices and connect to this paired device first.");
        return;
    }
    const auto root = library_->root();
    auto work = std::async(std::launch::async, [root, gameId, fingerprint = peer.fingerprint] {
        Library worker(root);
        const auto result = refreshLinkedPeerSaves(worker, gameId, 5000, fingerprint);
        if (!result.issues.isEmpty() || result.latestSnapshotId.isEmpty()) return std::pair{result, SaveSnapshot{}};
        return std::pair{result, captureVerifiedSaveState(worker, gameId, "before manual peer transfer")};
    });
    RemovalProgress progress(parent); progress.setWindowTitle("Fetching saves");
    auto progressLayout = new QVBoxLayout(&progress);
    progressLayout->addWidget(new QLabel("Checking the selected device and verifying both save states…", &progress));
    progress.resize(420, 100);
    QTimer poll; poll.setInterval(60);
    connect(&poll, &QTimer::timeout, &progress, [&] {
        if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
    });
    poll.start(); progress.exec(); poll.stop();
    const auto [result, local] = work.get();
    if (!result.issues.isEmpty()) {
        QMessageBox::information(parent, "Transfer saves", result.issues.join("\n"));
        return;
    }
    if (result.peersChecked == 0) {
        QMessageBox::information(parent, "Transfer saves", "The selected device is unavailable. Open Devices to reconnect it.");
        return;
    }
    const auto snapshotId = result.latestSnapshotId;
    if (snapshotId.isEmpty()) {
        QMessageBox::information(parent, "Transfer saves", "This device has no verified save state for the matching game.");
        return;
    }
    if (local.id == snapshotId) {
        QMessageBox::information(parent, "Transfer saves", "These saves are already installed locally.");
        return;
    }
    if (local.files > 0 && QMessageBox::question(parent, "Replace local saves?",
        "Saves in this game's configured locations will be replaced with saves from " + peer.name +
        ". Novelty will keep the current state so you can undo this transfer.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    auto restore = std::async(std::launch::async, [root, gameId, snapshotId, expected = local.id] {
        Library worker(root); replaceSavesFromPeer(worker, gameId, snapshotId, expected);
    });
    RemovalProgress restoring(parent); restoring.setWindowTitle("Transferring saves");
    auto restoringLayout = new QVBoxLayout(&restoring);
    restoringLayout->addWidget(new QLabel("Installing the selected saves and recording Undo…", &restoring));
    restoring.resize(420, 100);
    QTimer restorePoll; restorePoll.setInterval(60);
    connect(&restorePoll, &QTimer::timeout, &restoring, [&] {
        if (restore.wait_for(std::chrono::seconds(0)) == std::future_status::ready) restoring.accept();
    });
    restorePoll.start(); restoring.exec(); restorePoll.stop(); restore.get();
    message("Saves transferred from " + peer.name + ". Undo is available in Manage → Saves.");
}
} // namespace novelty
