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
    void fail(const QString &);
    Library &library_;
    QSslSocket socket_;
    QTimer timeout_;
    QByteArray input_, localNonce_, remoteNonce_;
    QString peerFingerprint_, pendingPeerName_;
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
