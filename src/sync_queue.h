#pragma once

#include "core.h"

namespace novelty {

struct PendingPeerSync {
    QString fingerprint;
    QString gameId;
    QString snapshotId;
    QString queuedAt;
};

QList<PendingPeerSync> pendingPeerSync(const Library &);
void queuePeerSync(const Library &, const QString &gameId, const QString &snapshotId);
// An acknowledgement for an older head must not clear a newer queued snapshot.
void acknowledgePeerSync(const Library &, const QString &fingerprint,
                         const QString &gameId, const QString &snapshotId);

} // namespace novelty
