#include "window_internal.h"

namespace novelty {
namespace {
QStringList localLanAddresses() {
    QStringList addresses;
    ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) return addresses;
    for (auto entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {};
        auto address = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
            QString::fromLatin1(text) != "127.0.0.1") addresses.append(QString::fromLatin1(text));
    }
    ::freeifaddrs(interfaces);
    addresses.removeDuplicates();
    return addresses;
}
}
void Window::devices() {
    if (!library_ || !rootAvailable()) { message("Choose an available Games folder first."); return; }
    try { ensureDeviceIdentity(*library_); }
    catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    QDialog dialog(this); dialog.setObjectName("devicesDialog"); dialog.setWindowTitle("Devices"); dialog.resize(560, 420);
    auto layout = new QVBoxLayout(&dialog);
    auto own = new QLabel("This device: " + QSysInfo::machineHostName(), &dialog);
    own->setToolTip(ensureDeviceIdentity(*library_).fingerprint);
    own->setWordWrap(true); own->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(own);
    auto syncAddress = new QLabel(saveListener_ ?
        "Game transfers and save sync use port " + QString::number(saveListener_->serverPort()) :
        lanListenIssue_, &dialog);
    syncAddress->setWordWrap(true); syncAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(syncAddress);
    auto list = new QListWidget(&dialog); list->setObjectName("trustedDevices"); layout->addWidget(list, 1);
    auto render = [&] {
        list->clear();
        for (const auto &peer : trustedPeers(*library_)) {
            auto item = new QListWidgetItem(peer.name + " — " + peer.fingerprint.left(12) + "…" +
                (peer.address.isEmpty() ? QString() : " · " + peer.address + ':' + QString::number(peer.port)), list);
            item->setData(Qt::UserRole, peer.fingerprint);
            item->setToolTip(peer.fingerprint);
        }
    };
    try { render(); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    auto actions = new QHBoxLayout;
    auto pairButton = new QPushButton("Pair device…", &dialog);
    auto revokeButton = new QPushButton("Revoke selected", &dialog); revokeButton->setEnabled(false);
    actions->addWidget(pairButton); actions->addWidget(revokeButton); actions->addStretch(); layout->addLayout(actions);
    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [=] { revokeButton->setEnabled(list->currentItem() != nullptr); });
    connect(revokeButton, &QPushButton::clicked, &dialog, [&, this] {
        if (!list->currentItem()) return;
        try { revokePeer(*library_, list->currentItem()->data(Qt::UserRole).toString()); render(); refreshPeerGames(); }
        catch (const std::exception &e) { QMessageBox::warning(&dialog, "Cannot revoke device", QString::fromUtf8(e.what())); }
    });
    connect(pairButton, &QPushButton::clicked, &dialog, [&, this] {
        QDialog pair(&dialog); pair.setObjectName("pairDeviceDialog"); pair.setWindowTitle("Pair a LAN device"); pair.resize(550, 370);
        auto column = new QVBoxLayout(&pair);
        LanListener listener(*library_, true, &pair);
        if (!listener.listen(QHostAddress::AnyIPv4, 0)) {
            QMessageBox::warning(&pair, "Cannot pair", "Cannot listen for LAN peers: " + listener.errorString()); return;
        }
        auto addresses = localLanAddresses();
        auto instructions = new QLabel("Open this screen on both devices. Select the other device below and connect. Check that both devices show the same six-digit code. Names are unverified until you pair. Confirm only with your own device in front of you.", &pair);
        instructions->setWordWrap(true); column->addWidget(instructions);
        auto local = new QLabel("This device: " + QSysInfo::machineHostName() + " · " + (addresses.isEmpty() ? QString("Find its IP in network settings") : addresses.join(", ")) +
                                "  ·  port " + QString::number(listener.serverPort()), &pair);
        local->setTextInteractionFlags(Qt::TextSelectableByMouse); local->setWordWrap(true); column->addWidget(local);
        auto row = new QHBoxLayout;
        auto host = new QLineEdit(&pair); host->setPlaceholderText("Other device's LAN IP address");
        auto port = new QLineEdit(QString::number(listener.serverPort()), &pair);
        port->setValidator(new QIntValidator(1, 65535, port)); port->setMaximumWidth(90);
        auto connectButton = new QPushButton("Connect", &pair);
        row->addWidget(host, 1); row->addWidget(port); row->addWidget(connectButton); column->addLayout(row);
        auto discovered = new QListWidget(&pair); discovered->setObjectName("discoveredPairingDevices");
        discovered->setMaximumHeight(90); column->addWidget(discovered);
        auto discoveryNote = new QLabel("Nearby pairing screens appear here when UDP discovery is available. You can always enter an IP address above.", &pair);
        discoveryNote->setWordWrap(true); column->addWidget(discoveryNote);
        QTimer beacon(&pair), discoveryPoll(&pair);
        const auto ownSession = newDiscoverySessionId();
        const int discoveryFd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int enabled = 1;
        sockaddr_in discoveryAddress{};
        discoveryAddress.sin_family = AF_INET; discoveryAddress.sin_port = htons(38374);
        discoveryAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool discoveryReady = discoveryFd >= 0 &&
            ::setsockopt(discoveryFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0 &&
            ::setsockopt(discoveryFd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            ::bind(discoveryFd, reinterpret_cast<sockaddr *>(&discoveryAddress), sizeof(discoveryAddress)) == 0;
        if (discoveryReady) {
            beacon.setInterval(2000);
            auto announce = [&] {
                const auto bytes = encodeDiscoveryAnnouncement({ownSession, listener.serverPort()});
                sockaddr_in broadcast{}; broadcast.sin_family = AF_INET; broadcast.sin_port = htons(38374);
                broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                ::sendto(discoveryFd, bytes.constData(), size_t(bytes.size()), 0,
                         reinterpret_cast<sockaddr *>(&broadcast), sizeof(broadcast));
            };
            connect(&beacon, &QTimer::timeout, &pair, announce);
            discoveryPoll.setInterval(250);
            connect(&discoveryPoll, &QTimer::timeout, &pair, [&] {
                for (;;) {
                    QByteArray bytes(64, Qt::Uninitialized); sockaddr_in sender{}; socklen_t senderSize = sizeof(sender);
                    const auto size = ::recvfrom(discoveryFd, bytes.data(), size_t(bytes.size()), 0,
                                                reinterpret_cast<sockaddr *>(&sender), &senderSize);
                    if (size < 0) { if (errno == EINTR) continue; break; }
                    if (size > bytes.size() || sender.sin_family != AF_INET ||
                        (ntohl(sender.sin_addr.s_addr) >> 24) == 127) continue;
                    char addressBuffer[INET_ADDRSTRLEN];
                    if (!::inet_ntop(AF_INET, &sender.sin_addr, addressBuffer, sizeof(addressBuffer))) continue;
                    bytes.resize(size);
                    DiscoveryAnnouncement item;
                    if (!decodeDiscoveryAnnouncement(bytes, item) || item.sessionId == ownSession) continue;
                    const auto address = QString::fromLatin1(addressBuffer);
                    bool found = false;
                    for (int i = 0; i < discovered->count(); ++i)
                        if (discovered->item(i)->data(Qt::UserRole).toByteArray() == item.sessionId) {
                            discovered->item(i)->setText("Nearby: " + address + ':' + QString::number(item.port));
                            discovered->item(i)->setData(Qt::UserRole + 1, address);
                            discovered->item(i)->setData(Qt::UserRole + 2, int(item.port));
                            found = true; break;
                        }
                    if (!found && discovered->count() < 16) {
                        auto row = new QListWidgetItem("Nearby: " + address + ':' + QString::number(item.port), discovered);
                        row->setData(Qt::UserRole, item.sessionId);
                        row->setData(Qt::UserRole + 1, address); row->setData(Qt::UserRole + 2, int(item.port));
                    }
                }
            });
            connect(discovered, &QListWidget::itemSelectionChanged, &pair, [&] {
                if (const auto selected = discovered->currentItem()) {
                    host->setText(selected->data(Qt::UserRole + 1).toString());
                    port->setText(QString::number(selected->data(Qt::UserRole + 2).toInt()));
                }
            });
            discoveryPoll.start(); beacon.start(); announce();
        } else discoveryNote->setText("UDP discovery is unavailable here. Enter the other device's LAN IP address above.");
        auto phrase = new QLabel("Waiting for a device…", &pair); phrase->setObjectName("pairingPhrase");
        phrase->setTextInteractionFlags(Qt::TextSelectableByMouse); phrase->setWordWrap(true); column->addWidget(phrase);
        auto code = new QLineEdit(&pair); code->setObjectName("pairingCode"); code->setMaxLength(6);
        code->setPlaceholderText("Six-digit code"); code->setValidator(new QIntValidator(0, 999999, code));
        code->hide(); column->addWidget(code);
        auto checkedCode = new QCheckBox("I checked the matching code on my other device", &pair);
        column->addWidget(checkedCode); checkedCode->hide();
        auto approve = new QPushButton("Pair", &pair); approve->setObjectName("approvePairing");
        approve->setEnabled(false); column->addWidget(approve);
        auto state = new QLabel(&pair); state->setTextFormat(Qt::PlainText); state->setWordWrap(true); column->addWidget(state);
        column->addStretch();
        LanConnection *active = nullptr;
        bool enteringCode = false;
        auto attach = [&](LanConnection *connection, bool outgoing) {
            enteringCode = outgoing; checkedCode->setChecked(false); checkedCode->setVisible(!outgoing); code->clear(); code->setEnabled(true); code->setVisible(outgoing);
            connection->usePairingCode(QSysInfo::machineHostName());
            connection->setServicePort(saveListener_ ? saveListener_->serverPort() : 0);
            connection->onEndpoint = [&, this] { render(); refreshPeerGames(); };
            active = connection; approve->setEnabled(false); phrase->setText("Connecting…"); state->clear();
            connection->onPhrase = [&, connection](const QString &fingerprint, const QString &value) {
                if (active != connection) return;
                phrase->setToolTip("Device certificate: " + fingerprint);
                phrase->setText("Pairing code: " + value + "\nUnverified device: " + connection->peerName());
                phrase->setTextFormat(Qt::PlainText);
                state->setText(enteringCode ? "Enter the code you read on your other device." :
                    "Compare both screens yourself before checking the box and allowing this device.");
                approve->setText(enteringCode ? "Pair" : "Allow " + connection->peerName());
                approve->setEnabled(enteringCode);
                if (enteringCode) code->setFocus();
            };
            connection->onRemoteApproval = [&, connection] {
                if (active == connection && !enteringCode)
                    state->setText("The other device sent approval. You must still check the code on both screens yourself.");
            };
            connection->onReady = [&, connection](const QString &) {
                if (active != connection) return;
                code->hide(); approve->setEnabled(false); phrase->setText("Paired with " + connection->peerName() + ".");
                state->setText("Games and saves connect automatically while Novelty is open.");
                render();
            };
            connection->onError = [&, connection](const QString &error) {
                if (active != connection) return;
                approve->setEnabled(false); state->setText(error); active = nullptr;
                connection->deleteLater();
            };
        };
        listener.onConnection = [&](LanConnection *connection) {
            if (active) return false;
            attach(connection, false); return true;
        };
        connect(connectButton, &QPushButton::clicked, &pair, [&, this] {
            if (active) { state->setText("Finish or close the current pairing first."); return; }
            if (QHostAddress(host->text().trimmed()).isNull()) {
                state->setText("Enter the other device's LAN IP address."); return;
            }
            auto connection = new LanConnection(*library_, true, &pair);
            attach(connection, true);
            bool validPort = false;
            const auto number = port->text().toUShort(&validPort);
            if (!validPort || number == 0) { state->setText("Enter a valid port."); connection->deleteLater(); active = nullptr; return; }
            connection->connectTo(host->text().trimmed(), number);
        });
        connect(checkedCode, &QCheckBox::toggled, &pair, [&](bool checked) {
            if (!enteringCode) approve->setEnabled(checked && active && !active->pairingCode().isEmpty());
        });
        connect(approve, &QPushButton::clicked, &pair, [&] {
            if (!active) return;
            if (enteringCode) {
                if (!active->submitPairingCode(code->text())) {
                    if (active) state->setText("That code does not match. Check the other device and retry.");
                    return;
                }
                approve->setEnabled(false); code->setEnabled(false);
                state->setText("Code matches. Confirm pairing on " + active->peerName() + ".");
            } else if (checkedCode->isChecked()) { approve->setEnabled(false); active->approvePairing(active->peerName()); }
        });
        connect(code, &QLineEdit::returnPressed, approve, &QPushButton::click);
        connect(&pair, &QDialog::finished, &pair, [&] { listener.close(); if (active) active->close(); });
        pair.exec();
        if (discoveryFd >= 0) ::close(discoveryFd);
    });
    auto close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close); dialog.exec(); refreshPeerGames();
}
} // namespace novelty
