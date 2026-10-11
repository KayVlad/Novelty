#include "core.h"
#include <QtCore/qcoreapplication.h>
#include <cstdio>
#include <unistd.h>

// Test-only entry point. Deterministic abrupt exit at real removal progress boundaries.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc < 3) return 2;
        novelty::Library lib(QString::fromLocal8Bit(argv[2]));
        if (QString::fromLocal8Bit(argv[1]) == "recover") {
            for (const auto &message : lib.recoverRemovals()) std::puts(qPrintable(message));
            return 0;
        }
        if (argc != 5 || QString::fromLocal8Bit(argv[1]) != "remove") return 2;
        const auto stop = QString::fromLocal8Bit(argv[4]);
        const auto result = lib.remove(lib.planRemoval(QString::fromLocal8Bit(argv[3])), [&](const QString &phase) {
            if (phase == stop) _exit(88);
        });
        return result.recoveryMessage.isEmpty() ? 0 : 1;
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
