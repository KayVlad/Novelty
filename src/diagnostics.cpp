#include "core.h"
#include <QtCore/qdatetime.h>
#include <QtCore/qfile.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>

namespace novelty {
namespace {
QJsonObject safeRecord(const QString &path, const QStringList &fields) {
    QJsonObject source;
    try { source = readJson(path); }
    catch (const std::exception &e) { return {{"read_error", QString::fromUtf8(e.what())}}; }
    QJsonObject result;
    for (const auto &field : fields) if (source.contains(field)) result.insert(field, source[field]);
    return result;
}
}
QJsonObject diagnosticReport(const Library &lib, const QString &id) {
    const auto g = lib.game(id);
    QJsonArray names;
    for (auto it = g.environment.cbegin(); it != g.environment.cend(); ++it) names.append(it.key());
    QJsonObject game{{"id", g.id}, {"name", g.name}, {"folder", g.folder}, {"executable", g.executable},
        {"cwd", g.workingDirectory}, {"engine", g.engine}, {"evidence", g.evidence},
        {"backend", g.backend}, {"runner", g.runner}, {"runner_revision", g.runnerRevision},
        {"profile", g.setupProfile}, {"locale", g.locale}, {"install_pending", g.installPending},
        {"environment_names", names}};
    const auto statePath = [&](const QString &file) { return lib.gamePath(id, file); };
    auto readLog = [&](const QString &file) {
        QFile log(statePath(file));
        if (log.open(QIODevice::ReadOnly)) return QString::fromUtf8(log.read(1024 * 1024));
        if (log.exists()) return "Cannot read " + file + ": " + log.errorString();
        return QString();
    };
    const auto logText = readLog("launch.log"), previousLog = readLog("launch.log.previous");
    const auto status = safeRecord(statePath("status.json"), {"state", "message", "updated"});
    const auto setup = safeRecord(statePath("setup.json"), {"state", "step", "attempt", "started_at", "completed_at", "failed_at", "message"});
    const auto recipe = safeRecord(statePath("recipe.lock.json"), {"schema", "attempt", "prefix_identity", "prepared_at", "profile"});
    const auto runtime = safeRecord(statePath("runtime.json"), {"schema", "boot_id", "supervisor_pid", "started_at"});
    const auto extra = safeRecord(statePath("extra-launch.json"), {"kind", "state", "attempt", "started_at", "finished_at", "exit_code", "message", "executable", "cwd"});
    QString display;
    try { display = lib.status(g); }
    catch (const std::exception &e) { display = "Cannot read status: " + QString::fromUtf8(e.what()); }
    return {{"schema", 1}, {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"status_display", display}, {"game", game}, {"status", status}, {"setup", setup},
        {"recipe", recipe}, {"runtime", runtime}, {"operation", extra}, {"launch_log", logText}, {"launch_log_previous", previousLog}};
}
} // namespace novelty
