#include "window.h"
#include "app_settings.h"
#include "provisioning.h"
#include "metadata.h"
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qtableview.h>
#include <QtWidgets/qtoolbutton.h>
#include <QtWidgets/qstackedwidget.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qplaintextedit.h>
#include <QtGui/qevent.h>
#include <QtGui/qpixmap.h>
#include <QtGui/qaction.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qmenu.h>
#include <QtWidgets/qfiledialog.h>
#include <QtWidgets/qheaderview.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qsettings.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qthread.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qbuffer.h>
#include <QtCore/qregularexpression.h>
#include <QtNetwork/qtcpserver.h>
#include <QtNetwork/qtcpsocket.h>
#include <QtNetwork/qhostaddress.h>
#include <QtGui/qimage.h>
#include <cstdio>
#include <cstdlib>

using namespace novelty;
void file(const QString &path, const QByteArray &data = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create fixture"); f.write(data);
}
bool waitFor(const std::function<bool()> &check, int limit = 7000) {
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < limit) { QCoreApplication::processEvents(); if (check()) return true; QThread::msleep(10); }
    return false;
}
class MetadataFixture : public QTcpServer {
public:
    int requests = 0;
    QByteArray cover;
    MetadataFixture() {
        QImage image(3, 4, QImage::Format_RGB32); image.fill(QColor("#456789"));
        QBuffer buffer(&cover); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
        if (!listen(QHostAddress::LocalHost)) throw std::runtime_error("Cannot start metadata fixture");
        QObject::connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto socket = nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes = QByteArray(), responded = false]() mutable {
                    if (responded) return;
                    bytes += socket->readAll();
                    const int split = bytes.indexOf("\r\n\r\n"); if (split < 0) return;
                    const auto header = bytes.left(split); const auto match = QRegularExpression("Content-Length: ([0-9]+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(header));
                    const int length = match.hasMatch() ? match.captured(1).toInt() : 0;
                    if (bytes.size() < split + 4 + length) return;
                    const auto body = QJsonDocument::fromJson(bytes.mid(split + 4, length)).object();
                    const auto path = header.left(header.indexOf(" HTTP/"));
                    QByteArray payload;
                    if (path.startsWith("GET /cover.png")) payload = cover;
                    else if (path.contains("/release")) {
                        const int page = body["page"].toInt();
                        const QJsonObject producer{{"name", page == 1 ? "Publisher A" : "Publisher B"}, {"publisher", true}};
                        payload = QJsonDocument(QJsonObject{{"results", QJsonArray{QJsonObject{{"producers", QJsonArray{producer}}}}}, {"more", page == 1}}).toJson(QJsonDocument::Compact);
                    } else if (body["fields"].toString().contains("length_minutes")) {
                        const QJsonObject vn{{"id", "v123"}, {"title", "English title"},
                            {"titles", QJsonArray{QJsonObject{{"title", "原題"}, {"main", true}}}},
                            {"description", "[b]A story[/b] about choices."}, {"length_minutes", 180},
                            {"developers", QJsonArray{QJsonObject{{"name", "Studio A"}}}},
                            {"tags", QJsonArray{QJsonObject{{"name", "Mystery"}, {"category", "cont"}, {"spoiler", 0}, {"rating", 3.0}},
                                                QJsonObject{{"name", "Hidden"}, {"category", "cont"}, {"spoiler", 1}},
                                                QJsonObject{{"name", "Engine"}, {"category", "tech"}, {"spoiler", 0}}}},
                            {"image", QJsonObject{{"thumbnail", QString("http://127.0.0.1:%1/cover.png").arg(serverPort())}}}};
                        payload = QJsonDocument(QJsonObject{{"results", QJsonArray{vn}}}).toJson(QJsonDocument::Compact);
                    } else payload = QJsonDocument(QJsonObject{{"results", QJsonArray{QJsonObject{{"id", "v123"}, {"title", "English title"}}}}}).toJson(QJsonDocument::Compact);
                    ++requests;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
                    responded = true; socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }
};
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (argc < 3) return 1;
    const QString launcher = argv[1];
    QTemporaryDir t;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, t.path());
    QCoreApplication::setOrganizationName("NoveltyTests"); QCoreApplication::setApplicationName("NoveltyTests");
    qputenv("NOVELTY_TEST_APP_DATA_DIR", QFile::encodeName(t.path()));
    try {
        const QString root = t.path() + "/Games"; QDir().mkpath(root);
        file(root + "/Game/game.exe"); file(t.path() + "/runner/proton");
        const auto fake = t.path() + "/fake-umu";
        file(fake, "#!/bin/sh\nif [ \"$2\" = \"/c\" ]; then mkdir -p \"$WINEPREFIX/drive_c/windows/system32\"; touch \"$WINEPREFIX/system.reg\" \"$WINEPREFIX/drive_c/windows/system32/winecfg.exe\" \"$WINEPREFIX/drive_c/windows/regedit.exe\"; exit 0; fi\nprintf '%s\\n' \"$1\" \"$PWD\" > \"$WINEPREFIX/launched-paths\"\ntouch \"$WINEPREFIX/started\"\nsleep 1\ntouch \"$WINEPREFIX/survived\"\n");
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Library lib(root); lib.setSettings({{"umu", fake}, {"runner", t.path() + "/runner"}});
        lib.import(discover(root), t.path() + "/runner"); const auto game = lib.games().first();
        {
            Window window(root, launcher); window.show(); QCoreApplication::processEvents();
            if (window.findChild<QPushButton *>("playButton") || window.findChild<QLabel *>("logo")) throw std::runtime_error("Removed sidebar/header controls remain");
            auto pathLink = window.findChild<QLabel *>("gamesFolderLink");
            if (!pathLink || !pathLink->text().contains("href=")) throw std::runtime_error("Games folder path is not a link");
            auto grid = window.findChild<QListView *>("gameGrid");
            auto table = window.findChild<QTableView *>("gameList");
            auto listButton = window.findChild<QToolButton *>("listViewButton");
            auto gridButton = window.findChild<QToolButton *>("gridViewButton");
            if (!grid || !grid->currentIndex().data(LaunchableRole).toBool()) throw std::runtime_error("Selected game cannot launch");
            if (!grid || !table || grid->selectionModel() != table->selectionModel()) throw std::runtime_error("Library views do not share selection");
            if (table->horizontalHeader()->defaultAlignment() != (Qt::AlignLeft | Qt::AlignVCenter) ||
                table->model()->index(0, 0).data(Qt::TextAlignmentRole).toInt() != int(Qt::AlignLeft | Qt::AlignVCenter)) throw std::runtime_error("List headings and cells are misaligned");
            listButton->click(); if (!table->isVisible() || grid->isVisible()) throw std::runtime_error("List toggle failed");
            QString listManageError;
            QTimer::singleShot(0, &window, [&] { auto dialog = window.findChild<QDialog *>("manageDialog"); if (!dialog) listManageError = "List Manage did not open"; else dialog->reject(); });
            auto listManage = table->findChild<QAction *>("listManageAction");
            if (!listManage || !listManage->isEnabled()) throw std::runtime_error("List view lost Manage action");
            listManage->trigger(); if (!listManageError.isEmpty()) throw std::runtime_error(listManageError.toStdString());
            table->setCurrentIndex(table->model()->index(0, 2)); // Clicking a status cell still selects this game.
            gridButton->click(); QCoreApplication::processEvents();
            if (!grid->isVisible() || table->isVisible() || grid->currentIndex().data().toString() != game.name) throw std::runtime_error("Grid toggle lost selection");
            for (const auto name : {"metadataPlaytime"}) {
                auto field = window.findChild<QLabel *>(name);
                if (!field || field->text() != "—") throw std::runtime_error("Metadata placeholder is missing or invented");
            }
            window.resize(900, 620); QCoreApplication::processEvents();
            auto smallCard = grid->visualRect(grid->currentIndex()).adjusted(5, 5, -5, -5);
            if (std::abs(smallCard.width() * 4 - smallCard.height() * 3) > 2)
                throw std::runtime_error("Grid card lost its 3:4 aspect ratio");
            const QPoint statusPoint(smallCard.right() - 18, smallCard.bottom() - 27);
            if (!grid->viewport()->rect().contains(statusPoint)) throw std::runtime_error("Card status light escapes smaller window");
            if (grid->viewport()->grab().toImage().pixelColor(statusPoint) !=
                statusColor(grid->currentIndex().data(StatusRole).toString(),
                            grid->currentIndex().data(SetupPendingRole).toBool()))
                throw std::runtime_error("Card status light is not at the bottom right");
            const auto previewDirectory = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
            if (!previewDirectory.isEmpty()) {
                QDir().mkpath(previewDirectory);
                if (!window.grab().save(previewDirectory + "/layout-small.png")) throw std::runtime_error("Cannot save smaller layout preview");
            }
            window.resize(1280, 800); QCoreApplication::processEvents();
            // Double-click the card body to launch without an embedded Play button.
            auto card = grid->visualRect(grid->currentIndex()).adjusted(5, 5, -5, -5);
            QPointF point(card.center());
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(grid->viewport(), &press); QCoreApplication::sendEvent(grid->viewport(), &release);
            QMouseEvent doubleClick(QEvent::MouseButtonDblClick, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(grid->viewport(), &doubleClick); QCoreApplication::sendEvent(grid->viewport(), &release);
            if (!waitFor([&] { return QFileInfo::exists(lib.prefix(game.id) + "/started"); }))
                throw std::runtime_error(("Supervisor never launched game: status=" + lib.status(lib.game(game.id)) +
                    " record=" + QString::fromUtf8(QJsonDocument(readJson(lib.gamePath(game.id, "status.json"))).toJson(QJsonDocument::Compact)) +
                    " log=" + [&] { QFile log(lib.gamePath(game.id, "launch.log")); return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString("<none>"); }()).toStdString());
            // Window destruction must leave the detached supervisor and game alive.
        }
        if (!waitFor([&] { return QFileInfo::exists(lib.prefix(game.id) + "/survived"); })) throw std::runtime_error("Closing UI killed the game");
        if (!waitFor([&] { return !lib.busy(game.id); })) throw std::runtime_error("Supervisor failed to release lock");
        if (lib.status(game) != "Ready") throw std::runtime_error("Successful exit not recorded");
        {
            const auto addRoot = t.path() + "/Add Games"; QDir().mkpath(addRoot);
            const auto external = t.path() + "/Existing game"; file(external + "/game.exe");
            const auto installer = t.path() + "/External media/setup.exe"; file(installer);
            const auto installUmu = t.path() + "/install-umu";
            file(installUmu, "#!/bin/sh\nif [ \"$2\" = \"/c\" ]; then mkdir -p \"$WINEPREFIX/drive_c\"; touch \"$WINEPREFIX/system.reg\"; exit 0; fi\ntouch \"$WINEPREFIX/installer-started\"\nsleep 0.3\ntouch \"$WINEPREFIX/dosdevices/n:/Story.exe\"\n");
            QFile::setPermissions(installUmu, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            Library addLib(addRoot); addLib.setSettings({{"umu", installUmu}, {"runner", t.path() + "/runner"}});
            QString error; QString installingId;
            {
                Window window(addRoot, launcher); window.show(); QCoreApplication::processEvents();
                QTimer::singleShot(0, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("addGameDialog");
                    auto folder = window.findChild<QLineEdit *>("addGameFolder");
                    if (!dialog || !folder) { error = "Missing Add folder controls"; if (dialog) dialog->reject(); return; }
                    folder->setText(external); window.findChild<QPushButton *>("addGameSubmit")->click();
                    if (dialog->result() != QDialog::Accepted) { error = "Add folder failed"; dialog->reject(); }
                });
                window.addGame();
                if (!error.isEmpty() || addLib.games().size() != 1 || addLib.games().first().executable != addRoot + "/Existing game/game.exe" ||
                    QFileInfo::exists(external) || !QFileInfo::exists(addRoot + "/Existing game/game.exe"))
                    throw std::runtime_error("Folder move and scan UI failed");
                QTimer::singleShot(0, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("addGameDialog");
                    auto tabs = window.findChild<QTabWidget *>("addGameTabs");
                    if (!dialog || !tabs) { error = "Missing installer tab"; if (dialog) dialog->reject(); return; }
                    tabs->setCurrentIndex(1);
                    window.findChild<QLineEdit *>("addGameInstaller")->setText(installer);
                    window.findChild<QLineEdit *>("addGameName")->setText("Installed story");
                    auto language = window.findChild<QComboBox *>("addGameInstallerLanguage");
                    if (!language || language->findData("ja_JP.UTF-8") < 0) { error = "Japanese installer option missing"; dialog->reject(); return; }
                    language->setCurrentIndex(language->findData("ja_JP.UTF-8"));
                    const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                    if (!preview.isEmpty()) { QCoreApplication::processEvents(); dialog->grab().save(preview + "/add-installer.png"); }
                    window.findChild<QPushButton *>("addGameSubmit")->click();
                    if (dialog->result() != QDialog::Accepted) { error = "Installer submit failed: " + window.findChild<QLabel *>("addGameError")->text(); dialog->reject(); }
                });
                window.addGame(); if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                for (const auto &entry : addLib.games()) if (entry.installPending) installingId = entry.id;
                if (installingId.isEmpty() || !waitFor([&] { return QFileInfo::exists(addLib.prefix(installingId) + "/installer-started"); })) throw std::runtime_error("Installer UI never started supervisor");
                auto grid = window.findChild<QListView *>("gameGrid");
                if (grid->currentIndex().data(LaunchableRole).toBool()) throw std::runtime_error("Pending installer allowed Play");
                // Closing the UI must not kill the installer or lose final detection.
            }
            if (!waitFor([&] { return !addLib.busy(installingId) && !addLib.game(installingId).installPending; })) throw std::runtime_error("Installer did not survive closing UI and detect game");
            if (addLib.game(installingId).executable != addRoot + "/Installed story/Story.exe" || addLib.game(installingId).locale != "ja_JP.UTF-8")
                throw std::runtime_error("Installer detection lost target folder or Japanese locale");
        }
        {
            Window window(root, launcher); window.show();
            const auto renamed = root + "/Renamed 日本語";
            if (!QDir().rename(game.folder, renamed)) throw std::runtime_error("Cannot rename fixture game");
            window.scan();
            if (!waitFor([&] { return lib.game(game.id).folder == renamed; })) throw std::runtime_error("Renamed game was not automatically relinked");
            if (lib.game(game.id).executable != renamed + "/game.exe" || lib.games().size() != 1) throw std::runtime_error("Automatic rename duplicated game or lost executable");
            file(root + "/Copy/copy.exe");
            window.scan();
            auto notice = window.findChild<QLabel *>("libraryNotice");
            if (!waitFor([&] { return notice && notice->text().contains("settle"); })) throw std::runtime_error("New copy was not deferred");
            if (lib.games().size() != 1) throw std::runtime_error("Copy imported before settling");
            auto grid = window.findChild<QListView *>("gameGrid");
            if (!grid->currentIndex().data(LaunchableRole).toBool()) throw std::runtime_error("Copy check blocked existing game");
            file(root + "/Copy/assets/deep/archive.bin", "copy still growing");
            window.scan();
            if (!waitFor([&] { return lib.games().size() == 2; }, 10000)) throw std::runtime_error("Stable copy never imported");
            window.scan();
            if (!waitFor([&] { return notice->text().contains("0 new"); })) throw std::runtime_error("Rescan did not finish");
            if (lib.games().size() != 2) throw std::runtime_error("Duplicate copy imported");
            auto search = window.findChild<QLineEdit *>("librarySearch");
            search->setText("Copy");
            if (grid->model()->rowCount() != 1 || window.findChild<QLabel *>("gameTitle")->text() != "Copy") throw std::runtime_error("Search did not update selected details");
            search->setText("no matching title");
            if (grid->currentIndex().data(LaunchableRole).toBool() || window.findChild<QLabel *>("gameTitle")->text() != "Your next story") throw std::runtime_error("Empty search left stale launch controls");
            search->clear();
            // Selecting a different cover updates both list selection and the detail pane.
            grid->setCurrentIndex(grid->model()->index(1, 0));
            auto title = window.findChild<QLabel *>("gameTitle");
            if (title->text() != grid->currentIndex().data().toString()) throw std::runtime_error("Card selection did not update details");
            QString cardManageError;
            auto checkCardManage = [&] {
                auto dialog = window.findChild<QDialog *>("manageDialog");
                if (!dialog) { cardManageError = "Card did not open Manage"; return; }
                auto tabs = window.findChild<QTabWidget *>("manageTabs");
                if (!tabs || tabs->currentIndex() != 0 || tabs->tabText(0) != "Overview") cardManageError = "Manage did not default to Overview";
                const auto previewDirectory = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                if (!previewDirectory.isEmpty()) dialog->grab().save(previewDirectory + "/manage-overview.png");
                dialog->reject();
            };
            auto selectedCard = grid->visualRect(grid->currentIndex()).adjusted(5, 5, -5, -5);
            const QPoint managePoint = selectedCard.center();
            QContextMenuEvent manageEvent(QContextMenuEvent::Mouse, managePoint, grid->viewport()->mapToGlobal(managePoint));
            QCoreApplication::sendEvent(grid->viewport(), &manageEvent);
            QCoreApplication::processEvents();
            auto menu = window.findChild<QMenu *>("cardMenu");
            if (!menu || !menu->isVisible() || !menu->findChild<QAction *>("cardDeleteAction")) throw std::runtime_error("Card menu missing Manage/Delete");
            auto cardManage = menu->findChild<QAction *>("cardManageAction"); menu->hide();
            QTimer::singleShot(0, &window, checkCardManage); cardManage->trigger(); menu->deleteLater();
            if (!cardManageError.isEmpty()) throw std::runtime_error(cardManageError.toStdString());
            const QString disconnected = t.path() + "/Disconnected";
            if (!QDir().rename(root, disconnected)) throw std::runtime_error("Cannot simulate unavailable library");
            window.scan();
            auto table = window.findChild<QTableView *>();
            if (!waitFor([&] { return table->model()->index(0, 2).data().toString() == "Unavailable"; })) throw std::runtime_error("Missing library not marked unavailable");
            if (grid->currentIndex().data(LaunchableRole).toBool() || table->model()->rowCount() != 2) throw std::runtime_error("Missing library not preserved safely");
            if (!QDir().rename(disconnected, root)) throw std::runtime_error("Cannot restore library");
            window.scan();
            if (!waitFor([&] { return table->model()->index(0, 2).data().toString() != "Unavailable"; })) throw std::runtime_error("Reconnected library did not recover");
            const auto external = t.path() + "/External relink 日本語"; file(external + "/game.exe");
            if (!QDir(renamed).removeRecursively()) throw std::runtime_error("Cannot simulate transferred game");
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, 0).data().toString() == game.name) table->selectRow(row);
            QString dialogError;
            QTimer::singleShot(0, &window, [&] {
                auto manageDialog = window.findChild<QDialog *>("manageDialog");
                auto button = window.findChild<QPushButton *>("relinkFolderButton");
                if (!button || !button->isEnabled()) {
                    dialogError = "Manage relink action unavailable"; if (manageDialog) manageDialog->reject(); return;
                }
                QTimer::singleShot(0, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("relinkDialog");
                    auto folderField = window.findChild<QLineEdit *>("relinkFolder");
                    auto apply = window.findChild<QPushButton *>("applyRelink");
                    if (!dialog || !folderField || !apply) { dialogError = "Relink dialog incomplete"; if (dialog) dialog->reject(); return; }
                    folderField->setText(external);
                    auto executable = window.findChild<QLineEdit *>("relinkExecutable");
                    if (!apply->isEnabled() || executable->text() != external + "/game.exe") {
                        dialogError = "Relink preview did not map paths"; dialog->reject(); return;
                    }
                    apply->click();
                });
                button->click();
                if (manageDialog) manageDialog->reject();
            });
            window.manage();
            if (!dialogError.isEmpty()) throw std::runtime_error(dialogError.toStdString());
            if (lib.game(game.id).folder != external || !QFileInfo::exists(lib.prefix(game.id) + "/survived")) throw std::runtime_error("Manage relink did not preserve installation/prefix");
            QFile::remove(lib.prefix(game.id) + "/started"); QFile::remove(lib.prefix(game.id) + "/survived");
            window.launch();
            if (!waitFor([&] { return QFileInfo::exists(lib.prefix(game.id) + "/survived") && !lib.busy(game.id); })) throw std::runtime_error("Relinked game did not launch/exit");
            QFile paths(lib.prefix(game.id) + "/launched-paths");
            if (!paths.open(QIODevice::ReadOnly) || paths.readAll() != (external + "/game.exe\n" + external + "\n").toUtf8()) throw std::runtime_error("Relinked launch used incorrect executable or working directory");
        }
        {
            const auto profileRoot = t.path() + "/Profile UI"; file(profileRoot + "/RenPy/game.exe");
            Library profiles(profileRoot); profiles.import(discover(profileRoot), t.path() + "/runner");
            profiles.setSettings({{"umu", fake}});
            auto g = profiles.games().first(); g.engine = "Ren'Py"; g.locale = "ja_JP.UTF-8";
            g.environment["CUSTOM"] = "keep=me"; g.arguments = {"keep this argument"}; profiles.save(g);
            g = profiles.game(g.id); const auto original = g.json();
            file(profiles.prefix(g.id) + "/save.dat", "keep save");
            Window window(profileRoot, launcher); window.show(); QCoreApplication::processEvents();
            for (int pass = 0; pass < 3; ++pass) {
                QString error;
                QTimer::singleShot(0, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("manageDialog");
                    auto choice = window.findChild<QComboBox *>("setupProfile");
                    auto reset = window.findChild<QPushButton *>("resetSetupProfile");
                    auto tabs = window.findChild<QTabWidget *>("manageTabs");
                    if (!dialog || !choice || !reset || !tabs) {
                        error = "Profile controls missing"; if (dialog) dialog->reject(); return;
                    }
                    tabs->setCurrentIndex(3);
                    if (pass == 0) {
                        auto debug = window.findChild<QPushButton *>("debugLaunchButton");
                        auto exportButton = window.findChild<QPushButton *>("exportDiagnosticButton");
                        if (!debug || !debug->isEnabled() || !exportButton) error = "Diagnostic actions missing";
                        const auto reportPath = t.path() + "/profile-diagnostic.json";
                        QTimer::singleShot(0, &window, [&] {
                            auto picker = window.findChild<QFileDialog *>();
                            if (!picker) { error = "Diagnostic save dialog missing"; return; }
                            picker->selectFile(reportPath); static_cast<QDialog *>(picker)->accept();
                        });
                        exportButton->click();
                        const auto report = readJson(reportPath);
                        if (report["game"].toObject()["id"] != g.id) error = "Diagnostic export did not save the selected game";
                    }
                    if (pass < 2) reset->click(); else choice->setCurrentIndex(choice->findData("generic-proton-v1"));
                    if (choice->currentData() != (pass < 2 ? "generic-game-v1" : "generic-proton-v1")) error = "Profile selection did not apply";
                    const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                    if (pass == 1 && !preview.isEmpty()) dialog->grab().save(preview + "/manage-runtime.png");
                    if (pass == 0) dialog->reject();
                    else dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
                });
                window.manage(); if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                auto expected = original;
                if (pass == 1) expected["setup_profile"] = "generic-game-v1";
                if (pass == 2) {
                    expected["setup_profile"] = "generic-proton-v1";
                    auto overrides = expected["recipe_overrides"].toObject();
                    overrides["setup_profile"] = "generic-proton-v1";
                    expected["recipe_overrides"] = overrides;
                }
                if (profiles.game(g.id).json() != expected) throw std::runtime_error(("Profile cancel/save changed unrelated settings or failed to persist (pass " +
                    QString::number(pass) + "): actual=" + QString::fromUtf8(QJsonDocument(profiles.game(g.id).json()).toJson(QJsonDocument::Compact)) +
                    " expected=" + QString::fromUtf8(QJsonDocument(expected).toJson(QJsonDocument::Compact))).toStdString());
            }
            auto afterProfileChoice = original; afterProfileChoice["setup_profile"] = "generic-proton-v1";
            auto profileOverrides = afterProfileChoice["recipe_overrides"].toObject();
            profileOverrides["setup_profile"] = "generic-proton-v1";
            afterProfileChoice["recipe_overrides"] = profileOverrides;
            QString toolError;
            QTimer::singleShot(0, &window, [&] {
                auto action = window.findChild<QPushButton *>("winecfgButton");
                if (!action || !action->isEnabled()) {
                    toolError = "Wine configuration action unavailable";
                    if (auto dialog = window.findChild<QDialog *>("manageDialog")) dialog->reject();
                    return;
                }
                action->click();
            });
            window.manage(); if (!toolError.isEmpty()) throw std::runtime_error(toolError.toStdString());
            if (!waitFor([&] { return QFileInfo::exists(profiles.prefix(g.id) + "/started"); }) || !profiles.busy(g.id))
                throw std::runtime_error("Wine configuration was not supervised");
            if (window.findChild<QListView *>("gameGrid")->currentIndex().data(LaunchableRole).toBool())
                throw std::runtime_error("Play remained available while prefix tool was open");
            window.close();
            if (!waitFor([&] { return QFileInfo::exists(profiles.prefix(g.id) + "/survived") && !profiles.busy(g.id); }))
                throw std::runtime_error("Prefix tool failed after closing the launcher window");
            QFile paths(profiles.prefix(g.id) + "/launched-paths");
            if (!paths.open(QIODevice::ReadOnly) || paths.readAll() !=
                (profiles.prefix(g.id) + "/drive_c/windows/system32/winecfg.exe\n" + g.workingDirectory + "\n").toUtf8())
                throw std::runtime_error("Prefix tool launched the game or used the wrong prefix");
            if (profiles.game(g.id).json() != afterProfileChoice) throw std::runtime_error("Tool action changed saved game settings");
            QFile saved(profiles.prefix(g.id) + "/save.dat");
            if (!saved.open(QIODevice::ReadOnly) || saved.readAll() != "keep save") throw std::runtime_error("Profile selection changed existing saves");
            {
                Window debugWindow(profileRoot, launcher); debugWindow.show(); QCoreApplication::processEvents();
                QFile::remove(profiles.prefix(g.id) + "/started"); QFile::remove(profiles.prefix(g.id) + "/survived");
                QString debugError;
                QTimer::singleShot(0, &debugWindow, [&] {
                    auto button = debugWindow.findChild<QPushButton *>("debugLaunchButton");
                    if (!button || !button->isEnabled()) { debugError = "Debug action unavailable"; if (auto dialog = debugWindow.findChild<QDialog *>("manageDialog")) dialog->reject(); return; }
                    button->click();
                });
                debugWindow.manage(); if (!debugError.isEmpty()) throw std::runtime_error(debugError.toStdString());
                if (!waitFor([&] { return QFileInfo::exists(profiles.prefix(g.id) + "/survived") && !profiles.busy(g.id); })) throw std::runtime_error("Debug action did not complete");
                QFile debugLog(profiles.gamePath(g.id, "launch.log"));
                if (!debugLog.open(QIODevice::ReadOnly) || !debugLog.readAll().contains("One-launch Wine debug")) throw std::runtime_error("Debug UI did not mark launch log");
            }
            if (profiles.game(g.id).json() != afterProfileChoice) throw std::runtime_error("Debug action changed saved settings");
        }
        {
            const auto extraRoot = t.path() + "/Extra UI", patch = t.path() + "/Patches 日本語/patch.exe";
            file(extraRoot + "/Game/game.exe"); file(patch);
            Library extras(extraRoot); extras.setSettings({{"umu", fake}}); extras.import(discover(extraRoot), t.path() + "/runner");
            auto g = extras.games().first(); g.arguments = {"game-only"}; extras.save(g); g = extras.game(g.id);
            const auto original = g.json(); const auto prefix = extras.prefix(g.id); file(prefix + "/save.dat", "keep save");
            {
                Window window(extraRoot, launcher); window.show(); QCoreApplication::processEvents();
                for (bool confirm : {false, true}) {
                    QString error;
                    QTimer::singleShot(0, &window, [&] {
                        auto manage = window.findChild<QDialog *>("manageDialog");
                        auto action = window.findChild<QPushButton *>("runExtraButton");
                        if (!manage || !action || !action->isEnabled()) { error = "Extra executable action missing"; if (manage) manage->reject(); return; }
                        QTimer::singleShot(0, &window, [&] {
                            auto dialog = window.findChild<QDialog *>("extraLaunchDialog");
                            auto exe = window.findChild<QLineEdit *>("extraExecutable");
                            auto cwd = window.findChild<QLineEdit *>("extraCwd");
                            auto arguments = window.findChild<QPlainTextEdit *>("extraArguments");
                            auto run = window.findChild<QPushButton *>("confirmExtraLaunch");
                            if (!dialog || !exe || !cwd || !arguments || !run) { error = "Extra launch fields missing"; if (dialog) dialog->reject(); return; }
                            if (run->isEnabled()) error = "Empty executable accepted";
                            exe->setText(patch); cwd->setText("/missing/extra-cwd");
                            if (run->isEnabled()) error = "Missing working directory accepted";
                            cwd->setText(g.folder); arguments->setPlainText("argument with spaces\n--flag=value\n日本語");
                            if (!run->isEnabled()) error = "Valid one-time executable rejected";
                            const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                            if (!confirm && !preview.isEmpty()) dialog->grab().save(preview + "/run-executable.png");
                            if (!confirm || !error.isEmpty()) dialog->reject(); else run->click();
                        });
                        action->click();
                        if (manage->isVisible()) manage->reject();
                    });
                    window.manage(); if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                    if (!confirm && (QFileInfo::exists(extras.gamePath(g.id, "extra-launch.json")) || QFileInfo::exists(prefix + "/started")))
                        throw std::runtime_error("Cancelled extra executable had launch side effects");
                }
                if (!waitFor([&] { return QFileInfo::exists(prefix + "/started"); }) || !extras.busy(g.id))
                    throw std::runtime_error("Extra executable was not supervised");
                if (window.findChild<QListView *>("gameGrid")->currentIndex().data(LaunchableRole).toBool())
                    throw std::runtime_error("Play available during extra executable");
            }
            if (!waitFor([&] { return QFileInfo::exists(prefix + "/survived") && !extras.busy(g.id); }))
                throw std::runtime_error("Extra executable did not survive launcher destruction");
            QFile paths(prefix + "/launched-paths");
            if (!paths.open(QIODevice::ReadOnly) || paths.readAll() != (patch + '\n' + g.folder + '\n').toUtf8())
                throw std::runtime_error("Extra executable used saved game path/cwd");
            const auto operation = readJson(extras.gamePath(g.id, "extra-launch.json"));
            if (operation["state"] != "complete" || operation["executable"] != patch || extras.game(g.id).json() != original)
                throw std::runtime_error("Extra completion record missing or saved launch configuration changed");
            QFile save(prefix + "/save.dat"); if (!save.open(QIODevice::ReadOnly) || save.readAll() != "keep save") throw std::runtime_error("Extra UI orchestration changed saves");
        }
        {
            Window window(root, launcher); window.show();
            QString settingsError;
            QTimer::singleShot(0, &window, [&] {
                auto dialog = window.findChild<QDialog *>("settingsDialog");
                auto folder = window.findChild<QLineEdit *>("settingsGamesFolder");
                if (!dialog || !folder || folder->text() != root) settingsError = "Games folder is missing from Settings";
                auto manager = window.findChild<QPushButton *>("protonManager");
                if (!manager) settingsError = "Proton Manager action missing from Settings";
                else {
                    QTimer::singleShot(0, &window, [&] {
                        auto managerDialog = window.findChild<QDialog *>("protonManagerDialog");
                        auto install = window.findChild<QPushButton *>("installManagedGe");
                        if (!managerDialog || !install) { settingsError = "Settings did not open Proton Manager"; return; }
                        QTimer::singleShot(0, &window, [&] {
                            auto geDialog = window.findChild<QDialog *>("geInstallerDialog");
                            if (!geDialog) settingsError = "Proton Manager did not open the GE installer";
                            else {
                                const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                                if (!preview.isEmpty()) geDialog->grab().save(preview + "/ge-installer.png");
                                geDialog->reject();
                            }
                            managerDialog->accept();
                        });
                        install->click();
                    });
                    manager->click();
                }
                auto getUmu = window.findChild<QPushButton *>("getUmuSettings");
                if (!getUmu) settingsError = "umu installer action missing from Settings";
                else {
                    QTimer::singleShot(0, &window, [&] {
                        auto umuDialog = window.findChild<QDialog *>("umuInstallerDialog");
                        if (!umuDialog) { settingsError = "Settings did not open the umu installer"; return; }
                        const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                        if (!preview.isEmpty()) umuDialog->grab().save(preview + "/umu-installer.png");
                        umuDialog->reject();
                    });
                    getUmu->click();
                }
                if (dialog) dialog->reject();
            });
            window.settings(); if (!settingsError.isEmpty()) throw std::runtime_error(settingsError.toStdString());
            // Removal only ever touches temporary fixtures; first cancel, then explicitly confirm.
            const auto target = lib.games().first();
            auto grid = window.findChild<QListView *>("gameGrid");
            for (int row = 0; row < grid->model()->rowCount(); ++row)
                if (grid->model()->index(row, 0).data().toString() == target.name) grid->setCurrentIndex(grid->model()->index(row, 0));
            file(lib.prefix(target.id) + "/save.dat", "fixture save");
            QString removeError;
            auto removeFromManage = [&](bool confirmRemoval) {
                QTimer::singleShot(0, &window, [&, confirmRemoval] {
                    auto manage = window.findChild<QDialog *>("manageDialog");
                    auto action = window.findChild<QPushButton *>("removeGameButton");
                    if (!manage || !action) { removeError = "Removal action missing"; if (manage) manage->reject(); return; }
                    QTimer::singleShot(0, &window, [&, confirmRemoval] {
                        auto dialog = window.findChild<QMessageBox *>("removeGameDialog");
                        if (!dialog) { removeError = "Removal confirmation missing"; return; }
                        auto synced = dialog->findChild<QCheckBox *>("deleteSyncedSaves");
                        auto yes = dialog->button(QMessageBox::Yes), no = dialog->button(QMessageBox::No);
                        if (!yes || !no || !synced) { removeError = "Removal buttons or checkbox missing"; dialog->reject(); return; }
                        if (synced->isEnabled() || synced->isChecked() ||
                            dialog->defaultButton() != no || !dialog->text().contains(target.name) ||
                            !dialog->informativeText().contains("local saves"))
                            removeError = "Yes/No deletion warning or future synced-saves option is incorrect";
                        const auto previewDirectory = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                        if (!confirmRemoval && !previewDirectory.isEmpty()) dialog->grab().save(previewDirectory + "/removal-confirmation.png");
                        if (!confirmRemoval || !removeError.isEmpty()) { no->click(); return; }
                        yes->click();
                    });
                    action->click();
                    if (manage->isVisible()) manage->reject();
                });
                window.manage(); if (!removeError.isEmpty()) throw std::runtime_error(removeError.toStdString());
            };
            removeFromManage(false);
            if (!QFileInfo::exists(target.folder) || !QFileInfo::exists(lib.prefix(target.id) + "/save.dat")) throw std::runtime_error("Cancelled removal changed files");
            removeFromManage(true);
            if (QFileInfo::exists(target.folder) || QFileInfo::exists(lib.prefix(target.id)) || lib.games().size() != 1) throw std::runtime_error("Full removal did not delete fixture game/prefix/entry");
        }
        {
            auto steamGame = lib.games().first(); steamGame.backend = "steam"; lib.save(steamGame);
            Window window(root, launcher); window.show(); QString guardError;
            QTimer::singleShot(0, &window, [&] {
                auto dialog = window.findChild<QDialog *>("manageDialog");
                auto remove = window.findChild<QPushButton *>("removeGameButton");
                if (!dialog || !remove || remove->isEnabled() || !remove->toolTip().contains("cannot confirm"))
                    guardError = "Steam removal is not visibly blocked in Manage";
                for (const auto name : {"winecfgButton", "regeditButton", "runExtraButton"}) {
                    auto action = window.findChild<QPushButton *>(name);
                    if (!action || action->isEnabled()) guardError = "Steam prefix tool was enabled";
                }
                if (dialog) dialog->reject();
            });
            window.manage();
            if (!guardError.isEmpty()) throw std::runtime_error(guardError.toStdString());
            steamGame.backend = "umu"; lib.save(steamGame);
        }
        {
            const auto g = lib.games().first(); QFile boot("/proc/sys/kernel/random/boot_id");
            if (!boot.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read fixture boot identity");
            const auto marker = lib.gamePath(g.id, "runtime.json");
            writeJson(marker, {{"schema", 1}, {"boot_id", QString::fromUtf8(boot.readAll()).trimmed()}});
            Window window(root, launcher); window.show();
            auto grid = window.findChild<QListView *>("gameGrid");
            const auto index = grid->currentIndex();
            if (index.data(LaunchableRole).toBool() || index.data(ManageableRole).toBool() ||
                !window.findChild<QLabel *>("gameStatus")->text().contains("Recovery required"))
                throw std::runtime_error("Recovery barrier did not disable affected game actions");
            window.launch();
            if (!window.findChild<QLabel *>("libraryNotice")->text().contains("Restart"))
                throw std::runtime_error("Recovery action did not explain restart requirement");
            window.manage();
            if (!window.findChild<QLabel *>("libraryNotice")->text().contains("Restart"))
                throw std::runtime_error("Manage did not explain recovery barrier");
            QFile::remove(marker); // Only this synthetic fixture record; never touch user state.
        }
        {
            const auto late = t.path() + "/NotMountedYet";
            Window window(late, launcher); window.show();
            QDir().mkpath(late); window.scan();
            auto notice = window.findChild<QLabel *>("libraryNotice");
            if (!waitFor([&] { return notice->text().contains("0 games"); })) throw std::runtime_error("Unavailable startup root cannot reconnect through Rescan");
        }
        {
            // An unmounted drive can leave an empty directory at the mount point.
            const auto mount = t.path() + "/MountPoint";
            QDir().mkpath(mount);
            setAppSetting("gamesRoot", mount);
            Window window({}, launcher); window.show(); window.scan();
            if (QFileInfo::exists(mount + "/.novelty")) throw std::runtime_error("Missing remembered library was recreated at empty mount point");
            { Library restored(mount); }
            window.scan();
            auto notice = window.findChild<QLabel *>("libraryNotice");
            if (!waitFor([&] { return notice->text().contains("0 games"); })) throw std::runtime_error("Remembered library did not recover after reconnect");
        }
        {
            const auto previewRoot = t.path() + "/GridLayout";
            for (int i = 0; i < 9; ++i) file(previewRoot + QString("/Story %1/game.exe").arg(i));
            Library gridLibrary(previewRoot); gridLibrary.import(discover(previewRoot), t.path() + "/runner");
            Window window(previewRoot, launcher); window.show();
            auto notice = window.findChild<QLabel *>("libraryNotice");
            if (!waitFor([&] { return notice->text().contains("9 games") && notice->text().contains("0 new"); })) throw std::runtime_error("Grid fixture scan did not finish");
            auto grid = window.findChild<QListView *>("gameGrid");
            const auto first = grid->visualRect(grid->model()->index(0, 0));
            const auto third = grid->visualRect(grid->model()->index(2, 0));
            if (third.y() != first.y() || !grid->viewport()->rect().contains(third)) throw std::runtime_error("Third card is clipped or wrapped at Deck resolution");
            for (int row = 0; row < 3; ++row) {
                const auto index = grid->model()->index(row, 0);
                if (grid->indexAt(grid->visualRect(index).center()) != index) throw std::runtime_error("Visible card hit testing lost an entry");
            }
            const auto previewDirectory = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
            if (!previewDirectory.isEmpty() && !window.grab().save(previewDirectory + "/layout-grid-fixture.png")) throw std::runtime_error("Cannot save full grid preview");
            if (!previewDirectory.isEmpty()) {
                window.findChild<QToolButton *>("listViewButton")->click(); QCoreApplication::processEvents();
                window.grab().save(previewDirectory + "/layout-list-fixture.png");
                window.findChild<QToolButton *>("gridViewButton")->click();
            }
            const auto last = grid->model()->index(8, 0); grid->scrollTo(last); QCoreApplication::processEvents();
            if (!grid->viewport()->rect().contains(grid->visualRect(last))) {
                const auto r = grid->visualRect(last);
                throw std::runtime_error(QString("Last card cannot be scrolled into view: %1,%2 %3x%4 in %5x%6")
                    .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(grid->viewport()->width()).arg(grid->viewport()->height()).toStdString());
            }
            // Card Delete uses the same Yes/No confirmation, including without a Manage parent.
            for (bool confirm : {false, true}) {
                const auto index = grid->model()->index(8, 0); const auto title = index.data().toString();
                const auto rect = grid->visualRect(index).adjusted(5, 5, -5, -5);
                const QPoint point = rect.center();
                QContextMenuEvent context(QContextMenuEvent::Mouse, point, grid->viewport()->mapToGlobal(point));
                QCoreApplication::sendEvent(grid->viewport(), &context);
                auto menu = window.findChild<QMenu *>("cardMenu");
                if (!menu || !menu->isVisible()) throw std::runtime_error("Delete menu unavailable");
                auto remove = menu->findChild<QAction *>("cardDeleteAction"); menu->hide();
                QString error;
                QTimer::singleShot(0, &window, [&] {
                    auto dialog = window.findChild<QMessageBox *>("removeGameDialog");
                    if (!dialog) { error = "Card Delete skipped confirmation"; return; }
                    auto synced = dialog->findChild<QCheckBox *>("deleteSyncedSaves");
                    if (!dialog->button(QMessageBox::Yes) || !dialog->button(QMessageBox::No)) {
                        error = "Card Delete buttons missing"; dialog->reject(); return;
                    }
                    if (!dialog->text().contains(title) || !synced || synced->isEnabled() || synced->isChecked() ||
                        dialog->defaultButton() != dialog->button(QMessageBox::No)) error = "Card Delete warning is incorrect";
                    if (confirm && error.isEmpty()) dialog->button(QMessageBox::Yes)->click();
                    else dialog->button(QMessageBox::No)->click();
                });
                remove->trigger(); delete menu;
                if (!error.isEmpty() || gridLibrary.games().size() != (confirm ? 8 : 9)) throw std::runtime_error("Card Delete confirmation failed");
            }
        }
        {
            const auto recoveryRoot = t.path() + "/RemovalRecovery"; file(recoveryRoot + "/Game/game.exe");
            Library recovery(recoveryRoot); recovery.import(discover(recoveryRoot), t.path() + "/runner");
            const auto g = recovery.games().first(); file(recovery.prefix(g.id) + "/save.dat", "keep my save");
            QProcess crash; crash.start(QString::fromLocal8Bit(argv[2]), {"remove", recoveryRoot, g.id, "prefix-staged"});
            if (!crash.waitForFinished(3000) || crash.exitCode() != 88) throw std::runtime_error("Cannot create removal crash fixture");
            Window window(recoveryRoot, launcher); window.show();
            auto grid = window.findChild<QListView *>("gameGrid");
            if (!waitFor([&] {
                return QFileInfo::exists(g.executable) && grid->currentIndex().data(ManageableRole).toBool() &&
                    window.findChild<QLabel *>("libraryNotice")->text().contains("Restored interrupted removal");
            })) throw std::runtime_error("Startup worker did not restore interrupted removal and refresh library");
            QFile save(recovery.prefix(g.id) + "/save.dat");
            if (!save.open(QIODevice::ReadOnly) || save.readAll() != "keep my save") throw std::runtime_error("Startup recovery lost fixture save");
        }
        {
            const auto stateRoot = t.path() + "/TargetedUpdates";
            file(stateRoot + "/First/game.exe"); file(stateRoot + "/Second/game.exe");
            Library states(stateRoot); states.import(discover(stateRoot), t.path() + "/runner");
            const auto g = states.games().first();
            GameModel model(nullptr); model.load(states);
            QSortFilterProxyModel proxy; proxy.setSourceModel(&model);
            QTableView view; view.setModel(&proxy); view.selectRow(1);
            int resets = 0, changes = 0;
            QObject::connect(&model, &QAbstractItemModel::modelReset, &view, [&] { ++resets; });
            QObject::connect(&model, &QAbstractItemModel::dataChanged, &view, [&](const QModelIndex &first, const QModelIndex &last) {
                if (first.row() != 0 || last.row() != 0) throw std::runtime_error("Poll changed unrelated rows");
                ++changes;
            });
            states.recordStatus(g.id, "Running");
            QLockFile lock(states.gamePath(g.id, "launch.lock"));
            if (!lock.tryLock()) throw std::runtime_error("Cannot lock targeted update fixture");
            if (!model.updateState(states, g.id) || model.index(0, 0).data(LaunchableRole).toBool())
                throw std::runtime_error("Targeted update failed to disable running game");
            if (model.updateState(states, g.id) || resets != 0 || changes != 1 || view.currentIndex().row() != 1)
                throw std::runtime_error("Unchanged polling reset/repainted model or lost selection");
            lock.unlock(); states.recordStatus(g.id, "Ready");
            if (!model.updateState(states, g.id) || !model.index(0, 0).data(ManageableRole).toBool() || resets != 0 || changes != 2)
                throw std::runtime_error("Stopped game did not update actions without model reset");
        }
        {
            const auto geRoot = t.path() + "/GE UI"; QDir().mkpath(geRoot);
            const auto helper = t.path() + "/fake-ge-installer.py";
            file(helper, R"PY(import json, pathlib, sys, time
root = pathlib.Path(sys.argv[1])
config = json.loads(pathlib.Path(sys.argv[3]).read_text())
version = config['version']
if version == 'GE-Proton0-0':
    (root / 'waiting').touch()
    time.sleep(20)
else:
    time.sleep(.15)
target = root / '.novelty/runners' / version
target.mkdir(parents=True)
(target / 'proton').write_text('fixture')
(target / 'version').write_text(version)
print(target)
)PY");
            QWidget parent; QString error; int ticks = 0;
            QTimer heartbeat; heartbeat.setInterval(10); QObject::connect(&heartbeat, &QTimer::timeout, &parent, [&] { ++ticks; }); heartbeat.start();
            QTimer watchdog; watchdog.setSingleShot(true); watchdog.setInterval(4000);
            QObject::connect(&watchdog, &QTimer::timeout, &parent, [&] {
                error = "GE dialog did not finish";
                if (auto dialog = parent.findChild<QDialog *>("geInstallerDialog")) dialog->reject();
            });
            auto start = [&](const QString &version) {
                QTimer::singleShot(0, &parent, [&, version] {
                    auto field = parent.findChild<QLineEdit *>("geVersion");
                    auto install = parent.findChild<QPushButton *>("installGeButton");
                    if (!field || !install) { error = "GE controls missing"; return; }
                    field->setText(version); install->click();
                });
            };
            start("GE-Proton11-7"); watchdog.start();
            const auto installed = provisionGe(&parent, geRoot, helper); watchdog.stop();
            if (!error.isEmpty() || installed != geRoot + "/.novelty/runners/GE-Proton11-7" || ticks < 2)
                throw std::runtime_error("GE request failed or blocked UI event processing");
            if (!QDir(geRoot + "/.novelty").entryList({"ge-request-*.json"}, QDir::Files).isEmpty())
                throw std::runtime_error("Completed GE request configuration was not cleaned up");
            QTimer cancel; cancel.setInterval(10);
            QObject::connect(&cancel, &QTimer::timeout, &parent, [&] {
                if (QFileInfo::exists(geRoot + "/waiting")) {
                    cancel.stop(); parent.findChild<QDialog *>("geInstallerDialog")->reject();
                }
            });
            start("GE-Proton0-0"); cancel.start(); watchdog.start();
            const auto cancelled = provisionGe(&parent, geRoot, helper); watchdog.stop(); cancel.stop();
            if (!error.isEmpty() || !cancelled.isEmpty() || QFileInfo::exists(geRoot + "/.novelty/runners/GE-Proton0-0"))
                throw std::runtime_error("Cancelled GE request selected or installed a runner");
            if (!QFileInfo::exists(installed + "/proton")) throw std::runtime_error("Cancellation changed a previously installed version");
        }
        {
            const auto umuRoot = t.path() + "/umu UI"; QDir().mkpath(umuRoot);
            const auto helper = t.path() + "/fake-umu-installer.py";
            file(helper, R"PY(import pathlib, sys, time
root = pathlib.Path(sys.argv[1])
if (root / 'fail').exists():
    print('Fixture download failed', file=sys.stderr)
    sys.exit(2)
if (root / 'cancel').exists():
    (root / 'waiting').touch()
    time.sleep(20)
time.sleep(.15)
target = root / '.novelty/tools/umu-1.4.4/umu-run'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text('#!/bin/sh\n')
target.chmod(0o755)
print(target)
)PY");
            QWidget parent; QString error; int ticks = 0; bool retried = false;
            QTimer heartbeat; heartbeat.setInterval(10); QObject::connect(&heartbeat, &QTimer::timeout, &parent, [&] { ++ticks; }); heartbeat.start();
            QTimer watchdog; watchdog.setSingleShot(true); watchdog.setInterval(4000);
            QObject::connect(&watchdog, &QTimer::timeout, &parent, [&] {
                error = "umu dialog did not finish";
                if (auto dialog = parent.findChild<QDialog *>("umuInstallerDialog")) dialog->reject();
            });
            auto start = [&] {
                QTimer::singleShot(0, &parent, [&] {
                    auto install = parent.findChild<QPushButton *>("installUmuButton");
                    if (!install) { error = "umu controls missing"; return; } install->click();
                });
            };
            file(umuRoot + "/fail");
            QTimer retry; retry.setInterval(10);
            QObject::connect(&retry, &QTimer::timeout, &parent, [&] {
                auto status = parent.findChild<QLabel *>("umuInstallStatus");
                if (!status || !status->text().contains("Fixture download failed")) return;
                auto install = parent.findChild<QPushButton *>("installUmuButton");
                if (!install->isEnabled()) { error = "Failed umu install cannot retry"; return; }
                retry.stop(); retried = true; QFile::remove(umuRoot + "/fail"); install->click();
            });
            start(); retry.start(); watchdog.start();
            const auto installed = provisionUmu(&parent, umuRoot, helper); watchdog.stop(); retry.stop();
            if (!error.isEmpty() || !retried || installed != managedUmuPath(umuRoot) || ticks < 2)
                throw std::runtime_error("umu failure/retry blocked the UI or selected an invalid result");
            file(umuRoot + "/cancel"); QTimer cancel; cancel.setInterval(10);
            QObject::connect(&cancel, &QTimer::timeout, &parent, [&] {
                if (QFileInfo::exists(umuRoot + "/waiting")) {
                    cancel.stop(); parent.findChild<QDialog *>("umuInstallerDialog")->reject();
                }
            });
            start(); cancel.start(); watchdog.start();
            const auto cancelled = provisionUmu(&parent, umuRoot, helper); watchdog.stop(); cancel.stop();
            if (!error.isEmpty() || !cancelled.isEmpty() || resolveProgram(installed).isEmpty())
                throw std::runtime_error("Cancelled umu installation selected a result or changed the previous version");
        }
        {
            const auto metadataRoot = t.path() + "/Metadata Games"; file(metadataRoot + "/Visual Novel/game.exe");
            Library library(metadataRoot); library.import(discover(metadataRoot), t.path() + "/runner");
            const auto entry = library.games().first();
            MetadataFixture fixture;
            const QUrl api(QString("http://127.0.0.1:%1/kana/").arg(fixture.serverPort()));
            {
                Window window(metadataRoot, launcher, api); window.show(); QCoreApplication::processEvents();
                QString error; int phase = 0; QElapsedTimer elapsed; elapsed.start();
                QTimer poll; poll.setInterval(10);
                QObject::connect(&poll, &QTimer::timeout, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("manageDialog");
                    if (!dialog) return;
                    if (elapsed.elapsed() > 5000) { error = "Metadata link timed out"; poll.stop(); dialog->reject(); return; }
                    if (phase == 0) {
                        auto tabs = dialog->findChild<QTabWidget *>("manageTabs");
                        if (!tabs || tabs->tabText(1) != "Metadata") { error = "Metadata tab missing"; poll.stop(); dialog->reject(); return; }
                        tabs->setCurrentIndex(1); dialog->findChild<QPushButton *>("metadataSearch")->click(); phase = 1;
                    } else if (phase == 1) {
                        auto results = dialog->findChild<QListWidget *>("metadataResults");
                        if (results && results->count()) { dialog->findChild<QPushButton *>("metadataLink")->click(); phase = 2; }
                    } else if (library.game(entry.id).metadata["id"] == "v123") {
                        const auto value = [&](const char *name) { return dialog->findChild<QLabel *>(name)->text(); };
                        if (value("metadataOriginalTitle") != "原題" || value("metadataExpectedTime") != "About 3.0 hours" ||
                            value("metadataDevelopers") != "Studio A" || !value("metadataPublishers").contains("Publisher A") ||
                            !value("metadataPublishers").contains("Publisher B") || value("metadataContentTags") != "Mystery" ||
                            !value("metadataVndbId").contains("https://vndb.org/v123") ||
                            dialog->findChild<QPlainTextEdit *>("metadataDescription")->toPlainText() != "A story about choices.")
                            error = "Metadata detail fields are incomplete or unfiltered";
                        const auto preview = qEnvironmentVariable("NOVELTY_UI_PREVIEW_DIR");
                        if (!preview.isEmpty()) { QCoreApplication::processEvents(); dialog->grab().save(preview + "/manage-metadata.png"); }
                        poll.stop(); dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
                    }
                });
                poll.start(); window.manage(); poll.stop();
                if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
            }
            const auto saved = library.game(entry.id);
            const auto coverPath = metadataCoverPath(library.dataRoot(), entry.id);
            if (saved.metadata["id"] != "v123" || !QFileInfo::exists(coverPath) || QImage(coverPath).isNull() || fixture.requests != 5)
                throw std::runtime_error("VNDB search/link, release pagination or cover cache failed");
            {
                Window offline(metadataRoot, launcher, api); offline.show(); QCoreApplication::processEvents();
                if (offline.findChild<QLabel *>("gameOriginalTitle")->text() != "原題" ||
                    offline.findChild<QLabel *>("metadataPlaytime")->text() != "About 3.0 hours" ||
                    !offline.findChild<QLabel *>("gameDescription")->text().contains("A story"))
                    throw std::runtime_error("Cached metadata did not appear offline in sidebar");
                if (fixture.requests != 5) throw std::runtime_error("Startup fetched metadata without request");
                QTimer::singleShot(0, &offline, [&] {
                    auto dialog = offline.findChild<QDialog *>("manageDialog");
                    dialog->findChild<QPushButton *>("metadataUnlink")->click(); dialog->reject();
                });
                offline.manage();
            }
            if (!library.game(entry.id).metadata.isEmpty() || QFileInfo::exists(coverPath) || library.game(entry.id).executable != entry.executable)
                throw std::runtime_error("Unlink did not clear only metadata and its cover");
        }
        {
            const auto autoRoot = t.path() + "/Automatic Metadata Games";
            const auto source = t.path() + "/English title";
            file(source + "/play.exe"); QDir().mkpath(autoRoot);
            MetadataFixture fixture;
            const QUrl api(QString("http://127.0.0.1:%1/kana/").arg(fixture.serverPort()));
            Window window(autoRoot, launcher, api); window.show();
            QTimer::singleShot(0, &window, [&] {
                window.findChild<QLineEdit *>("addGameFolder")->setText(source);
                window.findChild<QPushButton *>("addGameSubmit")->click();
            });
            window.addGame();
            Library library(autoRoot);
            if (!waitFor([&] {
                const auto games = library.games();
                return games.size() == 1 && games.first().metadata["id"] == "v123";
            })) throw std::runtime_error("Add did not link VNDB metadata automatically");
            const auto game = library.games().first();
            const auto coverPath = metadataCoverPath(library.dataRoot(), game.id);
            if (!QFileInfo::exists(coverPath) || QImage(coverPath).isNull() || fixture.requests != 5)
                throw std::runtime_error("Automatic metadata did not cache artwork or release details");
            Window offline(autoRoot, launcher, QUrl("http://127.0.0.1:1/kana/"));
            offline.show(); QCoreApplication::processEvents();
            if (offline.findChild<QLabel *>("gameOriginalTitle")->text() != "原題" || fixture.requests != 5)
                throw std::runtime_error("Automatically linked metadata was not available offline");
        }
        std::puts("PASS: grid/list actions, profiles, runtime installers, metadata, Overview, Settings, removal recovery, search, launch survival, discovery, relink and reconnection"); return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
