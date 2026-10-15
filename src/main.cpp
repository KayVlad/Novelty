#include "core.h"
#include "window.h"
#include "saves.h"
#include "save_server_config.h"
#include "save_server_sync.h"
#include "app_settings.h"
#include <QtWidgets/qapplication.h>
#include <QtCore/qcommandlineparser.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qsettings.h>
#include <QtCore/qtimer.h>
#include <QtGui/qimage.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {
void configurePortableSettings() {
    QSettings legacy; legacy.setFallbacksEnabled(false);
    const auto legacyFile = legacy.fileName();
    const auto legacyKeys = legacy.allKeys();
    const auto appData = novelty::appDataDirectory();
    if (QFileInfo(appData).isSymbolicLink() || !QDir().mkpath(appData))
        throw std::runtime_error("Novelty needs a writable data folder beside its executable.");
    for (const auto &name : {"cache", "tmp"}) {
        const auto path = appData + '/' + name;
        if (QFileInfo(path).isSymbolicLink() || !QDir().mkpath(path))
            throw std::runtime_error("Novelty needs writable application-local cache and temporary folders.");
    }
    qputenv("XDG_CACHE_HOME", QFile::encodeName(appData + "/cache"));
    qputenv("TMPDIR", QFile::encodeName(appData + "/tmp"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, appData);
    QSettings portable; portable.setFallbacksEnabled(false);
    const auto portableFile = portable.fileName();
    const auto portableKeys = portable.allKeys();
    if (!QFileInfo::exists(novelty::appSettingsPath())) {
        QJsonObject settings;
        for (const auto &key : {"gamesRoot", "libraryView"}) {
            if (portable.contains(key)) settings[key] = portable.value(key).toString();
            else if (legacy.contains(key)) settings[key] = legacy.value(key).toString();
        }
        novelty::saveAppSettings(settings);
    }
    if (QFileInfo::exists(portableFile) &&
        std::all_of(portableKeys.begin(), portableKeys.end(), [](const QString &key) {
            return key == "gamesRoot" || key == "libraryView";
        })) QFile::remove(portableFile);
    if (legacyFile != portable.fileName() && QFileInfo::exists(legacyFile) &&
        std::all_of(legacyKeys.begin(), legacyKeys.end(), [](const QString &key) {
            return key == "gamesRoot" || key == "libraryView";
        })) {
        QFile::remove(legacyFile); // Only remove an old file containing the two migrated Novelty preferences.
    }
}
}

