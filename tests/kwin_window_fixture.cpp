// Opt-in real-KWin acceptance fixture; never opened by the headless CTest suite.
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qlabel.h>
#include <QtCore/qfile.h>
#include <QtCore/qtimer.h>
#include <QtCore/qdir.h>
#include <QtCore/qcoreapplication.h>
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    QLabel first("Novelty foreground acceptance — first window"), second("Novelty foreground acceptance — second window");
    first.setWindowTitle("Novelty foreground fixture A"); second.setWindowTitle("Novelty foreground fixture B");
    first.resize(450,160); second.resize(450,160); first.show(); second.show();
    QFile pid(QDir::currentPath()+"/pid"); if (!pid.open(QIODevice::WriteOnly)) return 2;
    pid.write(QByteArray::number(app.applicationPid())); pid.close();
    QTimer poll; poll.setInterval(40); QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        if (QFile::exists(QDir::currentPath()+"/release")) app.quit();
    }); poll.start(); return app.exec();
}
