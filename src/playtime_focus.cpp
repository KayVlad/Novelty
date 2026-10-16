#include "playtime_focus.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qprocess.h>
#include <QtCore/qtemporaryfile.h>
#include <QtCore/quuid.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusconnectioninterface.h>
#include <QtDBus/qdbusmessage.h>
#include <QtDBus/qdbuserror.h>
#include <QtDBus/qdbusreply.h>
#include <QtDBus/qdbusvirtualobject.h>
#include <algorithm>
#include <time.h>
#include <unistd.h>

namespace novelty {
void FocusLedger::invalidate() { known_ = foreground_ = false; lastMono_ = lastBoot_ = -1; }
void FocusLedger::observe(qint64 mono, qint64 boot, const QDateTime &utc, bool known, bool foreground, const QTimeZone &zone) {
    const auto delta = mono - lastMono_;
    // A stalled bridge or a suspend boundary makes that interval unverified.
    // CLOCK_BOOTTIME is used only to detect suspend, never as a duration counter.
    if (lastMono_ >= 0 && delta >= 0 && delta <= 1500 &&
        std::abs(boot - lastBoot_ - delta) <= 50 && known_ && known) {
        observedMs += delta;
        if (foreground_) {
            foregroundMs += delta;
            auto cursor = lastUtc_;
            qint64 remaining = delta;
            while (remaining > 0) {
                const auto local = cursor.toTimeZone(zone);
                const auto midnight = local.date().addDays(1).startOfDay(zone);
                const auto part = std::min(remaining, std::max<qint64>(1, cursor.msecsTo(midnight)));
                days[local.date()] += part;
                cursor = cursor.addMSecs(part); remaining -= part;
            }
        }
    }
    lastMono_ = mono; lastBoot_ = boot; lastUtc_ = utc;
    known_ = known; foreground_ = known && foreground;
}
namespace {
struct ProcessIdentity { qint64 parent = 0; QString ticks; };
ProcessIdentity identity(qint64 pid) {
    QFile file("/proc/" + QString::number(pid) + "/stat");
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).simplified().split(' ');
    if (fields.size() < 20 || fields[0] == "Z" || fields[0] == "X") return {};
    return {fields[1].toLongLong(), QString::fromLatin1(fields[19])};
}
qint64 monoNow() { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return qint64(ts.tv_sec)*1000 + ts.tv_nsec/1000000; }
QString jsString(const QString &text) { return QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact)).mid(1).chopped(1); }
}
QString supervisedWindowProcess(qint64 pid, qint64 monitor) {
    if (pid <= 1 || pid == monitor) return {};
    const auto first = identity(pid);
    auto current = first;
    for (int depth = 0; depth < 128 && !current.ticks.isEmpty(); ++depth) {
        if (current.parent == monitor) {
            // Recheck the window owner after the walk; avoid a disappearing/reused PID.
            return identity(pid).ticks == first.ticks ? first.ticks : QString();
        }
        if (current.parent <= 1 || current.parent == pid) break;
        current = identity(current.parent);
    }
    return {};
}
bool WindowOwners::verify(const QString &id, qint64 pid, const QString &ticks) {
    if (id.isEmpty() || pid <= 1 || ticks.isEmpty()) return false;
    auto found = owners_.constFind(id);
    if (found != owners_.cend()) return found.value() == qMakePair(pid, ticks);
    if (owners_.size() >= 4096) return false;
    owners_.insert(id, {pid, ticks}); return true;
}
bool gamescopeSession() {
    const auto desktop = (qgetenv("XDG_CURRENT_DESKTOP") + ':' + qgetenv("XDG_SESSION_DESKTOP")).toLower();
    return desktop.contains("gamescope") || qgetenv("STEAM_GAMEPADUI") == "1" || !qgetenv("GAMESCOPE_WAYLAND_DISPLAY").isEmpty();
}
class FocusMonitor::Impl final : public QDBusVirtualObject {
public:
    Impl(QFile &log, Observation observation)
        : log_(log), observation_(std::move(observation)), name_("novelty-focus-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          bus_(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name_)) {
        diagnostics_ = qgetenv("NOVELTY_GAMESCOPE_DIAGNOSTICS") == "1";
        if (gamescopeSession()) { status_ = "Gamescope: foreground tracking unavailable"; announce(); return; }
        if (!bus_.isConnected() || !bus_.interface()) { announce(); return; }
        bus_.interface()->setTimeout(300);
        owner_ = bus_.interface()->serviceOwner("org.kde.KWin").value();
        if (owner_.isEmpty()) { announce(); return; }
        auto info = call("/KWin", "org.kde.KWin", "supportInformation");
        if (info.type() == QDBusMessage::ErrorMessage || info.arguments().isEmpty() ||
            !info.arguments().first().toString().contains("KWin version: 6.")) { announce(); return; }
        service_ = "org.novelty.Focus.p" + QString::number(::getpid());
        if (!bus_.registerService(service_) || !bus_.registerVirtualObject("/Focus", this)) { announce(); return; }
        script_.setFileTemplate(QDir::tempPath() + "/novelty-kwin-XXXXXX.js");
        if (!script_.open()) { announce(); return; }
        // KDE's documented JS scripting API, no title/executable/app-id guessing.
        // Poll is a paced asynchronous callback; Changed reports focus/minimize edges.
        const auto source = QString(R"JS(
(function() {
    var service = %1, plugin = %2;
    function snapshot() {
        var w = workspace.activeWindow;
        if (!w) return JSON.stringify({known:true, active:false, minimized:false, pid:0});
        return JSON.stringify({known:typeof w.active === 'boolean' && typeof w.minimized === 'boolean',
            active:w.active, minimized:w.minimized, pid:w.pid, id:String(w.internalId),
            hidden:w.hidden === true || w.hiddenByShowDesktop === true,
            appId:String(w.desktopFileName || w.resourceClass || '')});
    }
    function poll() {
        callDBus(service, '/Focus', 'org.novelty.Focus', 'Poll', snapshot(), function(again) {
            if (again === true) poll();
        });
    }
    function changed() {
        callDBus('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'NameHasOwner', service, function(alive) {
            if (!alive) callDBus('org.kde.KWin', '/Scripting', 'org.kde.kwin.Scripting', 'unloadScript', plugin);
        });
        callDBus(service, '/Focus', 'org.novelty.Focus', 'Changed', snapshot());
    }
    function watch(w) {
        w.minimizedChanged.connect(changed);
        if (w.hiddenChanged) w.hiddenChanged.connect(changed);
        if (w.hiddenByShowDesktopChanged) w.hiddenByShowDesktopChanged.connect(changed);
    }
    workspace.windowList().forEach(watch);
    workspace.windowAdded.connect(function(w) { watch(w); changed(); });
    workspace.windowRemoved.connect(changed);
    workspace.windowActivated.connect(changed);
    poll();
})();
)JS").arg(jsString(service_), jsString(name_));
        script_.write(source.toUtf8()); script_.flush();
        auto loaded = call("/Scripting", "org.kde.kwin.Scripting", "loadScript", {script_.fileName(), name_});
        if (loaded.type() == QDBusMessage::ErrorMessage || loaded.arguments().isEmpty() || loaded.arguments().first().toInt() < 0) { announce(); return; }
        scriptLoaded_ = true;
        auto running = call("/Scripting/Script" + QString::number(loaded.arguments().first().toInt()), "org.kde.kwin.Script", "run");
        if (running.type() == QDBusMessage::ErrorMessage) { announce(); return; }
        status_ = "KWin: waiting for verified window state"; announce();
    }
    ~Impl() override {
        if (pending_.type() == QDBusMessage::MethodCallMessage) bus_.send(pending_.createReply(false));
        if (scriptLoaded_) call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", {name_});
        bus_.unregisterObject("/Focus");
        QDBusConnection::disconnectFromBus(name_);
    }
    QString introspect(const QString &) const override {
        return "<interface name=\"org.novelty.Focus\"><method name=\"Poll\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
               "<method name=\"Changed\"><arg type=\"s\" direction=\"in\"/></method></interface>";
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        if (message.interface() != "org.novelty.Focus" || (message.member() != "Poll" && message.member() != "Changed")) return false;
        if (message.service() != owner_ || message.arguments().size() != 1 || !scriptLoaded_) {
            connection.send(message.createErrorReply(QDBusError::AccessDenied, "Only this session's KWin may report focus.")); return true;
        }
        const auto state = QJsonDocument::fromJson(message.arguments().first().toString().toUtf8()).object();
        const auto pid = state["pid"].toInteger();
        const auto ticks = supervisedWindowProcess(pid, ::getpid());
        const bool stateKnown = lockKnown_ && state["known"].isBool() && state["known"].toBool() && state["active"].isBool() &&
            state["minimized"].isBool() && (!state["active"].toBool() || pid > 1);
        // A reused PID can never inherit this window’s earlier process binding.
        const bool associated = windowOwners_.verify(state["id"].toString(), pid, ticks);
        const bool known = stateKnown && (ticks.isEmpty() || associated);
        const bool foreground = known && !locked_ && state["active"].toBool() && !state["minimized"].toBool() &&
            !state["hidden"].toBool() && associated;
        lastReport_ = monoNow();
        observation_(known, foreground);
        status_ = known ? "KWin: foreground observation available" : "KWin: window identity/state unknown";
        if (message.member() == "Poll") {
            if (pending_.type() == QDBusMessage::MethodCallMessage) connection.send(pending_.createReply(false));
            pending_ = message; pending_.setDelayedReply(true);
        } else connection.send(message.createReply());
        return true;
    }
    void poll() {
        if (scriptLoaded_) {
            // KWin may retain its active desktop window under the screen locker.
            // This reads lock state only; no idle or input APIs are used.
            const auto lockRequest = QDBusMessage::createMethodCall("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver", "GetActive");
            const auto lockReply = bus_.call(lockRequest, QDBus::Block, 200);
            lockKnown_ = lockReply.type() == QDBusMessage::ReplyMessage && lockReply.arguments().size() == 1 &&
                lockReply.arguments().first().metaType().id() == QMetaType::Bool;
            locked_ = lockKnown_ && lockReply.arguments().first().toBool();
        }
        QCoreApplication::processEvents();
        if (scriptLoaded_) {
            const auto owner = bus_.interface()->serviceOwner("org.kde.KWin");
            if (!owner.isValid() || owner.value() != owner_ || monoNow() - lastReport_ > 1500) {
                observation_(false, false); status_ = "KWin: foreground state unavailable";
            }
            if (pending_.type() == QDBusMessage::MethodCallMessage) {
                bus_.send(pending_.createReply(true)); pending_ = QDBusMessage();
            }
        }
        if (diagnostics_ && monoNow() - lastDiagnostic_ >= 5000) { lastDiagnostic_ = monoNow(); diagnostic(); }
    }
    QString status_ = "Foreground tracking unavailable (requires KDE Plasma 6 / KWin)";
