#include "core.h"
#include "saves.h"
#include "save_server_store.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <cstdio>
using namespace novelty;
void put(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size())
        throw std::runtime_error("fixture write");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc != 3) return 2;
        const auto root = QString::fromLocal8Bit(argv[1]), mode = QString::fromLocal8Bit(argv[2]);
        Library lib(root);
        if (mode == "setup") {
            QDir().mkpath(root + "/Game/save"); put(root + "/Game/game.exe", "exe");
            lib.import({inspect(root + "/Game")}, {});
            const auto game = lib.games().first();
            auto config = loadSaveConfig(lib, game.id); config.mappings = {{"game", "save"}};
            saveSaveConfig(lib, game.id, config); put(root + "/Game/save/slot", "old");
            const auto first = createSaveSnapshot(lib, game.id);
            writeJson(root + "/fixture.json", {{"snapshot", first.id}});
            put(root + "/Game/save/slot", "new"); createSaveSnapshot(lib, game.id);
            SaveServerStore server(root + "/Server");
            return 0;
        }
        const auto game = lib.games().first();
        const auto snapshot = readJson(root + "/fixture.json")["snapshot"].toString();
        if (mode == "inventory") inspectVerifiedLiveSaveState(lib, game.id);
        else if (mode == "restore") restoreSaveSnapshot(lib, game.id, snapshot);
        else if (mode == "snapshot") { put(root + "/Game/save/slot", "third"); createSaveSnapshot(lib, game.id); }
        else if (mode == "recover") {
            recoverSaveRestores(lib);
            if (!lib.recoveryIssue(game.id).isEmpty()) throw std::runtime_error("recovery still required");
            for (const auto &entry : listSaveSnapshots(lib, game.id)) verifiedSaveSnapshotBundle(lib, game.id, entry.id);
        } else if (mode == "server") {
            SaveServerStore server(root + "/Server");
            server.put("game", 0, QFileInfo(lib.gamePath(game.id, "saves.json")).absolutePath() + "/snapshots/" + snapshot);
        } else if (mode == "server-recover") {
            SaveServerStore server(root + "/Server"); const auto revision = server.current("game");
            if (revision.revision != 0 && revision.revision != 1) throw std::runtime_error("bad recovered revision");
            if (revision.revision) verifyPortableSaveBundle(revision.bundleDirectory);
        } else return 2;
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
