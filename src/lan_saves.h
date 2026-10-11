#pragma once

#include "lan_connection.h"
#include "saves.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qtimer.h>

namespace novelty {

class LanSaveExchange : public QObject {
public:
    struct Entry { QString path; bool directory = false; qint64 size = 0; QString sha256; };
    LanSaveExchange(Library &, LanConnection &, QObject *parent = nullptr);
    ~LanSaveExchange() override;
    void locate(const SaveConfig &);
    void fetch(const QString &localGameId, const QString &remoteProfileGameId,
               const QString &snapshotId);
    void offer(const QString &gameId, const QString &snapshotId);
    void acknowledgeOffer(const QString &offerId, const QString &snapshotId);
    void deferOffer(const QString &offerId, const QString &reason);
    std::function<void(const QJsonArray &)> onMatches;
    std::function<void(const SaveImportResult &)> onImported;
    std::function<void(qint64, qint64)> onProgress;
    std::function<void(const QString &)> onError;
    std::function<void(const QJsonObject &)> onOffer;
    std::function<void(const QString &gameId, const QString &snapshotId)> onAcknowledged;
    std::function<void(const QString &)> onDeferred;
    std::function<void(const QJsonObject &)> onOtherMessage;
    std::function<void()> onTransportDrain;
private:
    void handle(const QJsonObject &);
    void sendError(const QString &requestId, const QString &message);
    void answerLocate(const QJsonObject &);
    void answerFetch(const QJsonObject &);
    void pump();
    void receiveBegin(const QJsonObject &);
    void receiveEntry(const QJsonObject &);
    void receiveChunk(const QJsonObject &);
    void receiveFileEnd(const QJsonObject &);
    void receiveEnd(const QJsonObject &);
    void clearIncoming();
    Library &library_;
    LanConnection &connection_;
    QString locateRequest_, fetchRequest_, localGameId_, remoteGameId_, requestedSnapshot_;
    QString outgoingOfferId_, offeredGameId_, offeredSnapshot_;
    QString stage_, incomingPath_, outgoingFolder_, outgoingRequest_;
    QFile incomingFile_, outgoingFile_;
    QCryptographicHash incomingHash_{QCryptographicHash::Sha256};
    QTimer transferTimeout_;
    QList<Entry> outgoingEntries_;
    qsizetype outgoingIndex_ = 0;
    qint64 outgoingTotal_ = 0, incomingTotal_ = 0, incomingReceived_ = 0, incomingFileSize_ = 0;
    qint64 incomingFileReceived_ = 0;
    int expectedEntries_ = 0, receivedEntries_ = 0;
    bool outgoingActive_ = false, incomingActive_ = false;
};

} // namespace novelty
