#include "lan_connection.h"
#include "devices.h"
#include "pairing.h"
#include "peer_tls.h"
#include <QtCore/qjsondocument.h>
#include <QtNetwork/qsslerror.h>
#include <unistd.h>

namespace novelty {

LanConnection::LanConnection(Library &library, bool pairing, QObject *parent)
    : QObject(parent), library_(library), socket_(this), timeout_(this), pairing_(pairing) {
    timeout_.setSingleShot(true);
    QObject::connect(&timeout_, &QTimer::timeout, this, [this] { fail("Peer connection timed out."); });
    installSignals();
}

void LanConnection::installSignals() {
    QObject::connect(&socket_, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        if (!acceptExpectedPeerTlsErrors(socket_, errors)) fail("Peer TLS certificate failed verification.");
    });
    QObject::connect(&socket_, &QSslSocket::encrypted, this, [this] { encrypted(); });
    QObject::connect(&socket_, &QSslSocket::readyRead, this, [this] { read(); });
    QObject::connect(&socket_, &QSslSocket::bytesWritten, this, [this](qint64) {
        if (ready_ && !failed_ && onDrain) onDrain();
    });
    QObject::connect(&socket_, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!failed_) fail("Peer connection failed: " + socket_.errorString());
    });
    QObject::connect(&socket_, &QSslSocket::disconnected, this, [this] {
        if (!failed_) fail("Peer disconnected.");
    });
}

void LanConnection::connectTo(const QString &host, quint16 port) {
    if (socket_.state() != QAbstractSocket::UnconnectedState || host.isEmpty() || port == 0) {
        fail("Choose a valid LAN address and port."); return;
    }
    try { configureDeviceTls(socket_, ensureDeviceIdentity(library_)); }
    catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); return; }
    timeout_.start(30000);
    // The generated certificate has this DNS subject alternative name, even for a manual IP connection.
    socket_.connectToHostEncrypted(host, port, "novelty.local");
}

void LanConnection::acceptDescriptor(qintptr descriptor) {
    if (socket_.state() != QAbstractSocket::UnconnectedState) { fail("Peer connection already started."); return; }
    try { configureDeviceTls(socket_, ensureDeviceIdentity(library_)); }
    catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); return; }
    if (!socket_.setSocketDescriptor(descriptor)) { ::close(descriptor); fail("Cannot accept peer socket."); return; }
    timeout_.start(30000);
    socket_.startServerEncryption();
}

void LanConnection::encrypted() {
    if (failed_) return;
    const auto certificate = socket_.peerCertificate();
    if (!expectedPeerTlsErrors(certificate, socket_.sslHandshakeErrors())) {
        fail("Peer TLS identity is invalid."); return;
    }
    peerFingerprint_ = peerCertificateFingerprint(certificate);
    encrypted_ = true;
    if (!pairing_) {
        try {
            peerFingerprint_ = authorizedPeerFingerprint(library_, socket_);
            ready_ = true; timeout_.stop();
            if (onReady) onReady(peerFingerprint_);
        } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
        return;
    }
    try {
        if (isTrustedPeer(library_, peerFingerprint_)) {
            fail("This device is already paired. Reconnect normally or revoke it first."); return;
        }
        localNonce_ = newPairingNonce();
        const QJsonObject hello{{"type", "pair-hello"}, {"nonce", QString::fromLatin1(localNonce_.toHex())}};
        if (socket_.write(encodeControlFrame(QJsonDocument(hello).toJson(QJsonDocument::Compact))) < 0)
            fail("Cannot send pairing hello.");
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::read() {
    if (!encrypted_ || failed_) return;
    input_ += socket_.readAll();
    try {
        QByteArray payload;
        while (takeControlFrame(input_, payload)) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(payload, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                throw std::runtime_error("Malformed peer message.");
            process(document.object());
            if (failed_) return;
        }
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::process(const QJsonObject &message) {
    const auto type = message["type"].toString();
    if (pairing_ && !ready_) {
        if (type == "pair-hello" && !helloReceived_ && message.size() == 2) {
            const auto encoded = message["nonce"].toString();
            if (encoded.size() != 64) {
                fail("Invalid pairing nonce."); return;
            }
            remoteNonce_ = QByteArray::fromHex(encoded.toLatin1());
            if (remoteNonce_.size() != 32 || remoteNonce_.toHex() != encoded.toLatin1()) {
                fail("Invalid pairing nonce."); return;
            }
            helloReceived_ = true;
            timeout_.start(5 * 60 * 1000);
            try {
                const auto phrase = pairingPhrase(ensureDeviceIdentity(library_).fingerprint, localNonce_,
                                                  peerFingerprint_, remoteNonce_);
                if (onPhrase) onPhrase(peerFingerprint_, phrase);
            } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
            return;
        }
        if (type == "pair-approve" && helloReceived_ && !remoteApproved_ && message.size() == 1) {
            remoteApproved_ = true; finishPairing(); return;
        }
        fail("Unexpected pairing message."); return;
    }
    if (!ready_ || type.isEmpty() || type.startsWith("pair-")) { fail("Unauthorized peer message."); return; }
    try {
        if (!isTrustedPeer(library_, peerFingerprint_)) { fail("Peer access was revoked."); return; }
        if (onMessage) onMessage(message);
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::approvePairing(const QString &peerName) {
    if (!pairing_ || !helloReceived_ || localApproved_ || ready_ || failed_ || peerName.trimmed().isEmpty()) {
        fail("Pairing approval is not available yet."); return;
    }
    pendingPeerName_ = peerName.trimmed();
    localApproved_ = true;
    const QJsonObject message{{"type", "pair-approve"}};
    if (socket_.write(encodeControlFrame(QJsonDocument(message).toJson(QJsonDocument::Compact))) < 0)
        fail("Cannot send pairing approval.");
    finishPairing();
}

void LanConnection::finishPairing() {
    if (!localApproved_ || !remoteApproved_ || !helloReceived_ || failed_) return;
    try {
        approvePairedPeer(library_, peerFingerprint_, pendingPeerName_);
        ready_ = true; timeout_.stop();
        if (onReady) onReady(peerFingerprint_);
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::send(const QJsonObject &message) {
    if (!ready_ || failed_ || message["type"].toString().isEmpty() ||
        message["type"].toString().startsWith("pair-") || !isTrustedPeer(library_, peerFingerprint_))
        throw std::runtime_error("Peer is not authorized for application messages.");
    if (socket_.write(encodeControlFrame(QJsonDocument(message).toJson(QJsonDocument::Compact))) < 0)
        throw std::runtime_error("Cannot write peer message.");
}

void LanConnection::close() { timeout_.stop(); failed_ = true; socket_.abort(); }

void LanConnection::fail(const QString &reason) {
    if (failed_) return;
    failed_ = true; timeout_.stop(); socket_.abort();
    if (onError) onError(reason);
}

LanListener::LanListener(Library &library, bool pairing, QObject *parent)
    : QTcpServer(parent), library_(library), pairing_(pairing) {
    setMaxPendingConnections(4);
}

void LanListener::incomingConnection(qintptr descriptor) {
    if (!onConnection) { ::close(descriptor); return; }
    auto connection = new LanConnection(library_, pairing_, this);
    if (!onConnection(connection)) { delete connection; ::close(descriptor); return; }
    connection->acceptDescriptor(descriptor);
}

} // namespace novelty