private:
    QDBusMessage call(const QString &path, const QString &interface, const QString &method, const QVariantList &args = {}) {
        auto message = QDBusMessage::createMethodCall("org.kde.KWin", path, interface, method); message.setArguments(args);
        return bus_.call(message, QDBus::Block, 500);
    }
    void announce() { log_.write((status_ + '\n').toUtf8()); log_.flush(); }
    void diagnostic() {
        // Diagnostics only. These properties never feed FocusLedger.
        log_.write(("Gamescope diagnostic " + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
            " DISPLAY=" + qEnvironmentVariable("DISPLAY") + " WAYLAND_DISPLAY=" + qEnvironmentVariable("WAYLAND_DISPLAY") +
            " SteamAppId=" + qEnvironmentVariable("SteamAppId") + " SteamGameId=" + qEnvironmentVariable("SteamGameId") + '\n').toUtf8());
        QProcess process; process.setProgram("xprop");
        process.setArguments({"-root", "GAMESCOPE_FOCUSED_WINDOW", "GAMESCOPE_FOCUSABLE_WINDOWS", "GAMESCOPE_FOCUSED_APP",
            "GAMESCOPE_FOCUSABLE_APPS", "GAMESCOPE_FOCUS_CONTROL_APPIDS", "_NET_ACTIVE_WINDOW"});
        process.start();
        if (!process.waitForFinished(200)) { process.kill(); process.waitForFinished(200); }
        const auto root = process.readAllStandardOutput(); log_.write(root.left(8192));
        if (root.isEmpty()) log_.write(("xprop diagnostics unavailable: " + process.errorString() + "\n").toUtf8());
        log_.write(process.readAllStandardError().left(1024));
        // Inspect only published window IDs, with bounded output/runtime.
        QString windows;
        for (const auto &line : QString::fromLatin1(root).split('\n'))
            if (line.startsWith("GAMESCOPE_FOCUSED_WINDOW(") || line.startsWith("_NET_ACTIVE_WINDOW("))
                windows += line.section('=', 1) + ' ';
        const QRegularExpression ids("0x[0-9a-fA-F]+|[0-9]+");
        auto matches = ids.globalMatch(windows);
        for (int count = 0; count < 8 && matches.hasNext(); ++count) {
            const auto id = matches.next().captured();
            process.setArguments({"-id", id, "_NET_WM_PID", "STEAM_GAME", "WM_CLASS", "_NET_WM_STATE", "WM_STATE"});
            process.start(); if (!process.waitForFinished(150)) { process.kill(); process.waitForFinished(150); }
            log_.write(("window=" + id + '\n').toUtf8()); log_.write(process.readAllStandardOutput().left(4096));
        }
        log_.flush();
    }
    QFile &log_; Observation observation_;
    QString name_, service_, owner_;
    WindowOwners windowOwners_;
    QDBusConnection bus_; QTemporaryFile script_; QDBusMessage pending_;
    qint64 lastReport_ = 0, lastDiagnostic_ = -5000;
    bool scriptLoaded_ = false, diagnostics_ = false, lockKnown_ = false, locked_ = false;
};
FocusMonitor::FocusMonitor(QFile &log, Observation observation) : impl_(std::make_unique<Impl>(log, std::move(observation))) {}
FocusMonitor::~FocusMonitor() = default;
void FocusMonitor::poll() { impl_->poll(); }
QString FocusMonitor::status() const { return impl_->status_; }
}
