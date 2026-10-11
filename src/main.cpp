#include "core.h"
#include "window.h"
#include "saves.h"
#include "lan_sync.h"
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
    const auto appData = QCoreApplication::applicationDirPath() + "/data";
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
    for (const auto &key : {"gamesRoot", "libraryView"})
        if (!portable.contains(key) && legacy.contains(key)) portable.setValue(key, legacy.value(key));
    portable.sync();
    if (portable.status() != QSettings::NoError)
        throw std::runtime_error("Novelty cannot save settings beside its executable.");
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
        if (option == "--install" || option == "--debug-wine" || option == "--run" || option == "--prepare" || option == "--prefix-tool" || option == "--peer-checked" || option.startsWith("--extra-") || option == "--list" || option == "--version" || option == "--help") headless = true;
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
    parser.addOption({"prefix-tool", "Open winecfg or regedit in the --run installation's prefix", "tool"});
    parser.addOption({"extra-exe", "Run a Windows .exe once in the --run installation's prefix", "path"});
    parser.addOption({"extra-cwd", "Working directory for --extra-exe (defaults to its folder)", "path"});
    parser.addOption({"extra-arg", "One literal argument for --extra-exe; repeat as needed", "argument"});
    parser.addOption({"debug-wine", "Enable Wine loader/exception traces for this --run only"});
    parser.addOption({"peer-checked", "The window already checked reachable peers before this launch"});
    parser.addOption({"list", "Print installation IDs, names and statuses"});
    parser.addOption({"screenshot", "Render the library window to a PNG and exit (development)", "path"});
    parser.addOption({"startup-report", "Report time until the first UI event (development)"});
    parser.process(*app);
    try {
        if (headless) {
            if (!parser.isSet("root")) throw std::runtime_error("--root is required for headless commands.");
            if (int(parser.isSet("run")) + int(parser.isSet("prepare")) + int(parser.isSet("list")) + int(parser.isSet("install")) != 1 ||
                (parser.isSet("prefix-tool") && (!parser.isSet("run") || parser.value("prefix-tool").isEmpty())) ||
                (parser.isSet("peer-checked") && !parser.isSet("run")))
                throw std::runtime_error("Choose exactly one of --run, --prepare, --install or --list. --prefix-tool requires --run and a tool name.");
            std::optional<novelty::ExtraLaunch> extra;
            if (parser.isSet("extra-exe") || parser.isSet("extra-cwd") || parser.isSet("extra-arg")) {
                if (!parser.isSet("run") || !parser.isSet("extra-exe") || parser.value("extra-exe").isEmpty() || parser.isSet("prefix-tool"))
                    throw std::runtime_error("--extra-exe requires --run and cannot be combined with --prefix-tool. Extra arguments/cwd require --extra-exe.");
                extra = novelty::ExtraLaunch{parser.value("extra-exe"), parser.value("extra-cwd"), parser.values("extra-arg")};
            }
            if (parser.isSet("debug-wine") && (!parser.isSet("run") || parser.isSet("prefix-tool") || extra))
                throw std::runtime_error("--debug-wine requires an ordinary --run game launch.");
            novelty::Library lib(parser.value("root"));
            if (parser.isSet("list")) {
                for (auto g : lib.games()) std::printf("%s\t%s\t%s\n", qPrintable(g.id), qPrintable(g.name), qPrintable(lib.status(g)));
                return 0;
            }
            if (parser.isSet("run") && !parser.isSet("prefix-tool") && !extra &&
                lib.recoveryIssue(parser.value("run")).isEmpty() &&
                !lib.supervisorActive(parser.value("run"))) {
                const auto id = parser.value("run");
                if (!parser.isSet("peer-checked")) {
                    const auto refresh = novelty::refreshLinkedPeerSaves(lib, id);
                    for (const auto &issue : refresh.issues)
                        std::fprintf(stderr, "Novelty peer sync: %s\n", qPrintable(issue));
                }
                const auto candidates = novelty::importedSaveCandidates(lib, id);
                QList<novelty::ImportedSaveCandidate> newer;
                bool conflict = false;
                for (const auto &candidate : candidates) {
                    if (candidate.relation == "conflict") conflict = true;
                    else if (candidate.relation == "ahead" || candidate.relation == "available") newer.append(candidate);
                }
                if (conflict || newer.size() > 1)
                    throw std::runtime_error("Paired-device saves conflict. Open Novelty → Manage → Saves to choose a copy before Play.");
                if (newer.size() == 1) novelty::restoreSaveSnapshot(lib, id, newer.first().snapshotId);
            }
            return novelty::supervise(lib, parser.value(parser.isSet("install") ? "install" : parser.isSet("prepare") ? "prepare" : "run"), parser.isSet("prepare"), parser.value("prefix-tool"), extra, parser.isSet("install"), parser.isSet("debug-wine"));
        }
        configurePortableSettings();
        novelty::Window window(parser.value("root")); window.show();
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
