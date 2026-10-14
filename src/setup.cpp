#include "setup.h"
#include <QtCore/qdatetime.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qset.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QJsonObject inputs(const Game &g) {
    return {{"runner", canonicalPath(g.runner)},
            {"runner_version", runnerVersion(g.runner)}, {"locale", g.locale},
            {"environment", g.json()["environment"]}, {"umu_launcher", g.umuResolved},
            {"umu_game_id", g.umuGameId}, {"store", g.store}};
}
bool compatibleInputs(QJsonObject saved, const QJsonObject &current) {
    // Older receipts recorded the compiled generic setup profile. Its preparation
    // was identical, so keep those successful prefixes valid without retaining
    // the profile as an active setting.
    if (saved.contains("preset") && saved.take("preset") != "generic-proton-v1") return false;
    return saved == current;
}
QStringList appliedWinetricks(const Library &lib, const Game &g) {
    const auto receipt = readJson(lib.gamePath(g.id, "recipe.lock.json"));
    if (receipt["schema"].toInt() != 2 || receipt["prefix_identity"].toString().isEmpty() ||
        receipt["prefix_identity"].toString() != folderIdentity(lib.prefix(g.id))) return {};
    if (!receipt.contains("winetricks_applied")) return {}; // Older receipts did not track components.
    if (!receipt["winetricks_applied"].isArray())
        throw std::runtime_error("Winetricks preparation receipt is invalid. Review this game's setup before Play.");
    QStringList applied;
    for (const auto &value : receipt["winetricks_applied"].toArray()) {
        if (!value.isString()) throw std::runtime_error("Winetricks preparation receipt has an invalid verb.");
        applied.append(value.toString());
    }
    validateWinetricks(applied);
    return applied;
}
}
void validateWinetricks(const QStringList &verbs) {
    static const QSet<QString> allowed{"cjkfonts", "corefonts", "quartz", "wmp11", "qasf"};
    for (const auto &verb : verbs)
        if (!allowed.contains(verb))
            throw std::runtime_error(("Unsupported Winetricks verb: " + verb).toStdString());
}
QStringList pendingWinetricks(const Library &lib, const Game &g) {
    validateWinetricks(g.winetricks);
    if (!g.winetricks.isEmpty() && g.backend != "umu")
        throw std::runtime_error("Winetricks components can only be prepared for an umu game.");
    const auto applied = appliedWinetricks(lib, g);
    QStringList pending;
    for (const auto &verb : g.winetricks)
        if (!applied.contains(verb) && !pending.contains(verb)) pending.append(verb);
    return pending;
}
QString winetricksApproval(const Library &lib, const Game &g) {
    const auto pending = pendingWinetricks(lib, g);
    if (pending.isEmpty()) return {};
    const auto input = (g.id + '\n' + canonicalPath(g.runner) + '\n' + runnerVersion(g.runner) + '\n' +
        folderIdentity(lib.prefix(g.id)) + '\n' + pending.join('\n')).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}
bool setupComplete(const Library &lib, const Game &g) {
    try {
        const auto state = readJson(lib.gamePath(g.id, "setup.json"));
        const auto recipe = readJson(lib.gamePath(g.id, "recipe.lock.json"));
        return state["schema"].toInt() == 1 && state["state"] == "complete" &&
            !state["attempt"].toString().isEmpty() && state["attempt"] == recipe["attempt"] &&
            recipe["schema"].toInt() == 2 && compatibleInputs(recipe["inputs"].toObject(), inputs(g)) &&
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
        auto previous = readJson(lib.gamePath(g.id, "recipe.lock.json"));
        QJsonObject receipt{{"schema", 2}, {"attempt", state["attempt"]},
                  {"inputs", expectedInputs}, {"prefix_identity", folderIdentity(lib.prefix(g.id))},
                  {"prepared_at", now()}, {"configuration", g.json()},
                  {"winetricks_requested", QJsonArray::fromStringList(g.winetricks)}};
        if (previous["schema"].toInt() == 2 && previous["prefix_identity"] == receipt["prefix_identity"] &&
            previous["winetricks_applied"].isArray())
            receipt["winetricks_applied"] = previous["winetricks_applied"];
        writeJson(lib.gamePath(g.id, "recipe.lock.json"), receipt);
        state["state"] = "complete"; state["completed_at"] = now(); writeJson(path, state);
    } catch (const std::exception &e) {
        state["state"] = "failed"; state["message"] = QString::fromUtf8(e.what());
        state["failed_at"] = now(); writeJson(path, state); throw;
    }
}
void prepareWinetricks(const Library &lib, const Game &g, const std::function<int(const QString &)> &run) {
    const auto pending = pendingWinetricks(lib, g);
    if (pending.isEmpty()) return;
    if (!setupComplete(lib, g)) throw std::runtime_error("Prepare the Proton prefix before installing Winetricks components.");
    auto state = readJson(lib.gamePath(g.id, "setup.json"));
    auto receipt = readJson(lib.gamePath(g.id, "recipe.lock.json"));
    state["winetricks_state"] = "running"; state["winetricks_started_at"] = now();
    state["winetricks_requested"] = QJsonArray::fromStringList(g.winetricks);
    writeJson(lib.gamePath(g.id, "setup.json"), state);
    auto applied = appliedWinetricks(lib, g);
    try {
        for (const auto &verb : pending) {
            state["winetricks_current"] = verb; writeJson(lib.gamePath(g.id, "setup.json"), state);
            const auto result = run(verb);
            if (result != 0) throw std::runtime_error(("Winetricks " + verb + " exited with code " +
                QString::number(result) + ". Review the launch log; the existing prefix and saves were kept.").toStdString());
            applied.append(verb);
            receipt["winetricks_applied"] = QJsonArray::fromStringList(applied);
            receipt["winetricks_requested"] = QJsonArray::fromStringList(g.winetricks);
            receipt["configuration"] = g.json();
            receipt["winetricks_updated_at"] = now();
            writeJson(lib.gamePath(g.id, "recipe.lock.json"), receipt);
        }
        state["winetricks_state"] = "complete"; state.remove("winetricks_current");
        state["winetricks_completed_at"] = now(); writeJson(lib.gamePath(g.id, "setup.json"), state);
    } catch (const std::exception &error) {
        state["winetricks_state"] = "failed"; state["winetricks_error"] = QString::fromUtf8(error.what());
        state["winetricks_failed_at"] = now(); writeJson(lib.gamePath(g.id, "setup.json"), state);
        throw;
    }
}
}
