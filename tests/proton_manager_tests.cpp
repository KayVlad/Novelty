#include "proton_manager.h"
#include "app_settings.h"
#include "provisioning.h"
#include "recipes.h"
#include "runner_fixture.h"
#include "window.h"
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qscrollbar.h>
#include <QtWidgets/qcheckbox.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qthread.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace novelty;
namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void put(const QString &path, const QByteArray &bytes = {}) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture mkdir");
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(), "fixture write");
}
QByteArray get(const QString &path) { QFile file(path); require(file.open(QIODevice::ReadOnly), "fixture read"); return file.readAll(); }
bool waitFor(const std::function<bool()> &test, int limit = 5000) {
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < limit) { QCoreApplication::processEvents(); if (test()) return true; QThread::msleep(10); }
    return false;
}
void externalLocations(const QString &base) {
    const auto root = base + "/Library", collection = base + "/External";
    QDir().mkpath(root); Library library(root);
    put(collection + "/GE-Proton80-9/proton"); put(collection + "/Proton 9.0/proton");
    require(!installedRunners(root).contains(collection + "/GE-Proton80-9"), "unregistered launcher directories are not scanned");
    addExternalProtonLocation(collection);
    require(QFile::link(collection, base + "/Alias"), "external directory alias");
    addExternalProtonLocation(base + "/Alias");
    addExternalProtonLocation(collection + "/GE-Proton80-9");
    require(externalProtonLocations().size() == 2 && installedRunners(root).count(collection + "/GE-Proton80-9") == 1,
            "canonical locations and overlapping registrations deduplicate installations");
    require(resolveRunnerSelector(root, "ge-proton-latest") == collection + "/GE-Proton80-9" &&
            resolveRunnerSelector(root, "proton-latest") == collection + "/Proton 9.0", "aliases use registered versions");
    put(collection + "/Proton Experimental/proton");
    require(resolveRunnerSelector(root, "proton-experimental") == collection + "/Proton Experimental", "experimental selector retained");
    bool rejected = false; try { addExternalProtonLocation(base); } catch (const std::exception &) { rejected = true; }
    require(rejected, "non-Proton directories rejected");
    put(root + "/Game/game.exe"); library.import({inspect(root + "/Game")}, {});
    auto game = library.games().first(); game.selectRunner(collection + "/GE-Proton80-9"); library.save(game);
    require(QDir().rename(collection + "/GE-Proton80-9", collection + "/Disconnected"), "hide selected external version");
    require(library.game(game.id).runner == collection + "/GE-Proton80-9" && library.status(library.game(game.id)) == "Runner unavailable",
            "missing custom paths remain pinned and are reported");
    setAppSetting("autoGeUpdates", false);
    {
        Window window(root); window.show(); QCoreApplication::processEvents(); window.launch();
        const auto notice = window.findChild<QLabel *>("libraryNotice");
        require(notice && notice->text().contains("missing or unverified") && notice->text().contains(collection + "/GE-Proton80-9"),
                "Play reports the missing pinned external path without replacing the reason with a cancellation notice");
    }
    setAppSetting("autoGeUpdates", true);
    removeExternalProtonLocation(collection); removeExternalProtonLocation(collection + "/GE-Proton80-9");
    require(QFileInfo::exists(collection + "/Disconnected/proton") && QFileInfo::exists(collection + "/Proton 9.0/proton"),
            "unregistering never removes external files");
    require(!libraryPresetDocument(library.settings(), "portable", root).contains("externalProtonPaths"), "registrations do not enter portable presets");
}
void externalOwnership(const QString &base) {
    const auto root = base + "/LinkedLibrary", external = base + "/OwnedElsewhere";
    QDir().mkpath(root); Library library(root);
    managedRunnerFixture(external + "/GE-Proton80-1");
    require(QFile::link(external, library.dataRoot() + "/runners"), "linked runner parent fixture");
    addExternalProtonLocation(external);
    require(installedRunners(root).contains(external + "/GE-Proton80-1"), "linked installation is usable through its external registration");
    bool rejected = false;
    try { removeManagedRunner(library, "GE-Proton80-1"); } catch (const std::exception &) { rejected = true; }
    require(rejected && QFileInfo::exists(external + "/GE-Proton80-1/proton"), "a linked runner parent never grants Novelty deletion ownership");
    removeExternalProtonLocation(external);
}

