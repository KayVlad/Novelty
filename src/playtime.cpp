#include "playtime.h"
#include <QtCore/qfileinfo.h>
#include <QtCore/qlocale.h>
#include <QtCore/quuid.h>
#include <QtCore/qurl.h>
#include <QtSql/qsqldatabase.h>
#include <QtSql/qsqlquery.h>
#include <QtSql/qsqlerror.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <time.h>
#include <unistd.h>

namespace novelty {
namespace {
constexpr qint64 checkpointMs = 30000;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString currentBoot() {
    QFile file("/proc/sys/kernel/random/boot_id");
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read the playtime boot identity.");
    const auto boot = QString::fromUtf8(file.readAll()).trimmed();
    if (QUuid(boot).isNull()) fail("Invalid playtime boot identity.");
    return boot;
}
qint64 monotonicMs() {
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC, &time) != 0) fail("Cannot read the playtime monotonic clock.");
    return qint64(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
}
struct Process { QString start; bool dead = false; };
Process processIdentity(qint64 pid) {
    if (pid <= 0) return {{}, true};
    const auto path = QByteArray("/proc/") + QByteArray::number(pid) + "/stat";
    const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {{}, errno == ENOENT || errno == ESRCH};
    char buffer[8192]; const auto count = ::read(fd, buffer, sizeof(buffer)); ::close(fd);
    if (count <= 0) return {}; // Unknown: do not declare a potentially live owner dead.
    const auto bytes = QByteArray(buffer, count);
    const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).simplified().split(' ');
    if (fields.size() < 20 || fields[19].isEmpty()) return {};
    return {QString::fromLatin1(fields[19]), fields[0] == "Z" || fields[0] == "X"};
}
enum class Owner { Live, Dead, Unknown };
Owner ownerState(const QString &boot, qint64 pid, const QString &start, const QString &thisBoot) {
    if (boot != thisBoot) return Owner::Dead;
    const auto process = processIdentity(pid);
    if (process.dead || (!process.start.isEmpty() && process.start != start)) return Owner::Dead;
    return process.start.isEmpty() ? Owner::Unknown : Owner::Live;
}
void execute(QSqlQuery &query) { if (!query.exec()) fail("Playtime: " + query.lastError().text()); }
class Database {
public:
    explicit Database(const QString &root, bool writable) : name_("playtime-" + QUuid::createUuid().toString()) {
        auto url = QUrl::fromLocalFile(root + "/.novelty/library.sqlite");
        url.setQuery(writable ? "mode=rw" : "mode=ro"); // Never create a replacement library if media disappears.
        db = QSqlDatabase::addDatabase("QSQLITE", name_);
        db.setDatabaseName(QString::fromUtf8(url.toEncoded()));
        db.setConnectOptions("QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=200");
        try {
            if (!db.open()) fail("Playtime: " + db.lastError().text());
            if (writable) { QSqlQuery query(db); if (!query.exec("PRAGMA synchronous=FULL")) fail(query.lastError().text()); }
        } catch (...) { close(); throw; }
    }
    ~Database() { close(); }
    bool hasTable(const QString &name = "play_sessions") {
        QSqlQuery query(db);
        query.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
        query.addBindValue(name); execute(query);
        return query.next();
    }
    void ensureTable() {
        QSqlQuery query(db);
        if (!query.exec("CREATE TABLE IF NOT EXISTS play_sessions ("
            "session_id TEXT PRIMARY KEY, game_id TEXT NOT NULL, started_utc TEXT NOT NULL, "
            "elapsed_ms INTEGER NOT NULL CHECK(elapsed_ms>=0), ended_utc TEXT, "
            "state TEXT NOT NULL CHECK(state IN ('active','completed','interrupted')), "
            "checkpoint_utc TEXT NOT NULL, start_monotonic_ms INTEGER NOT NULL, "
            "boot_id TEXT NOT NULL, owner_pid INTEGER NOT NULL, owner_start_ticks TEXT NOT NULL)")) fail(query.lastError().text());
        if (!query.exec("CREATE INDEX IF NOT EXISTS play_sessions_game ON play_sessions(game_id,started_utc DESC)")) fail(query.lastError().text());
    }
    void ensureFocusTables() {
        QSqlQuery query(db);
        if (!query.exec("CREATE TABLE IF NOT EXISTS play_focus (session_id TEXT PRIMARY KEY, "
            "foreground_ms INTEGER NOT NULL CHECK(foreground_ms>=0), observed_ms INTEGER NOT NULL CHECK(observed_ms>=foreground_ms), "
            "status TEXT NOT NULL)")) fail(query.lastError().text());
        if (!query.exec("CREATE TABLE IF NOT EXISTS play_focus_days (session_id TEXT NOT NULL, local_day TEXT NOT NULL, "
            "elapsed_ms INTEGER NOT NULL CHECK(elapsed_ms>=0), PRIMARY KEY(session_id,local_day))")) fail(query.lastError().text());
    }
    QSqlDatabase db;
private:
    void close() { db.close(); db = {}; QSqlDatabase::removeDatabase(name_); }
    QString name_;
};
QString utc(const QDateTime &date) { return date.toUTC().toString(Qt::ISODateWithMs); }
}

