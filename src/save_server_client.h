#pragma once

#include <QtCore/qstring.h>
#include <QtCore/qurl.h>
#include <stdexcept>

namespace novelty {

struct RemoteSaveRevision {
    QString serverId;
    int revision = 0;
    QString snapshotId;
    int files = 0;
};
struct RemoteRevisionConflict : std::runtime_error {
    int currentRevision;
    explicit RemoteRevisionConflict(int revision);
};

// Synchronous operations intended for the detached launch supervisor or a worker.
// Construction validates the endpoint before any credential is sent.
class SaveServerClient {
public:
    SaveServerClient(const QUrl &baseUrl, const QByteArray &bearerToken);
    QString identity(int timeoutMs = 5000) const;
    RemoteSaveRevision current(const QString &gameKey, int timeoutMs = 5000) const;
    RemoteSaveRevision download(const QString &gameKey, const QString &destinationDirectory,
                                int timeoutMs = 30000) const;
    RemoteSaveRevision upload(const QString &gameKey, int expectedRevision,
                              const QString &bundleDirectory, int timeoutMs = 30000,
                              const QString &reconcileServerId = {}) const;
private:
    QUrl endpoint(const QString &gameKey) const;
    QUrl base_;
    QByteArray token_;
};

} // namespace novelty
