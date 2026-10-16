#pragma once
#include <QtCore/qdatetime.h>
#include <QtCore/qfile.h>
#include <QtCore/qlist.h>
#include <QtCore/qstring.h>

namespace novelty {
struct PlaySession {
    QString id, state;
    QDateTime started, ended, checkpoint;
    qint64 durationMs = 0;
};
struct PlaytimeStats {
    qint64 totalMs = 0, sessions = 0, currentMs = 0;
    QDateTime lastPlayed;
    bool active = false;
    QList<PlaySession> recent;
};
// Own short-lived SQLite connections: these never use Library's inherited connection.
void recoverPlaytime(const QString &libraryRoot, const QString &gameId = {});
PlaytimeStats readPlaytime(const QString &libraryRoot, const QString &gameId, int recentLimit = 8);
QString formatTrackedTime(qint64 milliseconds, bool seconds = false);
QString formatLastPlayed(const QDateTime &);

// Used only in the detached process monitor. Statistics errors are logged and
// retried, never propagated into launch/save recovery. No background service.
class PlaytimeSession {
public:
    PlaytimeSession(QString libraryRoot, QString gameId, QFile &log);
    void start() noexcept;
    void checkpoint() noexcept;
    void complete() noexcept;
private:
    void persist(bool completed) noexcept;
    QString root_, game_, id_, boot_, ownerStart_;
    QDateTime started_;
    qint64 startMs_ = -1, lastAttemptMs_ = -1, owner_ = 0;
    QFile &log_;
    bool warned_ = false;
};
}
