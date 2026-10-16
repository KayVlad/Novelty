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
    bool hasTable() {
        QSqlQuery query(db);
        if (!query.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='play_sessions'")) fail(query.lastError().text());
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
PlaytimeStats readPlaytime(const QString &root, const QString &id, int recentLimit) {
    if (recentLimit < 0 || recentLimit > 100) fail("Invalid playtime history limit.");
    recoverPlaytime(root, id);
    Database database(root, false);
    PlaytimeStats result;
    if (!database.hasTable()) return result;
    const auto boot = currentBoot(); const auto now = monotonicMs();
    QSqlQuery query(database.db);
    query.prepare("SELECT session_id,state,started_utc,elapsed_ms,ended_utc,checkpoint_utc,"
                  "start_monotonic_ms,boot_id,owner_pid,owner_start_ticks FROM play_sessions "
                  "WHERE game_id=? ORDER BY started_utc DESC,session_id DESC");
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
        result.totalMs += session.durationMs; ++result.sessions;
        if (session.started > result.lastPlayed) result.lastPlayed = session.started;
        if (result.recent.size() < recentLimit) result.recent.append(session);
    }
    return result;
}
PlaytimeSession::PlaytimeSession(QString root, QString game, QFile &log)
    : root_(std::move(root)), game_(std::move(game)), log_(log) {}
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
    try { if (monotonicMs() - lastAttemptMs_ >= checkpointMs) persist(false); }
    catch (...) { /* Optional statistics must not interrupt descendant supervision. */ }
}
void PlaytimeSession::complete() noexcept { if (startMs_ >= 0) { persist(true); startMs_ = -1; } }
void PlaytimeSession::persist(bool completed) noexcept {
    try {
        const auto now = monotonicMs(); lastAttemptMs_ = now;
        const auto stamp = utc(QDateTime::currentDateTimeUtc());
        Database database(root_, true); database.ensureTable();
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
