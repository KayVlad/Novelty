#include "window.h"
#include "provisioning.h"
#include "metadata.h"
#include "saves.h"
#include "devices.h"
#include "sync_queue.h"
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qlistview.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qtableview.h>
#include <QtGui/qpainter.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qsettings.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qthread.h>
#include <QtCore/qtimer.h>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>

using namespace novelty;

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void touch(const QString &path, const QByteArray &contents = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path); require(file.open(QIODevice::WriteOnly), "Cannot create executable fixture"); file.write(contents);
}
static void addFolder(Window &window, const QString &folder, const QString &chosen, bool later = false) {
    bool submitted = false, answered = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        if (!submitted) {
            auto dialog = window.findChild<QDialog *>("addGameDialog");
            if (!dialog || !dialog->isVisible()) return;
            window.findChild<QLineEdit *>("addGameFolder")->setText(folder);
            window.findChild<QPushButton *>("addGameSubmit")->click();
            submitted = true;
        } else if (!answered) {
            auto dialog = window.findChild<QDialog *>("selectExecutableDialog");
            if (!dialog || !dialog->isVisible()) return;
            auto field = dialog->findChild<QLineEdit *>("selectedExecutable");
            if (later) dialog->reject();
            else { field->setText(chosen); dialog->findChild<QPushButton *>("useExecutable")->click(); }
            answered = true;
        }
    });
    poll.start(20);
    window.addGame();
    poll.stop();
    require(submitted && answered, "Add game did not offer executable selection");
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (argc < 2) return 1;
    QTimer fixtureWatchdog;
    fixtureWatchdog.setSingleShot(true);
    QObject::connect(&fixtureWatchdog, &QTimer::timeout, &app, [&] {
        const auto modal = QApplication::activeModalWidget();
        std::fprintf(stderr, "FAIL: UI fixture stalled in %s %s: %s\n",
                     modal ? modal->metaObject()->className() : "none",
                     modal ? modal->objectName().toUtf8().constData() : "no modal dialog",
                     modal ? modal->windowTitle().toUtf8().constData() : "");
        if (modal) for (const auto label : modal->findChildren<QLabel *>())
            std::fprintf(stderr, "  %s\n", label->text().toUtf8().constData());
        std::_Exit(2);
    });
    fixtureWatchdog.start(20000);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("NoveltyExecutableTests");
    QCoreApplication::setApplicationName("NoveltyExecutableTests");
    try {
        const auto root = temporary.path() + "/Games";
        QDir().mkpath(root);
        const auto source = temporary.path() + "/Many";
        touch(source + "/first.exe"); touch(source + "/Sub/second.exe");
        const auto missing = temporary.path() + "/No executable";
        touch(missing + "/readme.txt");
        const auto runner = temporary.path() + "/runner";
        touch(runner + "/proton");
        const auto umu = temporary.path() + "/umu";
        touch(umu, "#!/bin/sh\nif [ \"$2\" = \"/c\" ]; then case \"$PROTONPATH:$WINEPREFIX\" in *GE-Proton11-7*runtime-probe-*) mkdir -p \"$UMU_FOLDERS_PATH/umu/steamrt4\"; touch \"$UMU_FOLDERS_PATH/preflight-invoked\" \"$UMU_FOLDERS_PATH/umu/steamrt4/.installed.ok\" \"$UMU_FOLDERS_PATH/umu/steamrt4/_v2-entry-point\";; esac; mkdir -p \"$WINEPREFIX/drive_c/windows/system32\"; touch \"$WINEPREFIX/system.reg\" \"$WINEPREFIX/drive_c/windows/system32/winecfg.exe\" \"$WINEPREFIX/drive_c/windows/regedit.exe\"; exit 0; fi\nif [ -e \"$WINEPREFIX/dosdevices/n:\" ]; then touch \"$WINEPREFIX/dosdevices/n:/First.exe\" \"$WINEPREFIX/dosdevices/n:/Second.exe\"; else touch \"$WINEPREFIX/played\"; fi\n");
        QFile::setPermissions(umu, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Library library(root); library.setSettings({{"umu", umu}, {"runner", runner}});
        Window window(root, argv[1], QUrl("http://127.0.0.1:1/kana/")); window.show();
        bool openedDevices = false;
        QTimer::singleShot(0, &window, [&] {
            auto devices = window.findChild<QDialog *>("devicesDialog");
            openedDevices = devices && devices->findChild<QListWidget *>("trustedDevices") &&
                            devices->findChild<QPushButton *>("approvePairing") == nullptr;
            if (devices) devices->reject();
        });
        window.devices();
        require(openedDevices, "Devices screen did not open with the profile-local trust list");
        require(window.findChild<QWidget *>("detailPanel")->width() == 340, "Sidebar width changed");
        Cover cover; cover.resize(304, 200); cover.show();
        QImage art(200, 400, QImage::Format_RGB32); art.fill(Qt::red);
        { QPainter painter(&art); painter.fillRect(0, 0, 200, 20, Qt::blue); painter.fillRect(0, 380, 200, 20, Qt::green); }
        GameMetadata coverMetadata; coverMetadata.cover = art; cover.setGame("Portrait", coverMetadata);
        QCoreApplication::processEvents();
        const auto displayed = cover.grab().toImage();
        require(cover.height() == 608 && displayed.pixelColor(152, 10) == QColor(Qt::blue) &&
                displayed.pixelColor(152, 598) == QColor(Qt::green),
                "Sidebar cover cropped the portrait artwork");
        cover.setGame("Landscape", GameMetadata{QImage(400, 200, QImage::Format_RGB32)});
        require(cover.height() == 152, "Sidebar cover did not follow landscape artwork ratio");
        require(bestVndbMatch("My Game!", QJsonArray{
                    QJsonObject{{"id", "v1"}, {"title", "A weak search hit"}},
                    QJsonObject{{"id", "v2"}, {"title", "MY GAME"}}}) == "v2" &&
                bestVndbMatch("My Game", QJsonArray{
                    QJsonObject{{"id", "v1"}, {"title", "Other"}, {"alttitle", "my-game"}}}) == "v1" &&
                bestVndbMatch("No exact match", QJsonArray{
                    QJsonObject{{"id", "v1"}, {"title", "First"}},
                    QJsonObject{{"id", "v2"}, {"title", "Second"}}}) == "v1" &&
                bestVndbMatch("Anything", {}).isEmpty(), "VNDB matching did not prefer exact titles or searchrank fallback");
        addFolder(window, source, root + "/Many/Sub/second.exe");
        auto games = library.games();
        require(games.size() == 1 && games.first().executable == root + "/Many/Sub/second.exe" &&
                games.first().workingDirectory == root + "/Many/Sub", "UI choice did not save executable and cwd");
        auto grid = window.findChild<QListView *>("gameGrid");
        require(grid && grid->currentIndex().data(LaunchableRole).toBool(), "Chosen game did not reach Play");
        auto setupStatus = window.findChild<QLabel *>("gameSetupStatus");
        require(setupStatus && setupStatus->isVisible() &&
                setupStatus->text().contains("No save folder found yet") &&
                !setupStatus->text().contains("No executable selected yet") &&
                grid->currentIndex().data(SetupPendingRole).toBool() &&
                grid->currentIndex().siblingAtColumn(2).data(Qt::ForegroundRole).value<QColor>() == QColor("#8dcfff"),
                "Missing save folder did not show the light blue setup status and explanation");
        const auto id = games.first().id;
        addFolder(window, missing, {}, true);
        games = library.games();
        require(games.size() == 2, "Choosing Later lost the added game");
        Game pending;
        for (const auto &game : games) if (game.name == "No executable") pending = game;
        require(!pending.id.isEmpty() && pending.executable.isEmpty(), "No-candidate game was not left pending");
        require(setupStatus->isVisible() && setupStatus->text().contains("No executable selected yet") &&
                setupStatus->text().contains("No save folder found yet") &&
                grid->currentIndex().data(SetupPendingRole).toBool() &&
                grid->currentIndex().siblingAtColumn(2).data(Qt::ForegroundRole).value<QColor>() == QColor("#8dcfff"),
                "Missing executable and save folder did not explain both setup steps");
        touch(pending.folder + "/Manual.EXE");
        require(grid->currentIndex().data(LaunchableRole).toBool(), "Missing executable did not allow first Play selection");
        bool firstPlayPicker = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("selectExecutableDialog");
            if (dialog) { firstPlayPicker = true; dialog->reject(); }
        });
        window.launch();
        require(firstPlayPicker && library.game(pending.id).executable.isEmpty() &&
                !QFileInfo::exists(library.prefix(pending.id) + "/played"),
                "Cancelling first Play executable selection changed the game or launched it");
        firstPlayPicker = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("selectExecutableDialog");
            if (!dialog) return;
            firstPlayPicker = true;
            dialog->findChild<QLineEdit *>("selectedExecutable")->setText(pending.folder + "/Manual.EXE");
            dialog->findChild<QPushButton *>("useExecutable")->click();
        });
        window.launch();
        require(firstPlayPicker && library.game(pending.id).executable == pending.folder + "/Manual.EXE",
                "First Play executable selection was not saved");
        QElapsedTimer firstPlayDeadline; firstPlayDeadline.start();
        while (firstPlayDeadline.elapsed() < 8000 && !QFileInfo::exists(library.prefix(pending.id) + "/played")) {
            QCoreApplication::processEvents(); QThread::msleep(10);
        }
        if (!QFileInfo::exists(library.prefix(pending.id) + "/played")) {
            const auto status = readJson(library.gamePath(pending.id, "status.json"));
            throw std::runtime_error(("First Play did not continue after executable selection: " +
                status["message"].toString() + " (state=" + status["state"].toString() + ")").toStdString());
        }
        while (firstPlayDeadline.elapsed() < 12000 &&
               (library.busy(pending.id) ||
                readJson(library.gamePath(pending.id, "status.json"))["state"].toString() == "Starting")) {
            QCoreApplication::processEvents(); QThread::msleep(10);
        }
        require(readJson(library.gamePath(pending.id, "status.json"))["state"].toString() == "Ready",
                "First Play did not finish before the next Add flow");
        QThread::msleep(550); QCoreApplication::processEvents(); // Let the window's launch poll clear its pending ID.
        const auto setup = temporary.path() + "/Media/setup.exe";
        touch(setup);
        QString installedId;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("addGameDialog");
            dialog->findChild<QTabWidget *>("addGameTabs")->setCurrentIndex(1);
            dialog->findChild<QLineEdit *>("addGameInstaller")->setText(setup);
            dialog->findChild<QLineEdit *>("addGameName")->setText("Installed Ambiguous");
            dialog->findChild<QPushButton *>("addGameSubmit")->click();
        });
        window.addGame();
        for (const auto &game : library.games()) if (game.name == "Installed Ambiguous") installedId = game.id;
        require(!installedId.isEmpty(), "Installer Add did not create a game entry");
        bool installerChoiceShown = false, installerCandidatesCorrect = false;
        QTimer chooser;
        QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
            auto dialog = window.findChild<QDialog *>("selectExecutableDialog");
            if (!dialog || !dialog->isVisible()) return;
            installerChoiceShown = true;
            installerCandidatesCorrect = dialog->findChild<QListWidget *>("executableCandidates")->count() == 2;
            dialog->findChild<QLineEdit *>("selectedExecutable")->setText(root + "/Installed Ambiguous/Second.exe");
            dialog->findChild<QPushButton *>("useExecutable")->click();
            chooser.stop();
        });
        chooser.start(20);
        QElapsedTimer deadline; deadline.start();
        while (deadline.elapsed() < 8000 && library.game(installedId).executable.isEmpty()) {
            QCoreApplication::processEvents(); QThread::msleep(10);
        }
        chooser.stop();
        require(installerChoiceShown && installerCandidatesCorrect &&
                library.game(installedId).executable == root + "/Installed Ambiguous/Second.exe",
                "Installer completion did not offer and persist executable selection");
        require(grid->currentIndex().data(LaunchableRole).toBool(), "Installed game did not reach Play after selection");
        const auto managedRunner = root + "/.novelty/runners/GE-Proton11-7";
        touch(managedRunner + "/proton");
        touch(managedRunner + "/toolmanifest.vdf", "\"manifest\"\n{\n\"require_tool_appid\" \"4183110\"\n}\n");
        touch(root + "/.novelty/runners/Fixture/proton");
        auto settings = library.settings(); settings["runner"] = ""; library.setSettings(settings);
        const auto firstPlayFolder = root + "/First Play";
        touch(firstPlayFolder + "/play.exe");
        const auto firstPlayGame = library.addFolder(firstPlayFolder).game;
        require(firstPlayGame.runner.isEmpty(), "First Play fixture unexpectedly selected a runner");
        Window firstPlay(root, argv[1], QUrl("http://127.0.0.1:1/kana/")); firstPlay.show();
        auto table = firstPlay.findChild<QTableView *>("gameList");
        bool selected = false;
        for (int row = 0; row < table->model()->rowCount(); ++row) if (table->model()->index(row, 0).data().toString() == "First Play") {
            table->setCurrentIndex(table->model()->index(row, 0)); selected = true; break;
        }
        require(selected, "First Play game missing from the library");
        bool rejectedInvalidSave = false;
        QTimer::singleShot(0, &firstPlay, [&] {
            auto dialog = firstPlay.findChild<QDialog *>("manageDialog");
            auto name = dialog->findChild<QLineEdit *>("gameName");
            auto location = dialog->findChild<QLineEdit *>("saveLocation");
            auto buttons = dialog->findChild<QDialogButtonBox *>();
            name->clear(); location->setText("UserData"); location->setModified(true);
            QTimer::singleShot(0, dialog, [&] {
                auto warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (warning) { rejectedInvalidSave = warning->windowTitle() == "Cannot save"; warning->accept(); }
            });
            buttons->button(QDialogButtonBox::Save)->click();
            buttons->button(QDialogButtonBox::Cancel)->click();
        });
        firstPlay.manage();
        require(rejectedInvalidSave && loadSaveConfig(library, firstPlayGame.id).mappings.isEmpty() &&
                library.game(firstPlayGame.id).name == "First Play",
                "failed Manage Save followed by Cancel must leave mappings and game settings unchanged");
        bool savedSimplePath = false;
        QTimer::singleShot(0, &firstPlay, [&] {
            auto dialog = firstPlay.findChild<QDialog *>("manageDialog");
            auto location = dialog ? dialog->findChild<QLineEdit *>("saveLocation") : nullptr;
            auto history = dialog ? dialog->findChild<QListWidget *>("saveSnapshots") : nullptr;
            auto sync = dialog ? dialog->findChild<QPushButton *>("syncSavesFromPeer") : nullptr;
            auto buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
            if (!location || !history || !sync || !buttons ||
                dialog->findChild<QLineEdit *>("saveGameIdentity") ||
                dialog->findChild<QPushButton *>("saveMappingsButton") ||
                dialog->findChild<QPushButton *>("createSaveSnapshot")) {
                if (dialog) dialog->reject(); return;
            }
            location->setText("UserData"); location->setModified(true);
            savedSimplePath = true;
            buttons->button(QDialogButtonBox::Save)->click();
        });
        firstPlay.manage();
        require(savedSimplePath && loadSaveConfig(library, firstPlayGame.id).mappings.size() == 1 &&
                loadSaveConfig(library, firstPlayGame.id).mappings.first().path == "UserData",
                "simple Saves tab persists the one manual path through global Save");
        const auto aokanaFolder = root + "/Aokana";
        touch(aokanaFolder + "/Aokana.exe");
        touch(aokanaFolder + "/SaveData/AoKana001.dat", "saved progress");
        const auto aokanaGame = library.addFolder(aokanaFolder).game;
        touch(library.prefix(aokanaGame.id) +
              "/drive_c/users/steamuser/AppData/LocalLow/NekoNyanSoft/Aokana/output_log.txt", "Unity diagnostics");
        Window candidateWindow(root, argv[1], QUrl("http://127.0.0.1:1/kana/")); candidateWindow.show();
        auto candidateTable = candidateWindow.findChild<QTableView *>("gameList");
        bool candidateSelected = false;
        for (int row = 0; row < candidateTable->model()->rowCount(); ++row)
            if (candidateTable->model()->index(row, 0).data().toString() == "Aokana") {
                candidateTable->setCurrentIndex(candidateTable->model()->index(row, 0));
                candidateSelected = true; break;
            }
        require(candidateSelected, "Aokana candidate fixture missing from the library");
        bool candidatesShown = false, candidateFilledInput = false;
        QTimer::singleShot(0, &candidateWindow, [&] {
            auto dialog = candidateWindow.findChild<QDialog *>("manageDialog");
            auto candidates = dialog ? dialog->findChild<QListWidget *>("saveLocationCandidates") : nullptr;
            auto location = dialog ? dialog->findChild<QLineEdit *>("saveLocation") : nullptr;
            auto status = dialog ? dialog->findChild<QLabel *>("saveDiscoveryStatus") : nullptr;
            auto tabs = dialog ? dialog->findChild<QTabWidget *>("manageTabs") : nullptr;
            auto buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
            if (!candidates || !location || !status || !tabs || !buttons || candidates->count() != 2) {
                if (dialog) dialog->reject(); return;
            }
            tabs->setCurrentWidget(candidates->parentWidget());
            candidatesShown = candidates->item(0)->text().endsWith("/Aokana/SaveData") &&
                              candidates->item(1)->text().endsWith("/NekoNyanSoft/Aokana") &&
                              status->text().contains("Multiple possible save folders") && candidates->isVisible();
            candidates->setCurrentRow(0);
            candidateFilledInput = location->text().endsWith("/Aokana/SaveData") && location->isModified();
            buttons->button(QDialogButtonBox::Save)->click();
        });
        candidateWindow.manage();
        const auto selectedSave = loadSaveConfig(library, aokanaGame.id);
        require(candidatesShown && candidateFilledInput && selectedSave.mappings.size() == 1 &&
                selectedSave.mappings.first().scope == "game" && selectedSave.mappings.first().path == "SaveData" &&
                selectedSave.discoverySource == "manual",
                "Saves tab lets the user choose one of multiple detected folders and save it");
        const auto peerRoot = temporary.path() + "/Offline peer"; QDir().mkpath(peerRoot);
        Library offlinePeer(peerRoot);
        const auto peerFingerprint = ensureDeviceIdentity(offlinePeer).fingerprint;
        approvePairedPeer(library, peerFingerprint, "Offline peer");
        rememberPeerEndpoint(library, peerFingerprint, "127.0.0.1", 1);
        const auto aokanaHead = appliedSaveSnapshotId(library, aokanaGame.id);
        queuePeerSync(library, aokanaGame.id, aokanaHead);
        const auto headPath = library.gamePath(aokanaGame.id, "save-head.json");
        const auto intactHead = readJson(headPath);
        touch(headPath, "damaged snapshot pointer");
        candidateWindow.syncPendingPeers();
        bool preservedPendingSync = pendingPeerSync(library).size() == 1;
        QFile::remove(headPath); candidateWindow.syncPendingPeers();
        preservedPendingSync &= pendingPeerSync(library).size() == 1;
        writeJson(headPath, intactHead);
        acknowledgePeerSync(library, peerFingerprint, aokanaGame.id, aokanaHead);
        revokePeer(library, peerFingerprint);
        require(preservedPendingSync, "a save-head read error must not discard pending peer work");
        candidateWindow.close();
        firstPlay.launch();
        deadline.restart();
        while (deadline.elapsed() < 8000 && !QFileInfo::exists(library.prefix(firstPlayGame.id) + "/played")) {
            QCoreApplication::processEvents(); QThread::msleep(10);
        }
        if (library.game(firstPlayGame.id).runner != managedRunner ||
            !QFileInfo::exists(library.prefix(firstPlayGame.id) + "/played") ||
            !QFileInfo::exists(library.dataRoot() + "/runtime/preflight-invoked")) {
            const auto status = readJson(library.gamePath(firstPlayGame.id, "status.json"));
            throw std::runtime_error(("First Play did not continue: " +
                status["message"].toString() + " (runner=" + library.game(firstPlayGame.id).runner +
                ", played=" + QString::number(QFileInfo::exists(library.prefix(firstPlayGame.id) + "/played")) +
                ", preflight=" + QString::number(QFileInfo::exists(library.dataRoot() + "/runtime/preflight-invoked")) +
                ")").toStdString());
        }
        const auto runtimeRoot = temporary.path() + "/Runtime preparation";
        const auto runtimeRunner = temporary.path() + "/Runtime runner";
        touch(runtimeRoot + "/Game/play.exe"); touch(runtimeRunner + "/proton");
        touch(runtimeRunner + "/toolmanifest.vdf", "\"manifest\"\n{\n\"require_tool_appid\" \"4183110\"\n}\n");
        require(requiredSteamRuntime(runtimeRunner) == "steamrt4", "Selected Proton runtime was not identified");
        const auto fakeRuntimeUmu = temporary.path() + "/fake-runtime-umu";
        touch(fakeRuntimeUmu, "#!/bin/sh\n[ -z \"$UMU_ZENITY\" ] || { echo 'UMU_ZENITY must be unset for the Python downloader' >&2; exit 1; }\nmkdir -p \"$UMU_FOLDERS_PATH/umu/steamrt4\"\ntouch \"$UMU_FOLDERS_PATH/invoked\"\nif [ -e \"$UMU_FOLDERS_PATH/fail\" ]; then echo 'Fixture runtime download failed' >&2; exit 1; fi\nif [ -e \"$UMU_FOLDERS_PATH/cancel\" ]; then touch \"$UMU_FOLDERS_PATH/waiting\"; exec sleep 20; fi\ntouch \"$UMU_FOLDERS_PATH/umu/steamrt4/.installed.ok\" \"$UMU_FOLDERS_PATH/umu/steamrt4/_v2-entry-point\"\nexit 0\n");
        QFile::setPermissions(fakeRuntimeUmu, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Library runtimeLibrary(runtimeRoot); runtimeLibrary.setSettings({{"umu", fakeRuntimeUmu}});
        runtimeLibrary.import({inspect(runtimeRoot + "/Game")}, runtimeRunner);
        const auto runtimeGame = runtimeLibrary.games().first();
        const auto runtimeData = runtimeLibrary.dataRoot() + "/runtime";
        touch(runtimeData + "/fail");
        bool retriedRuntime = false;
        QTimer runtimeRetry; runtimeRetry.setInterval(20);
        QObject::connect(&runtimeRetry, &QTimer::timeout, &firstPlay, [&] {
            auto dialog = firstPlay.findChild<QDialog *>("steamRuntimeDialog");
            auto retry = dialog ? dialog->findChild<QPushButton *>("retrySteamRuntime") : nullptr;
            if (!retry || !retry->isEnabled() || retriedRuntime) return;
            retriedRuntime = true; QFile::remove(runtimeData + "/fail"); retry->click(); runtimeRetry.stop();
        });
        QTimer runtimeWatchdog; runtimeWatchdog.setSingleShot(true);
        QObject::connect(&runtimeWatchdog, &QTimer::timeout, &firstPlay, [&] {
            if (auto dialog = firstPlay.findChild<QDialog *>("steamRuntimeDialog")) dialog->reject();
        });
        // A parent process may also export this; provisioning must remove it.
        const auto inheritedZenity = qgetenv("UMU_ZENITY");
        const bool hadZenity = qEnvironmentVariableIsSet("UMU_ZENITY");
        qputenv("UMU_ZENITY", "0");
        runtimeRetry.start(); runtimeWatchdog.start(5000);
        const bool runtimeInstalled = provisionSteamRuntime(&firstPlay, runtimeLibrary, runtimeGame);
        runtimeRetry.stop(); runtimeWatchdog.stop();
        require(retriedRuntime && runtimeInstalled && QFileInfo::exists(runtimeData + "/umu/steamrt4/.installed.ok") &&
                QDir(runtimeLibrary.dataRoot()).entryList({"runtime-probe-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
                "Steam Linux Runtime failure/retry did not complete or clean its probe");
        QFile::remove(runtimeData + "/invoked");
        require(provisionSteamRuntime(&firstPlay, runtimeLibrary, runtimeGame) && !QFileInfo::exists(runtimeData + "/invoked"),
                "Installed Steam Linux Runtime was not reused offline");
        QFile::remove(runtimeData + "/umu/steamrt4/.installed.ok");
        QFile::remove(runtimeData + "/umu/steamrt4/_v2-entry-point");
        touch(runtimeData + "/cancel");
        QTimer runtimeCancel; runtimeCancel.setInterval(20);
        QObject::connect(&runtimeCancel, &QTimer::timeout, &firstPlay, [&] {
            if (!QFileInfo::exists(runtimeData + "/waiting")) return;
            runtimeCancel.stop();
            if (auto dialog = firstPlay.findChild<QDialog *>("steamRuntimeDialog")) dialog->reject();
        });
        runtimeCancel.start(); runtimeWatchdog.start(5000);
        const bool cancelledRuntime = provisionSteamRuntime(&firstPlay, runtimeLibrary, runtimeGame);
        runtimeCancel.stop(); runtimeWatchdog.stop();
        require(!cancelledRuntime && !QFileInfo::exists(runtimeData + "/umu/steamrt4/.installed.ok") &&
                QDir(runtimeLibrary.dataRoot()).entryList({"runtime-probe-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
                "Cancelling runtime setup selected an incomplete runtime or left a probe");
        const auto combinedRoot = temporary.path() + "/Combined first Play";
        touch(combinedRoot + "/Game/play.exe");
        Library combinedLibrary(combinedRoot);
        combinedLibrary.import({inspect(combinedRoot + "/Game")}, {});
        auto combinedGame = combinedLibrary.games().first();
        const auto combinedGe = combinedLibrary.dataRoot() + "/runners/GE-Proton11-7";
        combinedGame.runner = combinedGe; // First-Play selection is not saved until preparation succeeds.
        const auto combinedGeHelper = temporary.path() + "/combined-ge.py";
        touch(combinedGeHelper, "import os,sys\nr=sys.argv[1]\nopen(r+'/.novelty/ge-invoked','w').close()\nif os.path.exists(r+'/.novelty/ge-fail'): sys.exit(1)\np=r+'/.novelty/runners/GE-Proton11-7'\nos.makedirs(p,exist_ok=True)\nopen(p+'/proton','w').close()\nopen(p+'/toolmanifest.vdf','w').write('\\\"require_tool_appid\\\" \\\"4183110\\\"')\nprint(p)\n");
        const auto combinedUmuHelper = temporary.path() + "/combined-umu.py";
        touch(combinedUmuHelper, "import os,sys,time\nr=sys.argv[1]\np=r+'/.novelty/tools/umu-1.4.4/umu-run'\nos.makedirs(os.path.dirname(p),exist_ok=True)\nopen(r+'/.novelty/umu-invoked','w').close()\nif os.path.exists(r+'/.novelty/umu-wait'): open(r+'/.novelty/waiting','w').close(); time.sleep(20)\nopen(p,'w').write('#!/bin/sh\\n[ -z \\\"$UMU_ZENITY\\\" ] || exit 1\\nmkdir -p \\\"$UMU_FOLDERS_PATH/umu/steamrt4\\\"\\ntouch \\\"$UMU_FOLDERS_PATH/umu/steamrt4/.installed.ok\\\" \\\"$UMU_FOLDERS_PATH/umu/steamrt4/_v2-entry-point\\\"\\n')\nos.chmod(p,0o700)\nprint(p)\n");
        touch(combinedLibrary.dataRoot() + "/ge-fail");
        writeJson(combinedLibrary.dataRoot() + "/ge-proton.json", {{"version", "GE-Proton11-7"}, {"auto_download", false}});
        bool combinedStarted = false, combinedRetried = false, oneDialog = false, policyRespected = false;
        QTimer combinedPoll; combinedPoll.setInterval(20);
        QTimer combinedWatchdog; combinedWatchdog.setSingleShot(true);
        QObject::connect(&combinedWatchdog, &QTimer::timeout, &firstPlay, [&] {
            if (auto dialog = firstPlay.findChild<QDialog *>("firstPlayPreparationDialog")) dialog->reject();
        });
        QObject::connect(&combinedPoll, &QTimer::timeout, &firstPlay, [&] {
            auto dialog = firstPlay.findChild<QDialog *>("firstPlayPreparationDialog");
            if (!dialog || !dialog->isVisible()) return;
            oneDialog = !firstPlay.findChild<QDialog *>("geInstallerDialog") &&
                        !firstPlay.findChild<QDialog *>("umuInstallerDialog") &&
                        !firstPlay.findChild<QDialog *>("steamRuntimeDialog");
            auto start = dialog->findChild<QPushButton *>("firstPlayPreparationStart");
            if (!start || !start->isEnabled()) return;
            if (!combinedStarted && start->text() == "Start setup") {
                combinedStarted = true;
                policyRespected = !QFileInfo::exists(combinedLibrary.dataRoot() + "/ge-invoked");
                start->click();
            } else if (!combinedRetried && start->text() == "Retry") {
                combinedRetried = true; QFile::remove(combinedLibrary.dataRoot() + "/ge-fail"); start->click();
            }
        });
        combinedPoll.start(); combinedWatchdog.start(8000);
        const bool combinedPrepared = provisionFirstPlay(&firstPlay, combinedLibrary, combinedGame, "GE-Proton11-7",
                                                          combinedGeHelper, combinedUmuHelper);
        combinedPoll.stop(); combinedWatchdog.stop();
        require(combinedPrepared && combinedStarted && combinedRetried && oneDialog && policyRespected &&
                QFileInfo::exists(combinedGe + "/proton") &&
                QFileInfo::exists(combinedLibrary.dataRoot() + "/umu-invoked") &&
                QFileInfo::exists(combinedLibrary.dataRoot() + "/runtime/umu/steamrt4/.installed.ok") &&
                combinedLibrary.game(combinedGame.id).runner.isEmpty() &&
                QDir(combinedLibrary.dataRoot()).entryList({"runtime-probe-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
                "One first-Play preparation dialog did not complete GE, umu and runtime with explicit start/retry");
        const auto cancelRoot = temporary.path() + "/Combined cancelled";
        touch(cancelRoot + "/Game/play.exe");
        Library cancelLibrary(cancelRoot); cancelLibrary.import({inspect(cancelRoot + "/Game")}, {});
        auto cancelGame = cancelLibrary.games().first(); cancelGame.runner = cancelLibrary.dataRoot() + "/runners/GE-Proton11-7";
        touch(cancelLibrary.dataRoot() + "/umu-wait");
        QTimer cancelPoll; cancelPoll.setInterval(20);
        QObject::connect(&cancelPoll, &QTimer::timeout, &firstPlay, [&] {
            if (!QFileInfo::exists(cancelLibrary.dataRoot() + "/waiting")) return;
            cancelPoll.stop();
            if (auto dialog = firstPlay.findChild<QDialog *>("firstPlayPreparationDialog")) dialog->reject();
        });
        cancelPoll.start(); combinedWatchdog.start(8000);
        const bool cancelledCombined = provisionFirstPlay(&firstPlay, cancelLibrary, cancelGame, "GE-Proton11-7",
                                                           combinedGeHelper, combinedUmuHelper);
        cancelPoll.stop(); combinedWatchdog.stop();
        require(!cancelledCombined && cancelLibrary.game(cancelGame.id).runner.isEmpty() &&
                !QFileInfo::exists(cancelLibrary.dataRoot() + "/runtime/umu/steamrt4/.installed.ok"),
                "Cancelling combined first-Play setup saved an incomplete runner or runtime");
        if (hadZenity) qputenv("UMU_ZENITY", inheritedZenity); else qunsetenv("UMU_ZENITY");
        const auto provisionRoot = temporary.path() + "/Provision";
        QDir().mkpath(provisionRoot);
        const auto umuHelper = temporary.path() + "/fake-umu-helper.py";
        touch(umuHelper, "import os, sys\np = sys.argv[1] + '/.novelty/tools/umu-1.4.4/umu-run'\nos.makedirs(os.path.dirname(p), exist_ok=True)\nwith open(p, 'w') as f: f.write('#!/bin/sh\\nexit 0\\n')\nos.chmod(p, 0o700)\nprint(p)\n");
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &firstPlay, [&] {
            if (auto dialog = firstPlay.findChild<QDialog *>("umuInstallerDialog")) dialog->reject();
        });
        watchdog.start(5000);
        const auto installedUmu = provisionUmu(&firstPlay, provisionRoot, umuHelper, true); watchdog.stop();
        require(installedUmu == managedUmuPath(provisionRoot), "Automatic umu provisioning did not start and complete");
        const auto geHelper = temporary.path() + "/fake-ge-helper.py";
        touch(geHelper, "import os, sys\np = sys.argv[1] + '/.novelty/runners/GE-Proton11-7'\nos.makedirs(p, exist_ok=True)\nopen(p + '/proton', 'w').close()\nopen(sys.argv[1] + '/invoked', 'w').close()\nprint(p)\n");
        watchdog.start(5000);
        const auto installedGe = provisionGe(&firstPlay, provisionRoot, geHelper, true, "GE-Proton11-7"); watchdog.stop();
        require(installedGe == provisionRoot + "/.novelty/runners/GE-Proton11-7",
                "Pinned GE provisioning did not start and complete");
        QFile::remove(provisionRoot + "/invoked");
        writeJson(provisionRoot + "/.novelty/ge-proton.json", {{"version", "GE-Proton11-7"}, {"auto_download", false}});
        QString disabledMessage;
        QTimer disabledCheck;
        disabledCheck.setSingleShot(true);
        QObject::connect(&disabledCheck, &QTimer::timeout, &firstPlay, [&] {
            auto dialog = firstPlay.findChild<QDialog *>("geInstallerDialog");
            if (!dialog) return;
            disabledMessage = dialog->findChild<QLabel *>("geInstallStatus")->text(); dialog->reject();
        });
        disabledCheck.start(100);
        const auto disabledInstall = provisionGe(&firstPlay, provisionRoot, geHelper, true); disabledCheck.stop();
        require(disabledInstall.isEmpty() && disabledMessage.contains("disabled") &&
                !QFileInfo::exists(provisionRoot + "/invoked") &&
                !readJson(provisionRoot + "/.novelty/ge-proton.json")["auto_download"].toBool(),
                "First Play ignored the library's disabled automatic-download policy");
        Library reopened(root);
        require(reopened.game(id).executable == root + "/Many/Sub/second.exe" &&
                reopened.game(pending.id).executable == pending.folder + "/Manual.EXE" &&
                reopened.game(installedId).executable == root + "/Installed Ambiguous/Second.exe",
                "Executable choice did not persist after reopening");
        std::printf("Executable UI flow passed\n");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
