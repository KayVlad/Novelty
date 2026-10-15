#include "save_server_client.h"
#include <QtCore/qcoreapplication.h>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc != 4) return 2;
        novelty::SaveServerClient client(QUrl(QString::fromLocal8Bit(argv[1])), "test-token");
        client.download("game", QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]).toInt());
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
