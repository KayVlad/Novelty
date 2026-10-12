#pragma once

#include <QtCore/qstring.h>
#include <stdexcept>

namespace novelty {

struct ServerSaveRevision {
    QString serverId;
    int revision = 0;
    QString snapshotId;
    QString bundleDirectory;
    int files = 0;
};

struct ServerRevisionConflict : std::runtime_error {
    int currentRevision;
    explicit ServerRevisionConflict(int revision);
};

// Durable storage for one current complete save state per explicit server game key.
// The service transport authenticates callers before invoking this API.
class SaveServerStore {
public:
    explicit SaveServerStore(const QString &root);
    ServerSaveRevision current(const QString &gameKey) const;
    ServerSaveRevision put(const QString &gameKey, int expectedRevision,
                           const QString &bundleDirectory);
    QString serverId() const { return serverId_; }
    QString rootPath() const { return root_; }
private:
    QString root_, serverId_;
};

} // namespace novelty