int main(int argc, char **argv) {
    const auto started = std::chrono::steady_clock::now();
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const auto option = QByteArray(argv[i]).split('=').first();
        if (option == "--install" || option == "--debug-wine" || option == "--run" || option == "--prepare" || option == "--sync-server-game" || option == "--prefix-tool" || option == "--peer-checked" || option == "--approve-winetricks" || option.startsWith("--extra-") || option == "--list" || option == "--version" || option == "--help") headless = true;
    }
    std::unique_ptr<QCoreApplication> app;
    if (headless) app = std::make_unique<QCoreApplication>(argc, argv);
    else app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setOrganizationName("Novelty"); QCoreApplication::setApplicationName("Novelty"); QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser; parser.setApplicationDescription("A focused visual novel launcher for Linux and Steam Deck.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"root", "Main Games folder", "path"});
    parser.addOption({"run", "Launch a saved installation without opening the UI", "id"});
    parser.addOption({"novelty-shortcut", "Steam shortcut identity marker", "identity"});
    parser.addOption({"install", "Run the installer for a pending installation", "id"});
    parser.addOption({"prepare", "Prepare a saved installation without opening the UI", "id"});
    parser.addOption({"sync-server-game", "Retry pending server saves for a saved installation", "id"});
    parser.addOption({"prefix-tool", "Open winecfg or regedit in the --run installation's prefix", "tool"});
    parser.addOption({"extra-exe", "Run a Windows .exe once in the --run installation's prefix", "path"});
    parser.addOption({"extra-cwd", "Working directory for --extra-exe (defaults to its folder)", "path"});
    parser.addOption({"extra-arg", "One literal argument for --extra-exe; repeat as needed", "argument"});
    parser.addOption({"debug-wine", "Enable Wine loader/exception traces for this --run only"});
    parser.addOption({"approve-winetricks", "Approval fingerprint for the pending recipe components", "fingerprint"});
    parser.addOption({"peer-checked", "The window already checked reachable peers before this launch"});
    parser.addOption({"list", "Print installation IDs, names and statuses"});
    parser.addOption({"screenshot", "Render the library window to a PNG and exit (development)", "path"});
    parser.addOption({"startup-report", "Report time until the first UI event (development)"});
    parser.process(*app);
    try {
        if (headless) {
            if (!parser.isSet("root")) throw std::runtime_error("--root is required for headless commands.");
            if (int(parser.isSet("run")) + int(parser.isSet("prepare")) + int(parser.isSet("list")) + int(parser.isSet("install")) + int(parser.isSet("sync-server-game")) != 1 ||
                (parser.isSet("prefix-tool") && (!parser.isSet("run") || parser.value("prefix-tool").isEmpty())) ||
                (parser.isSet("peer-checked") && !parser.isSet("run")) ||
                (parser.isSet("approve-winetricks") && !(parser.isSet("run") || parser.isSet("prepare") || parser.isSet("install"))))
                throw std::runtime_error("Choose exactly one of --run, --prepare, --install, --list or --sync-server-game. --prefix-tool requires --run and a tool name.");
            std::optional<novelty::ExtraLaunch> extra;
            if (parser.isSet("extra-exe") || parser.isSet("extra-cwd") || parser.isSet("extra-arg")) {
                if (!parser.isSet("run") || !parser.isSet("extra-exe") || parser.value("extra-exe").isEmpty() || parser.isSet("prefix-tool"))
                    throw std::runtime_error("--extra-exe requires --run and cannot be combined with --prefix-tool. Extra arguments/cwd require --extra-exe.");
                extra = novelty::ExtraLaunch{parser.value("extra-exe"), parser.value("extra-cwd"), parser.values("extra-arg")};
            }
            if (parser.isSet("debug-wine") && (!parser.isSet("run") || parser.isSet("prefix-tool") || extra))
                throw std::runtime_error("--debug-wine requires an ordinary --run game launch.");
            novelty::Library lib(parser.value("root"));
            if (parser.isSet("sync-server-game")) {
                const auto config = novelty::loadSaveServerConfig(lib);
                if (!config.enabled) return 0;
                const auto result = novelty::retryPendingServerSave(lib, parser.value("sync-server-game"), config);
                return result.status == novelty::ServerSyncStatus::Synced ? 0 :
                       result.status == novelty::ServerSyncStatus::Conflict ? 3 : 4;
            }
            if (parser.isSet("list")) {
                for (auto g : lib.games()) std::printf("%s\t%s\t%s\n", qPrintable(g.id), qPrintable(g.name), qPrintable(lib.status(g)));
                return 0;
            }
            return novelty::supervise(lib, parser.value(parser.isSet("install") ? "install" : parser.isSet("prepare") ? "prepare" : "run"), parser.isSet("prepare"), parser.value("prefix-tool"), extra, parser.isSet("install"), parser.isSet("debug-wine"), parser.value("approve-winetricks"));
        }
        configurePortableSettings();
        // The command-line folder seeds first launch; Settings owns subsequent GUI launches.
        const auto savedRoot = novelty::loadAppSettings()["gamesRoot"].toString();
        novelty::Window window(savedRoot.isEmpty() ? parser.value("root") : QString()); window.show();
        if (parser.isSet("startup-report")) QTimer::singleShot(0, &window, [&] {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "Novelty ready: %lld ms\n", static_cast<long long>(ms)); std::fflush(stderr);
        });
        if (parser.isSet("screenshot")) QTimer::singleShot(800, &window, [&] {
            QImage image(window.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); window.render(&image);
            app->exit(image.save(parser.value("screenshot")) ? 0 : 1);
        });
        return app->exec();
    } catch (const std::exception &e) { std::fprintf(stderr, "Novelty: %s\n", e.what()); return 1; }
}