void recoverPlaytime(const QString &root, const QString &gameId) {
    QList<QString> stale;
    {
        Database database(root, false);
        if (!database.hasTable()) return;
        const auto boot = currentBoot();
        QSqlQuery query(database.db);
        query.prepare("SELECT session_id,boot_id,owner_pid,owner_start_ticks FROM play_sessions WHERE state='active'" +
                      (gameId.isEmpty() ? QString() : " AND game_id=?"));
        if (!gameId.isEmpty()) query.addBindValue(gameId);
        execute(query);
        while (query.next())
            if (ownerState(query.value(1).toString(), query.value(2).toLongLong(), query.value(3).toString(), boot) == Owner::Dead)
                stale.append(query.value(0).toString());
    }
    if (stale.isEmpty()) return;
    Database database(root, true);
    for (const auto &id : stale) {
        QSqlQuery query(database.db);
        // Preserve only the last committed elapsed duration. No crash-to-startup gap,
        // and no invented end timestamp. Repeating recovery changes nothing.
        query.prepare("UPDATE play_sessions SET state='interrupted' WHERE session_id=? AND state='active'");
        query.addBindValue(id); execute(query);
    }
}
PlaytimeStats readPlaytime(const QString &root, const QString &id, int recentLimit, QDate today) {
    if (recentLimit < 0 || recentLimit > 100) fail("Invalid playtime history limit.");
    recoverPlaytime(root, id);
    Database database(root, false);
    PlaytimeStats result;
    for (int i = 29; i >= 0; --i) result.days.append({today.addDays(-i), 0, 0});
    for (int i = 364; i >= 0; --i) result.heatmap.append({today.addDays(-i), 0, 0});
    if (!database.hasTable()) return result;
    if (!database.db.transaction()) fail(database.db.lastError().text());
    const bool hasFocus = database.hasTable("play_focus");
    const auto boot = currentBoot(); const auto now = monotonicMs();
    QSqlQuery query(database.db);
    query.prepare("SELECT s.session_id,s.state,s.started_utc,s.elapsed_ms,s.ended_utc,s.checkpoint_utc,"
                  "s.start_monotonic_ms,s.boot_id,s.owner_pid,s.owner_start_ticks," +
                  (hasFocus ? QString("f.foreground_ms,f.observed_ms,f.status FROM play_sessions s LEFT JOIN play_focus f ON f.session_id=s.session_id ") :
                              QString("NULL,NULL,NULL FROM play_sessions s ")) +
                  "WHERE s.game_id=? ORDER BY s.started_utc DESC,s.session_id DESC");
    query.addBindValue(id); execute(query);
    while (query.next()) {
        PlaySession session{query.value(0).toString(), query.value(1).toString(),
            QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs),
            QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs),
            QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs), query.value(3).toLongLong()};
        if (session.durationMs < 0) fail("Invalid recorded playtime duration.");
        if (session.state == "active" && ownerState(query.value(7).toString(), query.value(8).toLongLong(),
                                                    query.value(9).toString(), boot) == Owner::Live) {
            session.durationMs = std::max(session.durationMs, now - query.value(6).toLongLong());
            result.active = true; result.currentMs += session.durationMs;
        }
        session.foregroundRecorded = !query.value(10).isNull();
        session.foregroundMs = session.foregroundRecorded ? query.value(10).toLongLong() : 0;
        if (session.foregroundMs < 0 || session.foregroundMs > session.durationMs) fail("Invalid foreground playtime duration.");
        result.foregroundMs += session.foregroundMs;
        if (session.state == "active") {
            result.currentForegroundMs += session.foregroundMs;
            result.focusStatus = query.value(12).toString();
        }
        if (!session.foregroundRecorded || query.value(11).toLongLong() < session.durationMs) result.incompleteForeground = true;
        if (session.durationMs > 300000 && (!result.firstPlay.isValid() || session.started < result.firstPlay)) result.firstPlay = session.started;
        result.totalMs += session.durationMs; ++result.sessions;
        if (session.started > result.lastPlayed) result.lastPlayed = session.started;
        if (result.recent.size() < recentLimit) result.recent.append(session);
    }
    query.finish();
    if (database.hasTable("play_focus_days")) {
        query.prepare("SELECT d.local_day,SUM(d.elapsed_ms) FROM play_focus_days d JOIN play_sessions s ON s.session_id=d.session_id "
                      "WHERE s.game_id=? GROUP BY d.local_day ORDER BY d.local_day");
        query.addBindValue(id); execute(query);
        QMap<QDate, qint64> days;
        while (query.next()) days[QDate::fromString(query.value(0).toString(), Qt::ISODate)] = query.value(1).toLongLong();
        for (auto &day : result.heatmap) day.foregroundMs = days.value(day.day);
        qint64 cumulative = 0;
        auto entry = days.cbegin();
        for (auto &day : result.days) {
            while (entry != days.cend() && entry.key() <= day.day) { cumulative += entry.value(); ++entry; }
            day.foregroundMs = days.value(day.day); day.cumulativeMs = cumulative;
        }
    }
    query.finish();
    if (!database.db.commit()) fail(database.db.lastError().text());
    return result;
}
PlaytimeSession::PlaytimeSession(QString root, QString game, QFile &log)
    : root_(std::move(root)), game_(std::move(game)), log_(log) {}
