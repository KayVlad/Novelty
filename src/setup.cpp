#include "setup.h"
#include "profiles.h"
#include <QtCore/qdatetime.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QJsonObject inputs(const Game &g) {
    const auto &profile = resolveSetupProfile(g);
    if (profile.preparation != "generic-proton-v1")
        throw std::runtime_error("This setup profile requires an unsupported preparation method.");
    return {{"preset", profile.preparation}, {"runner", canonicalPath(g.runner)},
            {"runner_version", runnerVersion(g.runner)}, {"locale", g.locale},
            {"environment", g.json()["environment"]}};
}
}
bool setupComplete(const Library &lib, const Game &g) {
    try {
        const auto state = readJson(lib.gamePath(g.id, "setup.json"));
        const auto recipe = readJson(lib.gamePath(g.id, "recipe.lock.json"));
        return state["schema"].toInt() == 1 && state["state"] == "complete" &&
            !state["attempt"].toString().isEmpty() && state["attempt"] == recipe["attempt"] &&
            recipe["schema"].toInt() == 2 && recipe["inputs"].toObject() == inputs(g) &&
            recipe["prefix_identity"].toString() == folderIdentity(lib.prefix(g.id)) &&
            QFileInfo(lib.prefix(g.id) + "/system.reg").isFile();
    } catch (const std::exception &) {
        // A damaged/missing receipt is never proof of success. Prepare can repair it.
        return false;
    }
}
void preparePrefix(const Library &lib, const Game &g, const std::function<int()> &run) {
    const auto path = lib.gamePath(g.id, "setup.json");
    const auto expectedInputs = inputs(g);
    QJsonObject state{{"schema", 1}, {"attempt", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                      {"step", "prefix-init"}, {"state", "running"}, {"started_at", now()}, {"inputs", expectedInputs}};
    writeJson(path, state); // Invalidate earlier success before invoking umu, even for explicit repair.
    try {
        const int result = run();
        if (result != 0) throw std::runtime_error(("Prefix preparation exited with code " + QString::number(result) +
            ". Check Logs, then retry Prepare. The existing prefix and saves have been kept.").toStdString());
        state["step"] = "prefix-validation"; writeJson(path, state);
        if (!QFileInfo(lib.prefix(g.id) + "/system.reg").isFile())
            throw std::runtime_error("Preparation returned success but the prefix registry is missing. Check Logs and retry Prepare.");
        if (inputs(g) != expectedInputs)
            throw std::runtime_error("The runner changed during preparation. Review it in Manage, save and retry Prepare.");
        writeJson(lib.gamePath(g.id, "recipe.lock.json"), {{"schema", 2}, {"attempt", state["attempt"]},
                  {"inputs", expectedInputs}, {"prefix_identity", folderIdentity(lib.prefix(g.id))},
                  {"prepared_at", now()}, {"profile", g.setupProfile}, {"configuration", g.json()}});
        state["state"] = "complete"; state["completed_at"] = now(); writeJson(path, state);
    } catch (const std::exception &e) {
        state["state"] = "failed"; state["message"] = QString::fromUtf8(e.what());
        state["failed_at"] = now(); writeJson(path, state); throw;
    }
}
}
