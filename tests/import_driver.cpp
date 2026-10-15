#include "core.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qfileinfo.h>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc != 3) return 2;
        novelty::Library library(QString::fromLocal8Bit(argv[1]));
        const auto result = library.addFolder(QString::fromLocal8Bit(argv[2]));
        const auto saved = library.game(result.game.id);
        if (saved.folder != result.game.folder || !QFileInfo::exists(saved.executable)) return 3;
        std::printf("%s\n%s\n", qPrintable(saved.folder), qPrintable(result.warning));
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
