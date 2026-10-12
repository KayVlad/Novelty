#pragma once

#include "core.h"
#include "save_server_client.h"
#include <QtCore/qjsonobject.h>

namespace novelty {

struct AcceptedServerSave {
    bool present = false;
    QString gameKey;
    QString serverId;
    int revision = 0;
    QString snapshotId;
    QJsonObject liveFingerprint;
};
enum class ServerSaveAction { Play, Download, Upload, AcceptEmpty, Conflict };

QString defaultServerGameKey(const Library &, const QString &gameId);
AcceptedServerSave loadAcceptedServerSave(const Library &, const QString &gameId);
void saveAcceptedServerSave(const Library &, const QString &gameId, const AcceptedServerSave &);
ServerSaveAction chooseServerSaveAction(const AcceptedServerSave &, const RemoteSaveRevision &,
                                        const QJsonObject &verifiedLive);

} // namespace novelty
