#include "playtime.h"
#include "core.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qdir.h>
#include <QtCore/qthread.h>
#include <QtSql/qsqlquery.h>
#include <QtSql/qsqlerror.h>
#include <QtCore/quuid.h>
#include <cstdio>
#include <signal.h>
#include <QtCore/qprocess.h>
#include <stdexcept>
using namespace novelty;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void sql(QSqlDatabase &db, const QString &text) { QSqlQuery q(db); if (!q.exec(text)) throw std::runtime_error(q.lastError().text().toStdString()); }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const QTimeZone zone("Europe/Dublin");
        auto utc = QDateTime::fromString("2026-01-03T23:59:59.500Z", Qt::ISODateWithMs);
        FocusLedger ledger;
        ledger.observe(0, 0, utc, true, true, zone);
        ledger.observe(1000, 1000, utc.addMSecs(1000), true, false, zone); // focus lost at receipt
        require(ledger.foregroundMs == 1000 && ledger.days[QDate(2026,1,3)] == 500 && ledger.days[QDate(2026,1,4)] == 500, "midnight split");
        ledger.observe(2000, 2000, utc.addMSecs(2000), true, false, zone); // minimized
        require(ledger.foregroundMs == 1000, "minimized counted");
        ledger.observe(3000, 3000, utc.addMSecs(3000), true, true, zone); // restored
        ledger.observe(4000, 4000, utc.addMSecs(4000), true, true, zone); // second associated window/fullscreen
        require(ledger.foregroundMs == 2000, "restore or multiple-window accounting");
        ledger.observe(5000, 65000, utc.addMSecs(65000), true, true, zone); // suspend; mono excludes sleep
        require(ledger.foregroundMs == 2000, "suspend boundary counted");
        ledger.observe(6000, 66000, utc.addMSecs(66000), true, true, zone);
        require(ledger.foregroundMs == 3000, "wake did not resume");
        ledger.observe(7000, 67000, utc.addMSecs(67000), false, false, zone);
        ledger.observe(8000, 68000, utc.addMSecs(68000), true, true, zone);
        require(ledger.foregroundMs == 3000, "unknown state inferred foreground");
        ledger.observe(10000, 70000, utc.addMSecs(70000), true, true, zone);
        require(ledger.foregroundMs == 3000, "bridge stall counted");
        ledger.observe(11000, 71000, utc.addDays(-2), true, true, zone);
        require(ledger.foregroundMs == 4000, "wall clock changed duration");
        qint64 sum = 0; for (auto value : ledger.days) sum += value;
        require(sum == ledger.foregroundMs && ledger.observedMs >= ledger.foregroundMs, "daily counters double counted");
        // DST changes occur without a midnight at UTC; use the actual local midnight.
        FocusLedger summer; utc = QDateTime::fromString("2026-07-03T22:59:59.750Z", Qt::ISODateWithMs);
        summer.observe(0,0,utc,true,true,zone); summer.observe(1000,1000,utc.addMSecs(1000),true,true,zone);
        require(summer.days[QDate(2026,7,3)] == 250 && summer.days[QDate(2026,7,4)] == 750, "local summer midnight split");
        WindowOwners owners;
        require(owners.verify("window-a", 100, "10") && owners.verify("window-a",100,"10"), "stable window binding");
        require(!owners.verify("window-a",100,"11") && !owners.verify("window-a",100,"11"), "PID reuse inherited an old window");
        require(owners.verify("window-b",100,"11") && !owners.verify("",100,"11") && !owners.verify("window-c",100,{}), "new/unknown window binding");
        QTemporaryDir temp; const auto root = temp.path()+"/Library"; QDir().mkpath(root); Library library(root);
        auto db = QSqlDatabase::addDatabase("QSQLITE", "focus-test"); db.setDatabaseName(root+"/.novelty/library.sqlite"); require(db.open(),"open fixture database");
        sql(db, "CREATE TABLE play_sessions(session_id TEXT PRIMARY KEY,game_id TEXT,started_utc TEXT,elapsed_ms INTEGER,ended_utc TEXT,state TEXT,checkpoint_utc TEXT,start_monotonic_ms INTEGER,boot_id TEXT,owner_pid INTEGER,owner_start_ticks TEXT)");
        sql(db, "INSERT INTO play_sessions VALUES('old','game','2026-01-01T23:00:00.000Z',300000,'2026-01-01T23:05:00.000Z','completed','2026-01-01T23:05:00.000Z',0,'old',0,'0')");
        auto stats = readPlaytime(root,"game",8,QDate(2026,3,1));
        require(stats.totalMs == 300000 && stats.foregroundMs == 0 && stats.incompleteForeground && !stats.firstPlay.isValid(), "historical records fabricated foreground or first play");
        require(stats.days.size() == 30 && stats.heatmap.size() == 365 && stats.days.first().day == QDate(2026,1,31), "calendar range");
        sql(db, "INSERT INTO play_sessions VALUES('new','game','2026-01-02T12:00:00.000Z',7200000,'2026-01-02T14:00:00.000Z','completed','2026-01-02T14:00:00.000Z',0,'old',0,'0')");
        sql(db, "CREATE TABLE play_focus(session_id TEXT PRIMARY KEY,foreground_ms INTEGER,observed_ms INTEGER,status TEXT)");
        sql(db, "CREATE TABLE play_focus_days(session_id TEXT,local_day TEXT,elapsed_ms INTEGER,PRIMARY KEY(session_id,local_day))");
        sql(db, "INSERT INTO play_focus VALUES('new',5400000,7200000,'KWin')");
        sql(db, "INSERT INTO play_focus_days VALUES('new','2026-01-02',3600000),('new','2026-02-15',1800000)");
        stats = readPlaytime(root,"game",8,QDate(2026,3,1));
        require(stats.firstPlay.date() == QDate(2026,1,2), "first play must be strictly over five minutes");
        require(stats.foregroundMs == 5400000 && stats.totalMs == 7500000, "independent totals");
        require(stats.days.first().cumulativeMs == 3600000 && stats.days.last().cumulativeMs == 5400000, "cumulative omitted pre-range history");
        int nonzero = 0; for (const auto &day : stats.days) if (day.foregroundMs) ++nonzero;
        require(nonzero == 1 && stats.days.last().foregroundMs == 0, "zero days missing");
        const auto repeated = readPlaytime(root,"game",8,QDate(2026,3,1));
        require(repeated.foregroundMs == stats.foregroundMs, "reading counted twice");
        QProcess child; child.start("/bin/sh", {"-c", "sleep 10 & echo $!; wait"}); require(child.waitForStarted(),"child process");
        child.waitForReadyRead(1000); const auto grandchild = child.readAllStandardOutput().trimmed().toLongLong();
        require(!supervisedWindowProcess(child.processId(),app.applicationPid()).isEmpty(), "native process association");
        require(grandchild > 1 && !supervisedWindowProcess(grandchild,app.applicationPid()).isEmpty(), "Proton-style child association");
        require(supervisedWindowProcess(app.applicationPid(),app.applicationPid()).isEmpty() && supervisedWindowProcess(1,app.applicationPid()).isEmpty(), "unrelated PID associated");
        ::kill(grandchild, SIGTERM); child.terminate(); child.waitForFinished();
        std::puts("PASS: independent counters, focus/minimize/restore, suspend, unknown/stalled state, wall clocks, local midnight, history, first play, daily cumulative, zero days, heatmap and native/child ancestry");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr,"FAIL: %s\n", error.what()); return 1; }
}