void releaseListScrolling(const QString &base) {
    const auto root = base + "/Scrolling", catalog = base + "/older-catalog.json";
    QDir().mkpath(root); Library library(root);
    QList<GeRelease> cached;
    for (int i = 80; i > 20; --i) cached.append({"GE-Proton95-" + QString::number(i), {}, 1});
    cacheGeCatalog(root, cached);
    QJsonArray olderEntries;
    for (int i = 20; i > 0; --i) {
        const auto version = "GE-Proton95-" + QString::number(i);
        olderEntries.append(QJsonObject{{"tag_name", version}, {"assets", QJsonArray{
            QJsonObject{{"name", version + ".tar.gz"}, {"size", 1}}, QJsonObject{{"name", version + ".sha512sum"}}}}});
    }
    put(catalog, QJsonDocument(olderEntries).toJson());
    GeUpdateManager updates(root, nullptr, QUrl::fromLocalFile(catalog));
    QWidget parent;
    QString error;
    QTimer::singleShot(0, &parent, [&] {
        auto dialog = parent.findChild<QDialog *>("protonManagerDialog");
        try {
            require(dialog, "manager opened for scrolling test");
            auto list = dialog->findChild<QListWidget *>("managedGeList");
            auto older = dialog->findChild<QPushButton *>("olderGeReleases");
            auto automatic = dialog->findChild<QCheckBox *>("automaticGeUpdates");
            require(list && older && automatic && automatic->text() == "Automatic check", "manager keeps catalog actions and concise automatic checkbox");
            require(!dialog->findChild<QPushButton *>("defaultManagedGe") && !dialog->findChild<QPushButton *>("followLatestGe") &&
                    !dialog->findChild<QPushButton *>("removeManagedGe"), "obsolete selected-release actions are absent");
            list->setCurrentRow(0); list->doItemsLayout();
            auto scroll = list->verticalScrollBar(); scroll->setValue(scroll->maximum() - 15);
            const auto position = scroll->value();
            require(position > 0, "release fixture scrolls");
            older->click();
            require(scroll->value() == position, "starting Load older preserves scroll position");
            require(waitFor([&] { return list->count() == 80; }), "older catalog entries append to the list");
            QCoreApplication::processEvents();
            require(scroll->value() == position, "Load older preserves position after the asynchronous catalog and layout update");
        } catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
        if (dialog) dialog->accept();
    });
    manageProton(&parent, library, updates);
    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
}

void jsonRunnerDefault(const QString &base) {
    const auto root = base + "/JSON runner default"; put(root + "/Game/game.exe"); Library library(root);
    const auto older = library.dataRoot() + "/runners/GE-Proton97-1", newer = library.dataRoot() + "/runners/GE-Proton97-2";
    managedRunnerFixture(older); managedRunnerFixture(newer);
    library.setSettings({{"umu", "/bin/true"}, {"runner", ""}});
    library.import({inspect(root + "/Game", root)}, {});
    const auto game = library.games().first();
    writeJson(library.dataRoot() + "/ge-proton.json", {{"schema", 1}, {"version", "GE-Proton97-1"}, {"auto_download", false}});
    const auto automatic = loadAppSettings()["autoGeUpdates"].toBool(); setAppSetting("autoGeUpdates", false);
    {
        Window window(root, "/bin/true"); window.show(); QCoreApplication::processEvents(); window.launch();
        require(library.game(game.id).runner == newer && library.game(game.id).runnerSelector == "ge-proton-latest",
                "library JSON delegates to latest installed GE without an old installer pin shadowing its choice");
    }
    setAppSetting("autoGeUpdates", automatic);
}

