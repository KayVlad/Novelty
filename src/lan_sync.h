#pragma once

#include "core.h"
#include <QtCore/qstringlist.h>

namespace novelty {

struct PeerRefreshResult {
    int peersChecked = 0;
    int snapshotsImported = 0;
    QString latestSnapshotId; // Set when a single selected peer advertises a complete state.
    QStringList issues;
};

// Checks only trusted peers with a remembered endpoint and an explicit or shared
// profile-game link. Offline peers leave the local applied save untouched.
PeerRefreshResult refreshLinkedPeerSaves(Library &, const QString &gameId,
                                         int connectTimeoutMs = 5000,
                                         const QString &selectedFingerprint = {});

} // namespace novelty
