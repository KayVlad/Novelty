#pragma once

#include "core.h"
#include <QtCore/qurl.h>

namespace novelty {

struct SaveServerConfig {
    bool enabled = false;
    QUrl url;
    QByteArray token;
    QString serverId;
};

// Credentials live beside the application in an owner-only profile directory,
// never in the Games library, peer metadata or exported diagnostics.
SaveServerConfig loadSaveServerConfig(const Library &library);
void saveSaveServerConfig(const Library &library, const SaveServerConfig &config);

} // namespace novelty
