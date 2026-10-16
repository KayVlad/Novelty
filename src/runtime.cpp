#include "playtime.h"
#include "runtime.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/quuid.h>
#include <cerrno>
#include <stdexcept>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace novelty {
namespace {
QString bootId() {
    QFile file("/proc/sys/kernel/random/boot_id");
    const auto value = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    return QUuid(value).isNull() ? QString() : value;
}
class RuntimeGuard {
public:
    RuntimeGuard(const Library &lib, const QString &id, PlaytimeSession *playtime)
        : path_(lib.gamePath(id, "runtime.json")), playtime_(playtime) {
#ifdef __linux__
        if (prctl(PR_GET_CHILD_SUBREAPER, &previous_) != 0 || prctl(PR_SET_CHILD_SUBREAPER, 1L) != 0)
            throw std::runtime_error("Cannot enable descendant process tracking; launch was not started.");
#else
        throw std::runtime_error("Descendant process tracking requires Linux.");
#endif
    }
    ~RuntimeGuard() {
#ifdef __linux__
        // QProcess has already destroyed/reaped its direct child. Orphaned descendants
        // are now our children, including double-forked processes and new sessions.
        // A killed monitor cannot execute this cleanup; its marker remains.
        bool stopped = false;
        for (;;) {
            const auto child = waitpid(-1, nullptr, playtime_ ? WNOHANG : 0);
            if (child > 0) continue;
            if (child == 0) {
                if (playtime_) playtime_->checkpoint();
                ::usleep(250000);
                continue;
            }
            if (errno == EINTR) continue;
            stopped = errno == ECHILD; break;
        }
        if (stopped) {
            if (playtime_) playtime_->complete();
            QFile::remove(path_);
        }
        prctl(PR_SET_CHILD_SUBREAPER, static_cast<long>(previous_));
#endif
    }
    RuntimeGuard(const RuntimeGuard &) = delete;
    RuntimeGuard &operator=(const RuntimeGuard &) = delete;
private:
    QString path_;
    PlaytimeSession *playtime_;
    int previous_ = 0;
};
int runTrackedWorker(const Library &lib, const QString &id, const Command &cmd, QFile &log, bool trackPlaytime) {
    PlaytimeSession playtime(lib.root(), id, log);
    RuntimeGuard guard(lib, id, trackPlaytime ? &playtime : nullptr);
    QProcess process;
    process.setProgram(cmd.program); process.setArguments(cmd.arguments);
    process.setWorkingDirectory(cmd.cwd); process.setProcessEnvironment(cmd.environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    qint64 written = log.pos();
    auto drain = [&] {
        const auto bytes = process.readAllStandardOutput();
        const auto remaining = 1024 * 1024 - written;
        if (remaining > 0) { auto part = bytes.left(remaining); log.write(part); written += part.size(); }
    };
    process.start();
    if (!process.waitForStarted(10000))
        throw std::runtime_error(("Could not start " + cmd.program + ": " + process.errorString()).toStdString());
    if (trackPlaytime) playtime.start(); // exec succeeded; preparation and sync are outside this invocation.
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(250); drain();
        if (trackPlaytime) playtime.checkpoint();
    }
    drain(); log.flush();
    if (process.exitStatus() == QProcess::CrashExit)
        throw std::runtime_error("The launch process crashed. See the launch log.");
    return process.exitCode();
}
}
QString runtimeRecoveryIssue(const Library &lib, const QString &id) {
    const auto path = lib.gamePath(id, "runtime.json");
    if (!QFileInfo::exists(path)) return {};
    try {
        const auto state = readJson(path);
        const auto previousBoot = state["boot_id"].toString();
        if (state["schema"].toInt() != 1 || QUuid(previousBoot).isNull())
            throw std::runtime_error("Invalid process recovery record");
        const auto currentBoot = bootId();
        if (!currentBoot.isEmpty() && currentBoot != previousBoot) return {};
        return "The previous launch is still being monitored or its monitor stopped unexpectedly. "
               "Wait for the game or tool to close, then press Rescan. If recovery remains blocked, "
               "Restart the Steam Deck/system before launching, editing, relinking or removing this game.";
    } catch (const std::exception &) {
        return "Process recovery data is unreadable or damaged. Game operations are blocked until this record can be inspected and recovered.";
    }
}
int executeTracked(const Library &lib, const QString &id, const Command &cmd, QFile &log, bool trackPlaytime) {
#ifdef __linux__
    // The worker is the backend's parent and subreaper. If the headless caller is
    // killed, this worker still owns descendants and can clear the barrier safely.
    const auto issue = lib.recoveryIssue(id);
    if (!issue.isEmpty()) throw std::runtime_error(issue.toStdString());
    const auto boot = bootId();
    if (boot.isEmpty()) throw std::runtime_error("Cannot establish the system boot identity; launch was not started.");
    // Publish before fork: a killed caller must never leave an unmarked worker.
    writeJson(lib.gamePath(id, "runtime.json"), {{"schema", 1}, {"boot_id", boot},
        {"supervisor_pid", QCoreApplication::applicationPid()},
        {"started_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    log.flush();
    const pid_t worker = fork();
    if (worker < 0) {
        QFile::remove(lib.gamePath(id, "runtime.json")); // No backend or monitor was started.
        throw std::runtime_error("Could not start the process monitor.");
    }
    if (worker == 0) {
        int code = 1;
        try {
            if (setsid() < 0) throw std::runtime_error("Could not detach the process monitor.");
            code = runTrackedWorker(lib, id, cmd, log, trackPlaytime);
        }
        catch (const std::exception &e) { log.write("Novelty monitor: "); log.write(e.what()); log.write("\n"); log.flush(); }
        _exit(code >= 0 && code <= 255 ? code : 1);
    }
    int status = 0;
    while (waitpid(worker, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("Could not wait for the process monitor; recovery evidence was kept.");
    }
    if (!WIFEXITED(status)) throw std::runtime_error("The process monitor stopped unexpectedly; recovery evidence was kept.");
    return WEXITSTATUS(status);
#else
    Q_UNUSED(lib); Q_UNUSED(id); Q_UNUSED(cmd); Q_UNUSED(log); Q_UNUSED(trackPlaytime);
    throw std::runtime_error("Descendant process tracking requires Linux.");
#endif
}
}
