#include "lan_discovery.h"
#include <QtCore/qjsondocument.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace novelty {
LanDiscovery::LanDiscovery(Library &library, quint16 port, QObject *parent)
    : QObject(parent), library_(library), fingerprint_(ensureDeviceIdentity(library).fingerprint),
      port_(port), beacon_(this), poll_(this) {
    socket_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int enabled = 1;
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_port = htons(38374); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (socket_ < 0 || ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        ::setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0 ||
        ::bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (socket_ >= 0) ::close(socket_);
        socket_ = -1; return; // Remembered, authenticated endpoints remain usable.
    }
    connect(&beacon_, &QTimer::timeout, this, [this] { announce(); });
    connect(&poll_, &QTimer::timeout, this, [this] { poll(); });
    beacon_.start(3000); poll_.start(250);
    QTimer::singleShot(0, this, [this] { announce(); });
}
LanDiscovery::~LanDiscovery() { if (socket_ >= 0) ::close(socket_); }
void LanDiscovery::announce() {
    if (socket_ < 0 || !port_) return;
    const auto bytes = QJsonDocument(QJsonObject{{"type", "novelty-service"},
        {"fingerprint", fingerprint_}, {"port", int(port_)}}).toJson(QJsonDocument::Compact);
    sockaddr_in target{}; target.sin_family = AF_INET;
    target.sin_port = htons(38374); target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    ::sendto(socket_, bytes.constData(), size_t(bytes.size()), 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
}
void LanDiscovery::poll() {
    try {
        const auto peers = trustedPeers(library_);
        for (int i = 0; i < 32; ++i) {
            char buffer[256]; sockaddr_in sender{}; socklen_t size = sizeof(sender);
            const auto count = ::recvfrom(socket_, buffer, sizeof(buffer), MSG_TRUNC,
                reinterpret_cast<sockaddr *>(&sender), &size);
            if (count < 0) break;
            if (count >= int(sizeof(buffer)) || sender.sin_family != AF_INET) continue;
            const auto message = QJsonDocument::fromJson(QByteArray(buffer, count)).object();
            const auto fingerprint = message["fingerprint"].toString();
            const auto port = message["port"].toInt();
            if (message["type"] != "novelty-service" || message.size() != 3 ||
                fingerprint == fingerprint_ || port < 1 || port > 65535) continue;
            char address[INET_ADDRSTRLEN];
            if (!::inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address))) continue;
            for (auto peer : peers) if (peer.fingerprint == fingerprint) {
                peer.address = QString::fromLatin1(address); peer.port = quint16(port);
                if (onPeer) onPeer(peer);
                break;
            }
        }
    } catch (const std::exception &) { /* A transient local read failure must not escape a timer. */ }
}
}
