#pragma once

#include "core.h"
#include "save_server_config.h"
#include "save_server_client.h"
#include <QtCore/qjsonobject.h>

namespace novelty {

enum class ServerSyncStatus { Synced, Pending, Conflict, Unavailable };
struct ServerSyncResult { ServerSyncStatus status; QString detail; };
struct ServerConflictPreview { RemoteSaveRevision remote; QJsonObject verifiedLive; };

// The detached launch supervisor calls these while holding launch.lock.
// Network errors leave local Play available and never advance acceptance.
ServerSyncResult syncServerBeforePlay(Library &, const QString &gameId, const SaveServerConfig &);
ServerSyncResult syncServerAfterPlay(Library &, const QString &gameId, const SaveServerConfig &);
ServerSyncResult retryPendingServerSave(Library &, const QString &gameId, const SaveServerConfig &);
ServerConflictPreview reviewServerSaveConflict(const Library &, const QString &gameId,
                                                const SaveServerConfig &);
void resolveServerSaveConflict(Library &, const QString &gameId, const SaveServerConfig &,
                               const ServerConflictPreview &, bool chooseLocal);

} // namespace novelty
