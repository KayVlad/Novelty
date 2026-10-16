#pragma once
#include <QtCore/qdatetime.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qmap.h>
#include <QtCore/qtimezone.h>
#include <functional>
#include <memory>
class QFile;
namespace novelty {
// No extrapolation: only intervals bounded by compositor observations are measured.
struct FocusLedger {
    qint64 foregroundMs = 0, observedMs = 0;
    QMap<QDate, qint64> days;
    void observe(qint64 monotonic, qint64 boottime, const QDateTime &utc,
                 bool known, bool foreground, const QTimeZone &zone = QTimeZone::systemTimeZone());
    void invalidate();
private:
    qint64 lastMono_ = -1, lastBoot_ = -1;
    QDateTime lastUtc_;
    bool known_ = false, foreground_ = false;
};
// Walk live Linux ancestry, including children adopted by the detached subreaper.
// Start ticks prevent an old window/PID from being attributed to a reused PID.
QString supervisedWindowProcess(qint64 windowPid, qint64 monitorPid);
class WindowOwners {
public:
    bool verify(const QString &windowId, qint64 pid, const QString &startTicks);
private:
    QMap<QString, QPair<qint64, QString>> owners_;
};
bool gamescopeSession();
class FocusMonitor {
public:
    using Observation = std::function<void(bool, bool)>;
    FocusMonitor(QFile &log, Observation observation);
    ~FocusMonitor();
    void poll();
    QString status() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
