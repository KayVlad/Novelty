#include "setup.h"
#include "core.h"
#include "playtime.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qjsondocument.h>
#include <cstdio>
using namespace novelty;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc < 4) return 2;
        const auto action = QString::fromLocal8Bit(argv[1]), root = QString::fromLocal8Bit(argv[2]), id = QString::fromLocal8Bit(argv[3]);
        Library library(root);
        if (action == "approval") { std::puts(qPrintable(winetricksApproval(library, library.game(id)))); return 0; }
        if (action == "relink" && argc == 5) library.relink(library.planRelink(id, QString::fromLocal8Bit(argv[4])));
        else if (action == "runner" && argc == 5) {
            auto game = library.game(id); game.selectRunner(QString::fromLocal8Bit(argv[4])); library.save(game);
        } else if (action != "stats") return 2;
        const auto stats = readPlaytime(root, id);
        const QJsonObject result{{"total_ms", double(stats.totalMs)}, {"sessions", double(stats.sessions)},
            {"foreground_ms", double(stats.foregroundMs)}, {"incomplete_foreground", stats.incompleteForeground}, {"active", stats.active}, {"current_ms", double(stats.currentMs)},
            {"last_played", stats.lastPlayed.toString(Qt::ISODateWithMs)}};
        const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::puts(bytes.constData()); return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