PlaytimeSession::~PlaytimeSession() = default;
void PlaytimeSession::prepareFocus() noexcept {
    try {
        monitor_ = std::make_unique<FocusMonitor>(log_, [this](bool known, bool foreground) {
            if (startMs_ < 0) return;
            timespec boot{};
            if (::clock_gettime(CLOCK_BOOTTIME, &boot) != 0) { focus_.invalidate(); return; }
            focus_.observe(monotonicMs(), qint64(boot.tv_sec)*1000 + boot.tv_nsec/1000000,
                           QDateTime::currentDateTimeUtc(), known, foreground);
        });
    } catch (const std::exception &error) { log_.write("Foreground tracking unavailable: "); log_.write(error.what()); log_.write("\n"); }
}
void PlaytimeSession::start() noexcept {
    if (startMs_ >= 0) return;
    try {
        boot_ = currentBoot(); owner_ = ::getpid(); ownerStart_ = processIdentity(owner_).start;
        if (ownerStart_.isEmpty()) fail("Cannot establish the playtime monitor identity.");
        id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        started_ = QDateTime::currentDateTimeUtc(); startMs_ = monotonicMs();
        persist(false);
    } catch (const std::exception &error) { log_.write("Playtime unavailable: "); log_.write(error.what()); log_.write("\n"); }
}
void PlaytimeSession::checkpoint() noexcept {
    if (startMs_ < 0) return;
    try {
        if (monitor_) monitor_->poll();
        if (monotonicMs() - lastAttemptMs_ >= checkpointMs) persist(false);
    }
    catch (...) { /* Optional statistics must not interrupt descendant supervision. */ }
}
void PlaytimeSession::complete() noexcept {
    if (startMs_ >= 0) { persist(true); startMs_ = -1; }
    monitor_.reset();
}
void PlaytimeSession::persist(bool completed) noexcept {
    try {
        const auto now = monotonicMs(); lastAttemptMs_ = now;
        const auto stamp = utc(QDateTime::currentDateTimeUtc());
        Database database(root_, true); database.ensureTable();
        database.ensureFocusTables();
        if (!database.db.transaction()) fail(database.db.lastError().text());
        QSqlQuery query(database.db);
        query.prepare("INSERT INTO play_sessions(session_id,game_id,started_utc,elapsed_ms,ended_utc,state,"
                      "checkpoint_utc,start_monotonic_ms,boot_id,owner_pid,owner_start_ticks) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                      "ON CONFLICT(session_id) DO UPDATE SET elapsed_ms=MAX(play_sessions.elapsed_ms,excluded.elapsed_ms),"
                      "ended_utc=excluded.ended_utc,state=excluded.state,checkpoint_utc=excluded.checkpoint_utc "
                      "WHERE play_sessions.state='active'");
        query.addBindValue(id_); query.addBindValue(game_); query.addBindValue(utc(started_));
        query.addBindValue(std::max<qint64>(0, now - startMs_));
        query.addBindValue(completed ? stamp : QVariant()); query.addBindValue(completed ? "completed" : "active");
        query.addBindValue(stamp); query.addBindValue(startMs_); query.addBindValue(boot_);
        query.addBindValue(owner_); query.addBindValue(ownerStart_); execute(query);
        // Both counters and all midnight slices are one FULL-synchronous transaction.
        query.prepare("INSERT INTO play_focus VALUES(?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET "
                      "foreground_ms=excluded.foreground_ms,observed_ms=excluded.observed_ms,status=excluded.status");
        query.addBindValue(id_); query.addBindValue(focus_.foregroundMs); query.addBindValue(focus_.observedMs);
        query.addBindValue(monitor_ ? monitor_->status() : "Foreground state not observed"); execute(query);
        for (auto day = focus_.days.cbegin(); day != focus_.days.cend(); ++day) {
            query.prepare("INSERT INTO play_focus_days VALUES(?,?,?) ON CONFLICT(session_id,local_day) DO UPDATE SET elapsed_ms=excluded.elapsed_ms");
            query.addBindValue(id_); query.addBindValue(day.key().toString(Qt::ISODate)); query.addBindValue(day.value()); execute(query);
        }
        if (!database.db.commit()) fail(database.db.lastError().text());
    } catch (const std::exception &error) {
        if (!warned_) { log_.write("Playtime recording needs attention: "); log_.write(error.what()); log_.write("\n"); log_.flush(); warned_ = true; }
    }
}
QString formatTrackedTime(qint64 ms, bool seconds) {
    const auto total = std::max<qint64>(0, ms) / 1000;
    const auto hours = total / 3600, minutes = total / 60 % 60;
    QString text = hours ? QString::number(hours) + "h " + QString::number(minutes) + "m" : QString::number(minutes) + "m";
    if (seconds) text += " " + QString::number(total % 60) + "s";
    else if (total < 60 && ms > 0) text = "<1m";
    return text;
}
QString formatLastPlayed(const QDateTime &time) {
    if (!time.isValid()) return "Never";
    const auto date = time.toLocalTime().date();
    const auto days = date.daysTo(QDate::currentDate());
    if (days == 0) return "Today";
    if (days == 1) return "Yesterday";
    return QLocale().toString(date, QLocale::ShortFormat);
}
}
