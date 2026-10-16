// Private-session-bus fixture. No compositor or user's desktop is modified.
#include <QtCore/qcoreapplication.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qtimer.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusmessage.h>
#include <QtDBus/qdbuspendingcall.h>
#include <QtDBus/qdbusvirtualobject.h>
#include <cstdio>
class Mock final : public QDBusVirtualObject {
public:
    QString destination;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        QVariantList reply;
        if (m.member() == "GetActive") {
            QFile state(qEnvironmentVariable("NOVELTY_TEST_FOCUS_STATE")); state.open(QIODevice::ReadOnly);
            reply << QJsonDocument::fromJson(state.readAll()).object()["locked"].toBool();
        } else if (m.member() == "supportInformation") reply << "KWin version: 6.0.0\n";
        else if (m.member() == "loadScript") {
            QFile f(m.arguments().first().toString()); if (!f.open(QIODevice::ReadOnly)) return false;
            destination = QRegularExpression("org\\.novelty\\.Focus\\.p[0-9]+").match(QString::fromUtf8(f.readAll())).captured();
            reply << 1;
        } else if (m.member() == "unloadScript") { destination.clear(); reply << true; }
        else if (m.member() != "run") return false;
        bus.send(m.createReply(reply)); return true;
    }
    void report() {
        if (destination.isEmpty()) return;
        QFile f(qEnvironmentVariable("NOVELTY_TEST_FOCUS_STATE"));
        if (!f.open(QIODevice::ReadOnly)) return;
        auto state = QJsonDocument::fromJson(f.readAll()).object();
        if (state["stop"].toBool()) return;
        if (state.contains("pid_file")) {
            QFile pid(state["pid_file"].toString()); if (pid.open(QIODevice::ReadOnly)) state["pid"] = pid.readAll().trimmed().toInt();
        }
        auto message = QDBusMessage::createMethodCall(destination,"/Focus","org.novelty.Focus","Changed");
        message.setArguments({QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact))});
        QDBusConnection::sessionBus().asyncCall(message);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv); Mock mock;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.kde.KWin") || !bus.registerService("org.freedesktop.ScreenSaver") || !bus.registerVirtualObject("/", &mock, QDBusConnection::SubPath)) return 2;
    QTimer poll; poll.setInterval(80); QObject::connect(&poll,&QTimer::timeout,&mock,[&]{ mock.report(); }); poll.start();
    std::puts("ready"); std::fflush(stdout); return app.exec();
}
