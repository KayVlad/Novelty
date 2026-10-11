#include "lan_connection.h"
#include "devices.h"
#include "pairing.h"
#include "peer_tls.h"
#include <QtCore/qjsondocument.h>
#include <QtCore/qcryptographichash.h>
#include <QtNetwork/qsslerror.h>
#include <unistd.h>
#include <algorithm>

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
    outgoing_ = true;
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
        QJsonObject hello{{"type", "pair-hello"}, {"nonce", QString::fromLatin1(localNonce_.toHex())}};
        if (codePairing_) {
            // Commit before revealing either nonce. A short verification code must
            // not let an intermediary choose its nonce after seeing ours.
            hello["nonce"] = QString::fromLatin1(QCryptographicHash::hash(localNonce_, QCryptographicHash::Sha256).toHex());
            hello["verification"] = "code-v1"; hello["name"] = deviceName_;
        }
        if (socket_.write(encodeControlFrame(QJsonDocument(hello).toJson(QJsonDocument::Compact))) < 0)
            fail("Cannot send pairing hello.");
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::read() {
    if (!encrypted_ || failed_) return;
    try {
        while (socket_.bytesAvailable() > 0) {
            // TCP/TLS can coalesce many valid frames with a previously partial frame.
            // Drain incrementally so the framing buffer bound never rejects a valid burst.
            const auto bytes = socket_.read(7 + maxControlPayload - input_.size());
            if (bytes.isEmpty()) break;
            input_ += bytes;
            QByteArray payload;
            while (takeControlFrame(input_, payload)) {
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(payload, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject())
                    throw std::runtime_error("Malformed peer message.");
                process(document.object());
                if (failed_) return;
            }
        }
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::process(const QJsonObject &message) {
    const auto type = message["type"].toString();
    if (pairing_ && !ready_) {
        if (type == "pair-hello" && !helloReceived_ && remoteCommitment_.isEmpty()) {
            const bool code = message["verification"] == "code-v1";
            if (code != codePairing_ || message.size() != (code ? 4 : 2)) {
                fail("Update Novelty on both devices to use the same pairing screen."); return;
            }
            const auto encoded = message["nonce"].toString();
            if (encoded.size() != 64) {
                fail("Invalid pairing nonce."); return;
            }
            const auto nonce = QByteArray::fromHex(encoded.toLatin1());
            if (nonce.size() != 32 || nonce.toHex() != encoded.toLatin1()) {
                fail("Invalid pairing nonce."); return;
            }
            if (code) {
                remoteName_ = message["name"].toString();
                if (remoteName_.trimmed().isEmpty() || remoteName_.size() > 80 ||
                    std::any_of(remoteName_.begin(), remoteName_.end(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; })) {
                    fail("Invalid device name."); return;
                }
                remoteCommitment_ = nonce;
                const QJsonObject reveal{{"type", "pair-code-nonce"}, {"nonce", QString::fromLatin1(localNonce_.toHex())}};
                if (socket_.write(encodeControlFrame(QJsonDocument(reveal).toJson(QJsonDocument::Compact))) < 0)
                    fail("Cannot send pairing verification.");
                return;
            }
            remoteNonce_ = nonce;
            helloReceived_ = true;
            showVerification();
            return;
        }
        if (type == "pair-code-nonce" && codePairing_ && !helloReceived_ && !remoteCommitment_.isEmpty() && message.size() == 2) {
            const auto encoded = message["nonce"].toString().toLatin1();
            remoteNonce_ = QByteArray::fromHex(encoded);
            if (remoteNonce_.size() != 32 || remoteNonce_.toHex() != encoded ||
                QCryptographicHash::hash(remoteNonce_, QCryptographicHash::Sha256) != remoteCommitment_) {
                fail("Pairing verification changed during the exchange."); return;
            }
            helloReceived_ = true; showVerification();
            return;
        }
        if (type == "pair-approve" && helloReceived_ && !remoteApproved_ && message.size() == 1) {
            remoteApproved_ = true;
            if (onRemoteApproval) onRemoteApproval();
            finishPairing(); return;
        }
        fail("Unexpected pairing message."); return;
    }
    if (!ready_ || type.isEmpty() || type.startsWith("pair-")) { fail("Unauthorized peer message."); return; }
    try {
        if (!isTrustedPeer(library_, peerFingerprint_)) { fail("Peer access was revoked."); return; }
        // Optional post-pairing message: older pairing screens ignore application
        // messages. The temporary pairing socket is never a transfer endpoint.
        if (pairing_ && type == "service-endpoint") {
            const auto port = message["port"].toInt();
            if (message.size() != 2 || port < 1 || port > 65535)
                throw std::runtime_error("Invalid peer transfer port.");
            rememberPeerEndpoint(library_, peerFingerprint_, socket_.peerAddress().toString(), quint16(port));
            if (onEndpoint) onEndpoint();
            return;
        }
        if (onMessage) onMessage(message);
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanConnection::showVerification() {
    timeout_.start(5 * 60 * 1000);
    try {
        const auto phrase = pairingPhrase(ensureDeviceIdentity(library_).fingerprint, localNonce_, peerFingerprint_, remoteNonce_);
        if (codePairing_) {
            const auto digest = QCryptographicHash::hash(phrase.toLatin1(), QCryptographicHash::Sha256).toHex();
            pairingCode_ = QString::number(digest.left(8).toUInt(nullptr, 16) % 1000000).rightJustified(6, '0');
        }
        if (onPhrase) onPhrase(peerFingerprint_, codePairing_ ? pairingCode_ : phrase);
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}
bool LanConnection::submitPairingCode(const QString &code) {
    if (!codePairing_ || !outgoing_ || !helloReceived_ || failed_ || ready_ || localApproved_) return false;
    if (code.size() != 6 || code != pairingCode_) {
        if (++codeAttempts_ >= 3) fail("Too many incorrect codes. Start pairing again.");
        return false;
    }
    codeVerified_ = true;
    approvePairing(remoteName_);
    return !failed_;
}

void LanConnection::approvePairing(const QString &peerName) {
    if (!pairing_ || !helloReceived_ || localApproved_ || ready_ || failed_ || peerName.trimmed().isEmpty()) {
        fail("Pairing approval is not available yet."); return;
    }
    if (codePairing_ && outgoing_ && !codeVerified_) {
        fail("Enter the code shown on the other device before approving."); return;
    }
    pendingPeerName_ = peerName.trimmed();
    localApproved_ = true;
    const QJsonObject message{{"type", "pair-approve"}};
    if (socket_.write(encodeControlFrame(QJsonDocument(message).toJson(QJsonDocument::Compact))) < 0)
        fail("Cannot send pairing approval.");
    finishPairing();
}

void LanConnection::finishPairing() {
    if (!localApproved_ || !remoteApproved_ || !helloReceived_ || failed_ || ready_) return;
    try {
        approvePairedPeer(library_, peerFingerprint_, pendingPeerName_);
        // Compatibility fallback for older peers; upgraded peers send their actual
        // listener port below, including when the preferred port is occupied.
        rememberPeerEndpoint(library_, peerFingerprint_, socket_.peerAddress().toString(), 38373);
        ready_ = true; timeout_.stop();
        if (servicePort_) send({{"type", "service-endpoint"}, {"port", int(servicePort_)}});
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
    // This override does not use QTcpServer's pending-connection queue, so its
    // limit alone does not bound accepted sockets waiting for TLS or pairing.
    int active = 0;
    for (auto child : children()) if (dynamic_cast<LanConnection *>(child)) ++active;
    if (active >= 4) { ::close(descriptor); return; }
    auto connection = new LanConnection(library_, pairing_, this);
    if (!onConnection(connection)) { delete connection; ::close(descriptor); return; }
    connection->acceptDescriptor(descriptor);
}

} // namespace novelty
