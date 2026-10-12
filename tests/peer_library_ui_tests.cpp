#include "window.h"
#include "lan_connection.h"
#include "metadata.h"
#include "saves.h"
#include "setup.h"
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qtableview.h>
#include <QtWidgets/qmenu.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qlabel.h>
#include <QtGui/qaction.h>
#include <QtGui/qpainter.h>
#include <QtGui/qevent.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qsettings.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qthread.h>
#include <QtNetwork/qhostaddress.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace novelty;
static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void file(const QString &path, const QByteArray &bytes = {}) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture directory");
    QFile f(path); require(f.open(QIODevice::WriteOnly), "fixture file"); f.write(bytes);
}
static bool waitFor(const std::function<bool()> &test, int limit = 10000) {
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < limit) { QCoreApplication::processEvents(); if (test()) return true; QThread::msleep(10); }
    return false;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) return 1;
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
    QCoreApplication::setOrganizationName("NoveltyPeerTests"); QCoreApplication::setApplicationName("NoveltyPeerTests");
    QTimer watchdog; watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (widget->isVisible())
            for (auto label : widget->findChildren<QLabel *>()) std::fprintf(stderr, "%s\n", qPrintable(label->text()));
        std::fprintf(stderr, "FAIL: peer library fixture timed out\n"); std::_Exit(2);
    });
    watchdog.start(25000);
    try {
        const auto sourceRoot = temp.path() + "/source", targetRoot = temp.path() + "/target";
        QDir().mkpath(sourceRoot); QDir().mkpath(targetRoot);
        Library source(sourceRoot), target(targetRoot);
        // Receiver starts without a default runner. Use its locally installed profile
        // runner, and prepare the prefix after transfer without starting the game.
        const auto runner = target.dataRoot() + "/runners/GE-Proton11-7";
        file(runner + "/proton");
        file(runner + "/toolmanifest.vdf", "\"require_tool_appid\" \"4183110\"");
        const auto umu = temp.path() + "/umu";
        file(umu, "#!/bin/sh\n"
             "case \"$WINEPREFIX\" in *runtime-probe-*)\n"
             "  [ -z \"$UMU_ZENITY\" ] || exit 1\n"
             "  mkdir -p \"$UMU_FOLDERS_PATH/umu/steamrt4\"\n"
             "  touch \"$UMU_FOLDERS_PATH/umu/steamrt4/.installed.ok\" \"$UMU_FOLDERS_PATH/umu/steamrt4/_v2-entry-point\";;\n"
             "esac\nmkdir -p \"$WINEPREFIX\"\ntouch \"$WINEPREFIX/system.reg\"\n"
             "if [ \"$2\" != \"/c\" ]; then touch \"$WINEPREFIX/played\"; fi\n");
        QFile::setPermissions(umu, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        target.setSettings({{"umu", umu}, {"runner", temp.path() + "/missing-runner"}});
        for (const auto &name : {"First", "Second"}) {
            const auto folder = sourceRoot + '/' + name;
            file(folder + "/game.exe", "game asset"); file(folder + "/save/slot.dat", "private progress");
            source.import({inspect(folder)}, {});
        }
        QImage artwork(120, 168, QImage::Format_RGB32); artwork.fill(QColor("#e04020"));
        for (const auto &game : source.games()) {
            source.setMetadata(game.id, {{"title", game.name}, {"description", "Peer preview description"}});
            const auto cover = metadataCoverPath(source.dataRoot(), game.id);
            QDir().mkpath(QFileInfo(cover).absolutePath()); require(artwork.save(cover), "fixture artwork");
            auto config = loadSaveConfig(source, game.id); config.mappings = {{"game", "save"}};
            saveSaveConfig(source, game.id, config); createSaveSnapshot(source, game.id);
        }
        const auto sourceIdentity = ensureDeviceIdentity(source), targetIdentity = ensureDeviceIdentity(target);
        approvePairedPeer(source, targetIdentity.fingerprint, "Receiver");
        approvePairedPeer(target, sourceIdentity.fingerprint, "Source");
        // Existing pairing with a dead temporary port must recover through discovery.
        QTcpServer closed; require(closed.listen(QHostAddress::LocalHost, 0), "fixture listener");
        const auto deadPort = closed.serverPort(); closed.close();
        rememberPeerEndpoint(target, sourceIdentity.fingerprint, "127.0.0.1", deadPort);
        Window sender(sourceRoot); sender.show();
        auto service = sender.findChild<QTcpServer *>(); require(service && service->serverPort() != deadPort, "source listener");
        Window receiver(targetRoot, argv[1]); receiver.show();
        const auto prepared = [&] {
            for (const auto &game : target.games())
                if (game.runner != runner || !setupComplete(target, game) || target.busy(game.id) ||
                    target.status(game) != "Ready" ||
                    appliedSaveSnapshotId(target, game.id).isEmpty() ||
                    QFileInfo::exists(target.prefix(game.id) + "/played")) return false;
            auto play = receiver.findChild<QAction *>("listPlayAction");
            return play && play->isEnabled(); // The UI has observed the supervisor finishing.
        };
        auto table = receiver.findChild<QTableView *>("gameList");
        auto grid = dynamic_cast<GameGrid *>(receiver.findChild<QListView *>("gameGrid"));
        require(table && grid, "library views");
        require(waitFor([&] {
            return table->model()->rowCount() == 2 &&
                !table->model()->index(0, 0).data(CoverRole).value<QImage>().isNull();
        }), "paired games and covers must appear automatically after a stale endpoint");
        require(trustedPeers(target).first().port == service->serverPort(), "verified discovery must replace the dead port");
        require(target.games().isEmpty(), "remote cards must not create installed game records");
        const auto first = table->model()->index(0, 0);
        table->setCurrentIndex(first);
        require(first.data(RemoteRole).toBool() && !first.data(ManageableRole).toBool(), "remote actions differ from local actions");
        require(receiver.findChild<QLabel *>("gameStatus")->text().contains("Not installed"), "remote detail state");
        QImage painted(220, 300, QImage::Format_RGB32); painted.fill(Qt::black);
        QPainter painter(&painted); QStyleOptionViewItem option; option.rect = painted.rect();
        CardDelegate delegate(nullptr); delegate.paint(&painter, option, first); painter.end();
        const auto color = painted.pixelColor(100, 40);
        require(qAbs(color.red() - color.green()) <= 1 && qAbs(color.green() - color.blue()) <= 1,
                "remote artwork must be desaturated");
        grid->contextAction(first, QPoint(30, 30));
        auto menu = receiver.findChild<QMenu *>("cardMenu");
        require(menu && menu->actions().size() == 1 && menu->actions().first()->text() == "Install",
                "remote context menu must offer Install only");
        auto install = menu->actions().first(); menu->hide(); install->trigger();
        require(target.games().size() == 1, "context Install must transfer one game");
        require(table->model()->rowCount() == 2, "installation must replace its remote card, not duplicate it");
        require(QFileInfo::exists(metadataCoverPath(target.dataRoot(), target.games().first().id)), "installed cover persists");
        require(waitFor(prepared), "context Install must select a local runner and complete setup without playing");
        require(QFileInfo::exists(target.games().first().folder + "/save/slot.dat"), "fresh install applies peer progress after setup");
        require(!appliedSaveSnapshotId(target, target.games().first().id).isEmpty() &&
                importedSaveCandidates(target, target.games().first().id).isEmpty(),
                "installed peer progress is current without a manual sync choice");
        QModelIndex second;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const auto index = table->model()->index(row, 0);
            if (index.data(RemoteRole).toBool()) second = index;
        }
        require(second.isValid(), "second remote game remains"); table->setCurrentIndex(second);
        // QListView emits activated for both double-click and keyboard activation.
        require(QMetaObject::invokeMethod(grid, "activated", Qt::DirectConnection, Q_ARG(QModelIndex, second)), "activate remote card");
        require(target.games().size() == 2 && table->model()->rowCount() == 2, "activation must install once and replace the card");
        require(waitFor(prepared), "activation must also prepare the installed game without playing");
        for (int row = 0; row < 2; ++row) require(!table->model()->index(row, 0).data(RemoteRole).toBool(), "installed cards are local");
        // Closing windows during a new background lookup must release connections before libraries.
        receiver.refreshPeerGames();
        std::puts("PASS: automatic trusted endpoint recovery, peer covers, grayscale cards, context Install, activation, save separation and card replacement");
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return 0;
}
