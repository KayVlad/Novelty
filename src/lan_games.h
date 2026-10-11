#pragma once

#include "lan_connection.h"
#include "lan_saves.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qfile.h>
#include <QtCore/qhash.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qset.h>
#include <QtCore/qtimer.h>
#include <future>
#include <thread>

namespace novelty {

// A trusted peer can list portable game folders and pull one folder into a
// profile-local staging directory. Complete files survive a disconnected peer.
class LanGameExchange : public QObject {
public:
    struct Entry { QString path; bool directory = false; qint64 size = 0; QString sha256; };
    LanGameExchange(Library &, LanConnection &, LanSaveExchange &, QObject *parent = nullptr);
    ~LanGameExchange() override;
    void catalogue();
    void install(const QJsonObject &descriptor);
    std::function<void(const QJsonArray &)> onCatalogue;
    std::function<void(qint64, qint64)> onProgress;
    std::function<void(const QString &)> onInstalled;
    std::function<void(const QString &)> onError;
private:
    void handle(const QJsonObject &);
    void sendCatalogue(const QJsonObject &);
    void sendManifest(const QJsonObject &);
    void sendFile(const QJsonObject &);
    void pump();
    void receiveBegin(const QJsonObject &);
    void receiveEntry(const QJsonObject &);
    void receiveManifestEnd(const QJsonObject &);
    void requestNext();
    void receiveFileBegin(const QJsonObject &);
    void receiveChunk(const QJsonObject &);
    void receiveFileEnd(const QJsonObject &);
    void finalize();
    void fail(const QString &);
    Library &library_;
    LanConnection &connection_;
    LanSaveExchange &router_;
    QTimer timeout_, manifestPoll_, resumePoll_;
    QString requestId_, remoteGameId_, stage_, sendingFolder_, sendingRequest_;
    QJsonObject descriptor_;
    QJsonObject sendingDescriptor_;
    QList<Entry> entries_, sendingEntries_;
    QSet<QString> receivedPaths_;
    QSet<QString> verifiedStagedFiles_;
    QHash<QString, Entry> sendingMap_;
    int expectedEntries_ = 0, receivedEntries_ = 0, nextEntry_ = 0;
    int sendingIndex_ = 0;
    qint64 totalBytes_ = 0, completeBytes_ = 0, currentBytes_ = 0, sendingBytes_ = 0, sendingCurrentBytes_ = 0;
    qint64 lastManifestHeartbeat_ = 0;
    QString currentPath_, sendingPath_;
    QFile currentFile_, sendingFile_;
    QCryptographicHash currentHash_{QCryptographicHash::Sha256};
    QCryptographicHash sendingHash_{QCryptographicHash::Sha256};
    bool receivingManifest_ = false, sendingManifest_ = false, sendingFileActive_ = false;
    std::jthread manifestWorker_;
    std::future<QPair<QList<Entry>, qint64>> manifestFuture_;
    std::jthread resumeWorker_;
    std::future<QSet<QString>> resumeFuture_;
};

// Validate the complete untrusted manifest before creating or fetching files.
void validateLanGameManifest(const QJsonObject &descriptor,
                             const QList<LanGameExchange::Entry> &entries, qint64 totalBytes);

} // namespace novelty