const char helper[] = R"PY(import fcntl,json,os,pathlib,sys,time
root=pathlib.Path(sys.argv[1]); config=json.loads(pathlib.Path(sys.argv[3]).read_text()); version=config['version']
runners=root/'.novelty/runners'; runners.mkdir(parents=True,exist_ok=True)
with (runners/('.'+version+'.install.lock')).open('a+b') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 stage=runners/('.install-'+version); stage.mkdir()
 (stage/'proton').write_text('#!/bin/sh\n'); (stage/'proton').chmod(0o700)
 (root/'.novelty/update-started').write_text(version)
 while not (root/'.novelty/release-update').exists(): time.sleep(.01)
 if (root/'.novelty/fail-update').exists(): sys.exit(1)
 (stage/'version').write_text(version)
 (stage/'novelty-install.json').write_text(json.dumps(dict(schema=1,version=version,architecture='x86_64',sha512='a'*128,size=1)))
 os.rename(stage,runners/version)
 print(runners/version)
)PY";
void updatesAndPlay(const QString &base) {
    const auto root = base + "/Games"; QDir().mkpath(root); Library library(root);
    const auto old = library.dataRoot() + "/runners/GE-Proton90-2", fresh = library.dataRoot() + "/runners/GE-Proton90-3";
    managedRunnerFixture(old);
    put(library.dataRoot() + "/runners/GE-Proton90-99/proton", "incomplete unverified payload");
    require(resolveRunnerSelector(root, "ge-proton-latest") == old, "unverified higher versions never enter selection");
    require(QFile::link(library.dataRoot() + "/runners/GE-Proton90-99", base + "/PartialAlias") &&
            !runnerInstallationReady(root, base + "/PartialAlias"), "an alias cannot expose an unverified managed runner");
    put(root + "/Following/game.exe"); put(root + "/Pinned/game.exe");
    library.import({inspect(root + "/Following"), inspect(root + "/Pinned")}, {});
    auto games = library.games(); auto following = games.first(), pinned = games.last();
    following.selectRunner("ge-proton-latest"); pinned.selectRunner(old); library.save(following); library.save(pinned);
    const auto umu = base + "/fake-umu";
    put(umu, "#!/bin/sh\nmkdir -p \"$WINEPREFIX\"\ntouch \"$WINEPREFIX/system.reg\"\nif [ \"$2\" != \"/c\" ]; then printf '%s' \"$PROTONPATH\" > runner-used; fi\n");
    QFile::setPermissions(umu, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    library.setSettings({{"umu", umu}, {"runner", "ge-proton-latest"}});
    require(library.settings()["runner"] == "ge-proton-latest", "library alias survives settings serialization");
    const auto script = base + "/install-fixture.py", catalog = base + "/catalog.json";
    put(script, helper);
    QJsonArray entries;
    for (const auto &version : {QString("GE-Proton90-1"), QString("GE-Proton90-3"), QString("GE-Proton90-2")})
        entries.append(QJsonObject{{"tag_name", version}, {"assets", QJsonArray{QJsonObject{{"name", version + ".tar.gz"}, {"size", 1}},
                                                                              QJsonObject{{"name", version + ".sha512sum"}}}}});
    put(catalog, QJsonDocument(entries).toJson());
    GeUpdateManager updates(root, nullptr, QUrl::fromLocalFile(catalog), script);
    require(updates.automaticUpdates(), "automatic updates enabled by default");
    QElapsedTimer clock; clock.start(); updates.checkAtStartup();
    require(clock.elapsed() < 250, "startup update check is asynchronous");
    require(waitFor([&] { return QFileInfo::exists(library.dataRoot() + "/update-started"); }), "background install starts");
    require(updates.installingVersion() == "GE-Proton90-3" && !installedRunners(root).contains(fresh), "in-progress version is visible but not selectable");
    following = library.game(following.id);
    clock.restart();
    require(provisionFirstPlay(nullptr, library, following, "ge-proton-latest") && clock.elapsed() < 500 && following.runner == old,
            "first-Play setup reuses the verified older version without joining an update");
    require(supervise(library, following.id, false) == 0 && get(following.folder + "/runner-used") == old.toUtf8(),
            "headless launch immediately uses the older fully installed version during update");
    require(!QFileInfo::exists(fresh), "launch did not release or wait for the background download");
    const auto beforeUpdate = library.game(following.id).json();
    auto edited = beforeUpdate; edited["name"] = "Changed after update";
    const auto saves = library.gamePath(following.id, "saves.json");
    const auto journal = library.gamePath(following.id, "settings-change.json");
    writeJson(saves, {{"marker", "uncommitted"}});
    writeJson(journal, {{"schema", 1}, {"before_game", beforeUpdate}, {"after_game", edited},
                        {"had_config", true}, {"before_config", QJsonObject{{"marker", "original"}}},
                        {"after_config", QJsonObject{{"marker", "uncommitted"}}}});
    put(library.dataRoot() + "/release-update");
    require(waitFor([&] { return updates.installingVersion().isEmpty(); }) && runnerInstallationReady(root, fresh), "verified update is published");
    require(library.game(following.id).runner == fresh && library.game(pinned.id).runner == old && QFileInfo::exists(old + "/proton"),
            "floating selections advance while exact selections and old installations are retained");
    require(library.game(following.id).json() == beforeUpdate, "background updates do not change persisted game settings or recovery identities");
    library.recoverSettingsChanges();
    require(!QFileInfo::exists(journal) && readJson(saves)["marker"] == "original", "settings recovery remains correct across a completed background update");
    require(supervise(library, following.id, false) == 0 && get(following.folder + "/runner-used") == fresh.toUtf8(), "next launch sees the complete new version");
    GeUpdateManager second(root, nullptr, QUrl::fromLocalFile(base + "/does-not-exist"), script);
    second.checkAtStartup(); require(!second.checking() && second.status().contains("last day"), "startup checks are limited to once per day");
    bool directInstall = false;
    QTimer::singleShot(0, &updates, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QDialog *>(widget)) {
            auto button = dialog->findChild<QPushButton *>("installGe_GE-Proton90-1");
            if (!button || !button->isEnabled()) continue;
            button->click();
            directInstall = !dialog->findChild<QLineEdit *>("geVersion") && updates.installingVersion() == "GE-Proton90-1";
            dialog->accept(); break;
        }
    });
    manageProton(nullptr, library, updates);
    require(directInstall && waitFor([&] { return updates.installingVersion().isEmpty(); }) &&
            runnerInstallationReady(root, library.dataRoot() + "/runners/GE-Proton90-1"), "release-row installation is one click and continues after closing Manager");
    const auto exported = gameRecipeDocument(library.game(following.id), "floating", root);
    require(exported["settings"].toObject()["runner"] == "ge-proton-latest", "floating preference remains portable without external registrations");
    put(base + "/ExternalExisting/GE-Proton90-3/proton"); addExternalProtonLocation(base + "/ExternalExisting");
    QDir(fresh).removeRecursively(); QFile::remove(library.dataRoot() + "/update-started");
    updates.install("GE-Proton90-3");
    require(updates.installingVersion().isEmpty() && !QFileInfo::exists(library.dataRoot() + "/update-started"), "registered official versions avoid duplicate downloads");
    removeExternalProtonLocation(base + "/ExternalExisting");
    updates.setAutomaticUpdates(false);
    GeUpdateManager disabled(root, nullptr, QUrl::fromLocalFile(catalog), script); disabled.checkAtStartup();
    require(!disabled.checking() && disabled.status().contains("disabled"), "disabled startup updates make no request");
}
void interruptedUpdates(const QString &base) {
    const auto script = base + "/install-fixture.py", catalog = base + "/catalog.json";
    for (bool cancel : {false, true}) {
        const auto root = base + (cancel ? "/Cancelled" : "/Failed"); QDir().mkpath(root); Library library(root);
        const auto old = library.dataRoot() + "/runners/GE-Proton90-2";
        managedRunnerFixture(old);
        setAppSetting("autoGeUpdates", true);
        GeUpdateManager updates(root, nullptr, QUrl::fromLocalFile(catalog), script);
        updates.checkAtStartup();
        require(waitFor([&] { return QFileInfo::exists(library.dataRoot() + "/update-started"); }), "interrupt fixture starts");
        GeUpdateManager concurrent(root, nullptr, QUrl::fromLocalFile(catalog), script);
        concurrent.install("GE-Proton90-1");
        require(concurrent.installingVersion().isEmpty() && concurrent.status().contains("Another Novelty"), "concurrent managers cannot start overlapping operations");
        if (cancel) updates.setAutomaticUpdates(false);
        else { put(library.dataRoot() + "/fail-update"); put(library.dataRoot() + "/release-update"); }
        require(waitFor([&] { return updates.installingVersion().isEmpty(); }), "failed or cancelled background operation releases its lock");
        require(!QFileInfo::exists(library.dataRoot() + "/runners/GE-Proton90-3") &&
                resolveRunnerSelector(root, "ge-proton-latest") == old, "failure and cancellation leave old versions ready and partial files unselectable");
        require(!geInstallInProgress(root) && QDir(library.dataRoot()).entryList({"ge-request-*"}, QDir::Files).isEmpty(),
                "interrupted update closes installation locks and removes its temporary request");
    }
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); QCoreApplication::setApplicationName("NoveltyProtonTests");
    QTemporaryDir temp; if (!temp.isValid()) return 1;
    qputenv("HOME", QFile::encodeName(temp.path() + "/home"));
    qputenv("NOVELTY_TEST_APP_DATA_DIR", QFile::encodeName(temp.path() + "/app"));
    QTimer watchdog; watchdog.setSingleShot(true); QObject::connect(&watchdog, &QTimer::timeout, &app, [] { std::fprintf(stderr, "FAIL: Proton Manager fixture stalled\n"); std::_Exit(2); }); watchdog.start(20000);
    try { externalLocations(temp.path()); externalOwnership(temp.path()); releaseListScrolling(temp.path()); jsonRunnerDefault(temp.path()); updatesAndPlay(temp.path()); interruptedUpdates(temp.path()); std::puts("PASS: Proton Manager inventory, background updates and immediate launch"); return 0; }
    catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
