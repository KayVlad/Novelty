#include "core.h"
#include "saves.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <cstdio>
#include <stdexcept>

using namespace novelty;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4) { std::fprintf(stderr, "Usage: real_save_acceptance <Games root> <game folder> <Proton directory>\n"); return 2; }
    try {
        Library lib(QString::fromLocal8Bit(argv[1]));
        const auto folder = QString::fromLocal8Bit(argv[2]);
        const auto runner = QString::fromLocal8Bit(argv[3]);
        if (lib.games().isEmpty()) lib.import({inspect(folder)}, runner);
        if (lib.games().size() != 1) throw std::runtime_error("Use a disposable Games folder containing only the test game.");
        auto game = lib.games().first();
        game.runner = runner;
        game.executable = folder + "/Tenpure.exe";
        game.workingDirectory = folder;
        game.setupProfile = "generic-proton-v1";
        lib.save(game);
        const auto saveDir = folder + "/game/saves";
        if (!QFileInfo(saveDir + "/1-1-LT1.save").isFile())
            throw std::runtime_error("Expected Template!! test slot is missing. Nothing was removed.");
        auto config = loadSaveConfig(lib, game.id);
        config.gameIdentity = "vndb:v22357";
        config.releaseIdentity = "Template!! test copy";
        config.mappings = {{"game", "game/saves"}};
        saveSaveConfig(lib, game.id, config);
        const auto snapshot = createSaveSnapshot(lib, game.id, "real-game acceptance baseline");
        if (snapshot.files < 1) throw std::runtime_error("Snapshot contains no save files. Nothing was removed.");
        if (!QDir(saveDir).removeRecursively()) throw std::runtime_error("Cannot clear disposable save folder for restore test.");
        restoreSaveSnapshot(lib, game.id, snapshot.id);
        if (!QFileInfo(saveDir + "/1-1-LT1.save").isFile())
            throw std::runtime_error("Restored slot is missing.");
        std::printf("Ready: game=%s snapshot=%s restored_slot=%s\n", qPrintable(game.id), qPrintable(snapshot.id),
                    qPrintable(saveDir + "/1-1-LT1.save"));
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
