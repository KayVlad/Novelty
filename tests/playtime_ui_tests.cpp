#include "window.h"
#include "playtime.h"
#include "playtime_chart.h"
#include <QtWidgets/qtabwidget.h>
#include "app_settings.h"
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qlistwidget.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qthread.h>
#include <cstdio>
#include <stdexcept>
using namespace novelty;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void file(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile output(path);
    require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(), "fixture file");
}
bool waitFor(const std::function<bool()> &predicate, int limit = 7000) {
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < limit) { QCoreApplication::processEvents(); if (predicate()) return true; QThread::msleep(10); }
    return false;
}
QString label(Window &window, const char *name) {
    const auto field = window.findChild<QLabel *>(name);
    require(field, "missing playtime label"); return field->text();
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        require(argc == 2, "launcher argument"); QTemporaryDir temp;
        qputenv("NOVELTY_TEST_APP_DATA_DIR", QFile::encodeName(temp.path() + "/app"));
        setAppSetting("autoGeUpdates", false);
        const auto root = temp.path() + "/Games", executable = root + "/Game/play.sh";
        file(executable, "#!/bin/sh\nsleep 4\n");
        QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Library library(root); library.import({inspect(root + "/Game")}, {});
        auto game = library.games().first(); game.metadata = {{"expected_play_time", "12 hours"}}; library.save(game);
        {
            Window window(root, QString::fromLocal8Bit(argv[1])); window.show();
            require(waitFor([&] { return label(window, "gameLastPlayed") == "Never"; }), "new game last played");
            require(label(window, "gameTimePlayed") == "0m" && label(window, "metadataPlaytime") == "12 hours", "actual/expected playtime are distinct");
            window.launch();
            require(waitFor([&] { return readPlaytime(root, game.id).active; }), "GUI did not start a tracked session");
            const auto initial = label(window, "gameCurrentSession");
            require(waitFor([&] { const auto current = label(window, "gameCurrentSession"); return current != "—" && current != initial; }), "current session does not update live");
        } // Close the GUI while the existing detached monitor and real subprocess run.
        require(readPlaytime(root, game.id).active, "closing GUI stopped tracking");
        require(waitFor([&] { const auto stats = readPlaytime(root, game.id); return stats.sessions == 1 && !stats.active; }), "session did not complete after GUI shutdown");
        require(readPlaytime(root, game.id).totalMs >= 3900, "GUI shutdown lost session duration");
        {
            Window reopened(root, QString::fromLocal8Bit(argv[1])); reopened.show();
            require(waitFor([&] { return label(reopened, "gameLastPlayed") == "Today"; }), "reopened GUI did not read tracked history");
            require(label(reopened, "gameOpenedTime") == "<1m" && label(reopened, "gameTimePlayed") == "0m" && label(reopened, "metadataPlaytime") == "12 hours", "reopened totals or expected time changed");
            bool inspected = false;
            QTimer::singleShot(0, &reopened, [&] {
                auto dialog = reopened.findChild<QDialog *>("manageDialog");
                if (dialog) {
                    const auto count = dialog->findChild<QLabel *>("statisticsCount");
                    const auto history = dialog->findChild<QListWidget *>("statisticsHistory");
                    const auto tabs = dialog->findChild<QTabWidget *>("manageTabs");
                    const auto chart = dialog->findChild<QWidget *>("playtimeChart");
                    const auto heatmap = dialog->findChild<QWidget *>("playtimeHeatmap");
                    bool playtimeTab = false;
                    for (int i = 0; tabs && i < tabs->count(); ++i) if (tabs->tabText(i) == "Playtime") { tabs->setCurrentIndex(i); playtimeTab = true; }
                    inspected = playtimeTab && chart && heatmap && count && count->text() == "1" && history && history->count() == 1 && history->item(0)->text().contains("completed");
                    const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                    if (!preview.isEmpty()) { dialog->resize(1000,900); QCoreApplication::processEvents(); dialog->grab().save(preview+"/manage-playtime.png"); }
                    dialog->reject();
                }
            });
            reopened.manage(); require(inspected, "Manage statistics/history missing");
        }
        {
            PlaytimeChart chart; chart.resize(700,300);
            chart.setDays({{QDate(2026,1,1),3600000,360000000},{QDate(2026,1,2),0,360000000}});
            require(chart.dailyAxisHours() == 1 && chart.cumulativeAxisHours() == 100, "chart axes must scale independently");
            require(!chart.grab().isNull(), "native chart painting failed");
            PlaytimeHeatmap heatmap; heatmap.resize(800,160); heatmap.setDays(readPlaytime(root,game.id).heatmap);
            require(!heatmap.grab().isNull(), "native heatmap painting failed");
        }
        game = library.game(game.id); game.backend = "steam"; game.steamId = "123"; library.save(game);
        {
            Window steam(root, QString::fromLocal8Bit(argv[1])); steam.show();
            require(waitFor([&] { return label(steam, "gameCurrentSession") == "Unavailable for Steam"; }), "Steam tracking must be explicitly unavailable");
        }
        std::puts("PASS: live actual playtime, expected metadata, GUI shutdown/restart, Manage history and Steam availability");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
