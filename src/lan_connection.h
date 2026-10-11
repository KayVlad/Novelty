#pragma once

#include "core.h"
#include <QtCore/qjsonobject.h>
#include <QtCore/qobject.h>
#include <QtCore/qtimer.h>
#include <QtNetwork/qsslsocket.h>
#include <QtNetwork/qtcpserver.h>
#include <functional>

namespace novelty {

class LanConnection : public QObject {
public:
    using PhraseCallback = std::function<void(const QString &fingerprint, const QString &phrase)>;
    using ReadyCallback = std::function<void(const QString &fingerprint)>;
    using MessageCallback = std::function<void(const QJsonObject &)>;
    using ErrorCallback = std::function<void(const QString &)>;

    // Library must outlive this connection. Pairing never authorizes application messages
    // until both sides have approved the same TLS-bound phrase.
    LanConnection(Library &, bool pairing, QObject *parent = nullptr);
    void connectTo(const QString &host, quint16 port);
    void acceptDescriptor(qintptr descriptor);
    void approvePairing(const QString &peerName);
    void send(const QJsonObject &message);
    qint64 pendingBytes() const { return socket_.bytesToWrite(); }
    void close();
    bool ready() const { return ready_; }
    QString peerFingerprint() const { return peerFingerprint_; }
    void setServicePort(quint16 port) { servicePort_ = port; }
    void usePairingCode(const QString &deviceName) { codePairing_ = true; deviceName_ = deviceName.left(80).trimmed(); }
    bool submitPairingCode(const QString &code);
    QString peerName() const { return remoteName_; }
    QString pairingCode() const { return pairingCode_; }
    std::function<void()> onEndpoint;
    std::function<void()> onRemoteApproval;
    PhraseCallback onPhrase;
    ReadyCallback onReady;
    MessageCallback onMessage;
    std::function<void()> onDrain;
    ErrorCallback onError;
private:
    void installSignals();
    void encrypted();
    void read();
    void process(const QJsonObject &);
    void finishPairing();
    void showVerification();
    void fail(const QString &);
    Library &library_;
    QSslSocket socket_;
    QTimer timeout_;
    QByteArray input_, localNonce_, remoteNonce_;
    QByteArray remoteCommitment_;
    QString peerFingerprint_, pendingPeerName_;
    QString deviceName_, remoteName_, pairingCode_;
    bool codePairing_ = false, outgoing_ = false, codeVerified_ = false;
    int codeAttempts_ = 0;
    quint16 servicePort_ = 0;
    bool pairing_ = false, encrypted_ = false, ready_ = false;
    bool helloReceived_ = false, localApproved_ = false, remoteApproved_ = false, failed_ = false;
};

class LanListener : public QTcpServer {
public:
    LanListener(Library &, bool pairing, QObject *parent = nullptr);
    std::function<bool(LanConnection *)> onConnection;
protected:
    void incomingConnection(qintptr descriptor) override;
private:
    Library &library_;
    bool pairing_;
};

} // namespace novelty
