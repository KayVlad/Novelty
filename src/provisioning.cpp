#include "provisioning.h"
#include "core.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qfile.h>
#include <QtCore/quuid.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtimer.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qplaintextedit.h>
#include <QtGui/qtextdocument.h>
#include <memory>
#include <stdexcept>

namespace novelty {
namespace {
class InstallerDialog : public QDialog {
public:
    explicit InstallerDialog(QWidget *parent) : QDialog(parent), process(this) {}
    QProcess process;
    bool cancelled = false;
    void reject() override {
        if (process.state() != QProcess::NotRunning) {
            if (cancelled) return;
            cancelled = true; process.terminate();
            QTimer::singleShot(2000, this, [this] { if (process.state() != QProcess::NotRunning) process.kill(); });
        }
        else QDialog::reject();
    }
};
QString installerHelper(const QString &name, const QString &overridePath) {
    if (!overridePath.isEmpty()) return overridePath;
    for (const auto &directory : {"/../share/novelty/", "/../scripts/"}) {
        const auto candidate = QCoreApplication::applicationDirPath() + directory + name;
        if (QFileInfo(candidate).isFile()) return canonicalPath(candidate);
    }
    throw std::runtime_error("The bundled installer helper is missing. Reinstall Novelty or select an existing tool path.");
}
QString installerPython() {
    const auto bundled = QCoreApplication::applicationDirPath() + "/../python/bin/python3";
    const auto portable = resolveProgram(bundled);
    return portable.isEmpty() ? resolveProgram("python3") : portable;
}
}
QString provisionGe(QWidget *parent, const QString &libraryRoot, const QString &helper, bool startImmediately,
                    const QString &suggestedVersion) {
    try {
        const auto root = canonicalPath(libraryRoot);
        if (libraryRoot.isEmpty() || !QFileInfo(root).isDir()) throw std::runtime_error("Choose an existing Games folder first.");
        const auto data = root + "/.novelty";
        if (canonicalPath(data) != data) throw std::runtime_error("Managed runner data must not be linked to another location.");
        const auto configPath = data + "/ge-proton.json";
        const auto saved = readJson(configPath);
        InstallerDialog dialog(parent); dialog.setObjectName("geInstallerDialog"); dialog.setWindowTitle("Get GE-Proton"); dialog.resize(580, 330);
        auto layout = new QVBoxLayout(&dialog);
        auto description = new QLabel(startImmediately ?
            "Prepare a GE-Proton build for this game. Installed versions are kept side by side. Novelty will select this build for the current game after installation." :
            "Install a specific official GE-Proton build. Installed versions are kept side by side. Save Settings or Manage afterward to select the installed runner.", &dialog);
        description->setWordWrap(true); layout->addWidget(description);
        auto releases = new QLabel("<a style='color:#9eafc5' href='https://github.com/GloriousEggroll/proton-ge-custom/releases'>Browse official GE-Proton releases</a>", &dialog);
        releases->setOpenExternalLinks(true); layout->addWidget(releases);
        auto form = new QFormLayout;
        auto version = new QLineEdit(saved["version"].toString().isEmpty() ? suggestedVersion : saved["version"].toString(), &dialog);
        version->setObjectName("geVersion"); version->setPlaceholderText("For example: GE-Proton11-7"); form->addRow("Exact version", version);
        auto destination = new QLabel(root, &dialog); destination->setWordWrap(true); destination->setTextFormat(Qt::PlainText);
        form->addRow("Games library", destination); layout->addLayout(form);
        auto status = new QLabel("Downloads are checked against the release's SHA-512 checksum.", &dialog);
        status->setObjectName("geInstallStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status, 1);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto install = buttons->addButton("Install / use version", QDialogButtonBox::ActionRole); install->setObjectName("installGeButton");
        layout->addWidget(buttons);
        QString result, requestConfig; QByteArray output, errors;
        auto drain = [&] {
            output = (output + dialog.process.readAllStandardOutput()).right(64 * 1024);
            errors = (errors + dialog.process.readAllStandardError()).right(64 * 1024);
        };
        QObject::connect(&dialog.process, &QProcess::readyReadStandardOutput, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::readyReadStandardError, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                QFile::remove(requestConfig);
                if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
                install->setEnabled(true); version->setEnabled(true);
                status->setText("Could not start the GE installer: " + dialog.process.errorString());
            }
        });
        QObject::connect(&dialog.process, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus exit) {
            drain();
            QFile::remove(requestConfig);
            if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
            install->setEnabled(true); version->setEnabled(true);
            const auto expected = data + "/runners/" + version->text().trimmed();
            const auto returned = QString::fromUtf8(output).trimmed();
            if (code == 0 && exit == QProcess::NormalExit && returned == expected && QFileInfo(expected + "/proton").isFile()) {
                result = expected; dialog.accept();
            } else status->setText(errors.isEmpty() ? "Installation did not complete. Review the version and retry." : QString::fromUtf8(errors));
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(install, &QPushButton::clicked, &dialog, [&] {
            try {
                const auto selected = version->text().trimmed();
                if (selected.isEmpty()) throw std::runtime_error("Enter an exact GE-Proton version.");
                const auto script = installerHelper("install-ge.py", helper);
                const auto python = installerPython();
                if (script.isEmpty() || python.isEmpty()) throw std::runtime_error("GE downloads require the bundled installer helper and Python 3.");
                QJsonObject config{{"schema", 1}, {"version", selected}, {"architecture", "x86_64"}, {"auto_download", true}};
                if (saved["version"] == selected && saved.contains("sha512")) config["sha512"] = saved["sha512"];
                writeJson(configPath, config);
                requestConfig = data + "/ge-request-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
                writeJson(requestConfig, config); // Another window cannot replace this request's version mid-startup.
                output.clear(); errors.clear(); dialog.cancelled = false;
                install->setEnabled(false); version->setEnabled(false);
                status->setText("Downloading, checking and installing GE-Proton… This can take several minutes. Cancel stops this installation; you can retry it later.");
                dialog.process.setProcessEnvironment(externalToolEnvironment());
                dialog.process.start(python, {script, root, "--config", requestConfig});
            } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
        });
        if (startImmediately && saved.contains("auto_download") && !saved["auto_download"].toBool())
            status->setText("Automatic GE downloads are disabled for this library. Choose Install / use version to allow this one download.");
        else if (startImmediately && !version->text().trimmed().isEmpty())
            QTimer::singleShot(0, install, &QPushButton::click);
        dialog.exec();
        QObject::disconnect(&dialog.process, nullptr, &dialog, nullptr);
        if (dialog.process.state() != QProcess::NotRunning) { dialog.process.kill(); dialog.process.waitForFinished(3000); }
        QFile::remove(requestConfig);
        return result;
    } catch (const std::exception &e) {
        QMessageBox::warning(parent, "Cannot get GE-Proton", QString::fromUtf8(e.what())); return {};
    }
}
QString provisionUmu(QWidget *parent, const QString &libraryRoot, const QString &helper, bool startImmediately) {
    try {
        const auto root = canonicalPath(libraryRoot);
        if (libraryRoot.isEmpty() || !QFileInfo(root).isDir()) throw std::runtime_error("Choose an existing Games folder first.");
        const auto expected = managedUmuPath(root);
        if (canonicalPath(root + "/.novelty/tools") != root + "/.novelty/tools")
            throw std::runtime_error("Managed tools must not be linked to another location.");
        InstallerDialog dialog(parent); dialog.setObjectName("umuInstallerDialog"); dialog.setWindowTitle("Get umu"); dialog.resize(580, 310);
        auto layout = new QVBoxLayout(&dialog);
        auto description = new QLabel(startImmediately ?
            "Prepare the pinned umu launcher for this library. Novelty will use it for the current game after installation.\n\nThe first game setup may also need to download the Steam Linux Runtime." :
            "Install the pinned umu launcher for this library. Existing versions are kept. Save Settings afterward to select it.\n\nThe first game setup may also need to download the Steam Linux Runtime.", &dialog);
        description->setWordWrap(true); layout->addWidget(description);
        auto destination = new QLabel(expected, &dialog); destination->setWordWrap(true); destination->setTextFormat(Qt::PlainText); layout->addWidget(destination);
        auto status = new QLabel("The download and installed launcher are checked against pinned SHA-256 checksums.", &dialog);
        status->setObjectName("umuInstallStatus"); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status, 1);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto install = buttons->addButton("Install / use umu", QDialogButtonBox::ActionRole); install->setObjectName("installUmuButton"); layout->addWidget(buttons);
        QString result; QByteArray output, errors;
        auto drain = [&] {
            output = (output + dialog.process.readAllStandardOutput()).right(64 * 1024);
            errors = (errors + dialog.process.readAllStandardError()).right(64 * 1024);
        };
        QObject::connect(&dialog.process, &QProcess::readyReadStandardOutput, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::readyReadStandardError, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
            install->setEnabled(true); status->setText("Could not start the umu installer: " + dialog.process.errorString());
        });
        QObject::connect(&dialog.process, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus exit) {
            drain();
            if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
            install->setEnabled(true);
            if (code == 0 && exit == QProcess::NormalExit && QString::fromUtf8(output).trimmed() == expected &&
                canonicalPath(expected) == expected && !resolveProgram(expected).isEmpty()) {
                result = expected; dialog.accept();
            } else status->setText(errors.isEmpty() ? "Installation did not complete. Retry or choose an existing umu executable." : QString::fromUtf8(errors));
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(install, &QPushButton::clicked, &dialog, [&] {
            try {
                const auto script = installerHelper("install-umu.py", helper), python = installerPython();
                if (python.isEmpty()) throw std::runtime_error("umu installation requires Python 3.");
                output.clear(); errors.clear(); dialog.cancelled = false; install->setEnabled(false);
                status->setText("Downloading, checking and installing umu…");
                dialog.process.setProcessEnvironment(externalToolEnvironment());
                dialog.process.start(python, {script, root});
            } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
        });
        if (startImmediately) QTimer::singleShot(0, install, &QPushButton::click);
        dialog.exec();
        QObject::disconnect(&dialog.process, nullptr, &dialog, nullptr);
        if (dialog.process.state() != QProcess::NotRunning) { dialog.process.kill(); dialog.process.waitForFinished(3000); }
        return result;
    } catch (const std::exception &e) {
        QMessageBox::warning(parent, "Cannot get umu", QString::fromUtf8(e.what())); return {};
    }
}
QString requiredSteamRuntime(const QString &runner) {
    QFile manifest(QDir(runner).filePath("toolmanifest.vdf"));
    if (!manifest.open(QIODevice::ReadOnly)) return {};
    const auto bytes = manifest.read(64 * 1024 + 1);
    if (bytes.size() > 64 * 1024) return {};
    static const QRegularExpression appId("\\\"require_tool_appid\\\"\\s+\\\"([0-9]+)\\\"");
    const auto match = appId.match(QString::fromUtf8(bytes));
    if (!match.hasMatch()) return {};
    const auto id = match.captured(1);
    if (id == "1391110") return "steamrt2";
    if (id == "1628350") return "steamrt3";
    if (id == "4183110") return "steamrt4";
    if (id == "4185400") return "steamrt4-arm64";
    return {};
}
bool provisionSteamRuntime(QWidget *parent, const Library &library, const Game &game) {
    if (!library.managedRuntimeStorage()) return true; // Legacy libraries keep their existing umu storage until migration.
    const auto variant = requiredSteamRuntime(game.runner);
    if (variant.isEmpty()) return true; // Let umu handle an unfamiliar custom compatibility tool.
    const auto runtimeRoot = library.dataRoot() + "/runtime";
    const auto cacheRoot = library.dataRoot() + "/cache";
    const auto installed = runtimeRoot + "/umu/" + variant;
    const auto ready = [&] {
        return QFileInfo(installed + "/.installed.ok").isFile() &&
               QFileInfo(installed + "/_v2-entry-point").isFile();
    };
    try {
        for (const auto &path : {library.dataRoot(), runtimeRoot, runtimeRoot + "/umu", installed, cacheRoot})
            if (QFileInfo(path).isSymbolicLink() || canonicalPath(path) != path)
                throw std::runtime_error("Managed runtime/cache paths must stay inside Games.");
        if (ready()) return true;
        auto command = launchCommand(library, game, true);
        if (resolveProgram(command.program).isEmpty()) throw std::runtime_error("Choose a working umu executable before preparing its runtime.");
        InstallerDialog dialog(parent); dialog.setObjectName("steamRuntimeDialog");
        dialog.setWindowTitle("Prepare Steam Linux Runtime"); dialog.resize(600, 390);
        auto layout = new QVBoxLayout(&dialog);
        auto description = new QLabel("Novelty is preparing " + variant + " for the selected Proton build. The runtime is stored in this Games folder and reused offline. This may take several minutes on first Play.", &dialog);
        description->setWordWrap(true); layout->addWidget(description);
        auto phase = new QLabel("Starting runtime setup…", &dialog); phase->setObjectName("steamRuntimeStatus");
        phase->setWordWrap(true); phase->setTextFormat(Qt::PlainText); layout->addWidget(phase);
        auto progress = new QLabel("Working…", &dialog); progress->setObjectName("steamRuntimeProgress"); layout->addWidget(progress);
        auto elapsed = new QLabel("Elapsed: 0 s", &dialog); layout->addWidget(elapsed);
        auto details = new QPlainTextEdit(&dialog); details->setObjectName("steamRuntimeDetails");
        details->setReadOnly(true); details->document()->setMaximumBlockCount(120); layout->addWidget(details, 1);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto retry = buttons->addButton("Retry", QDialogButtonBox::ActionRole); retry->setObjectName("retrySteamRuntime");
        retry->setEnabled(true); layout->addWidget(buttons);
        std::unique_ptr<QTemporaryDir> probe;
        QElapsedTimer timer;
        QTimer clock(&dialog); clock.setInterval(1000);
        QObject::connect(&clock, &QTimer::timeout, &dialog, [&] {
            const auto seconds = timer.elapsed() / 1000;
            elapsed->setText(QString("Elapsed: %1 s").arg(seconds));
            progress->setText(QString("Working%1").arg(QString(seconds % 4, '.')));
        });
        auto drain = [&] {
            auto output = dialog.process.readAllStandardOutput() + dialog.process.readAllStandardError();
            if (output.isEmpty()) return;
            auto lines = QString::fromUtf8(output.right(16 * 1024)).remove('\r').split('\n', Qt::SkipEmptyParts);
            for (const auto &line : lines) details->appendPlainText(line.left(500));
            for (const auto &line : lines) if (line.contains("Downloading", Qt::CaseInsensitive))
                phase->setText("Downloading and checking " + variant + "…");
            else if (line.contains("Extract", Qt::CaseInsensitive) || line.contains("Installing", Qt::CaseInsensitive))
                phase->setText("Installing " + variant + "…");
        };
        QObject::connect(&dialog.process, &QProcess::readyReadStandardOutput, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::readyReadStandardError, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            clock.stop(); progress->setText("Stopped"); retry->setEnabled(true);
            phase->setText("Could not start umu: " + dialog.process.errorString());
        });
        QObject::connect(&dialog.process, &QProcess::finished, &dialog, [&](int, QProcess::ExitStatus) {
            drain(); clock.stop();
            if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
            if (ready()) { dialog.accept(); return; }
            progress->setText("Stopped"); retry->setEnabled(true);
            phase->setText("Runtime setup did not finish. Check the details, then Retry. Your game and saves are unchanged.");
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(retry, &QPushButton::clicked, &dialog, [&] {
            if (dialog.process.state() != QProcess::NotRunning) return;
            probe = std::make_unique<QTemporaryDir>(library.dataRoot() + "/runtime-probe-XXXXXX");
            if (!probe->isValid()) { phase->setText("Cannot create temporary runtime setup data inside Games."); return; }
            auto environment = command.environment;
            environment.insert("WINEPREFIX", probe->path() + "/prefix");
            environment.insert("UMU_NO_PROTON", "0");
            environment.insert("UMU_ZENITY", "0");
            environment.insert("UMU_LOG", "1");
            dialog.process.setProcessEnvironment(environment);
            dialog.process.setProgram(command.program); dialog.process.setArguments({""});
            dialog.process.setWorkingDirectory(library.dataRoot());
            details->clear(); phase->setText("Preparing " + variant + "…");
            progress->setText("Working…"); retry->setEnabled(false); dialog.cancelled = false;
            timer.start(); elapsed->setText("Elapsed: 0 s"); clock.start(); dialog.process.start();
        });
        QTimer::singleShot(0, retry, &QPushButton::click);
        const bool accepted = dialog.exec() == QDialog::Accepted;
        if (dialog.process.state() != QProcess::NotRunning) { dialog.process.kill(); dialog.process.waitForFinished(3000); }
        return accepted && ready();
    } catch (const std::exception &error) {
        QMessageBox::warning(parent, "Cannot prepare Steam Linux Runtime", QString::fromUtf8(error.what()));
        return false;
    }
}
bool provisionFirstPlay(QWidget *parent, Library &library, Game &game, const QString &geVersion,
                        const QString &geHelper, const QString &umuHelper) {
    if (game.backend != "umu") return true;
    if (!geVersion.isEmpty() && (geVersion.contains('/') || geVersion.contains('\\') || geVersion == "." || geVersion == "..")) {
        QMessageBox::warning(parent, "Cannot prepare game", "The pinned GE-Proton version is invalid."); return false;
    }
    const auto data = library.dataRoot();
    const auto gePath = geVersion.isEmpty() ? QString() : data + "/runners/" + geVersion;
    const bool needGe = !geVersion.isEmpty() && !QFileInfo(gePath + "/proton").isFile();
    const auto selectedUmu = library.settings()["umu"].toString("umu-run");
    const bool needUmu = selectedUmu == "umu-run" && resolveProgram(selectedUmu).isEmpty() &&
                         resolveProgram(data + "/tools/umu-run").isEmpty() &&
                         resolveProgram(managedUmuPath(library.root())).isEmpty();
    const auto runtimeVariant = [&] { return library.managedRuntimeStorage() ? requiredSteamRuntime(game.runner) : QString(); };
    const auto runtimePath = [&](const QString &variant) { return data + "/runtime/umu/" + variant; };
    const auto runtimeReady = [&](const QString &variant) {
        const auto path = runtimePath(variant);
        return QFileInfo(path + "/.installed.ok").isFile() && QFileInfo(path + "/_v2-entry-point").isFile();
    };
    try {
        QStringList guarded{data};
        if (needGe) guarded.append(data + "/runners");
        if (needUmu) guarded.append(data + "/tools");
        if (library.managedRuntimeStorage() && !runtimeVariant().isEmpty())
            guarded.append({data + "/runtime", data + "/runtime/umu", runtimePath(runtimeVariant()), data + "/cache"});
        for (const auto &path : guarded)
            if (QFileInfo(path).isSymbolicLink() || canonicalPath(path) != path)
                throw std::runtime_error("Managed tool/runtime paths must stay inside Games.");
        if (!needGe && !needUmu && (runtimeVariant().isEmpty() || runtimeReady(runtimeVariant()))) return true;

        InstallerDialog dialog(parent); dialog.setObjectName("firstPlayPreparationDialog");
        dialog.setWindowTitle("Prepare game for Play"); dialog.resize(620, 410);
        auto layout = new QVBoxLayout(&dialog);
        auto intro = new QLabel("Novelty is preparing this game's missing components. Completed downloads stay in Games and are reused on later Plays.", &dialog);
        intro->setWordWrap(true); layout->addWidget(intro);
        auto phase = new QLabel(&dialog); phase->setObjectName("firstPlayPreparationStatus");
        phase->setTextFormat(Qt::PlainText); phase->setWordWrap(true); layout->addWidget(phase);
        auto activity = new QLabel("Working…", &dialog); layout->addWidget(activity);
        auto elapsed = new QLabel("Elapsed: 0 s", &dialog); layout->addWidget(elapsed);
        auto details = new QPlainTextEdit(&dialog); details->setObjectName("firstPlayPreparationDetails");
        details->setReadOnly(true); details->document()->setMaximumBlockCount(160); layout->addWidget(details, 1);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto start = buttons->addButton("Start setup", QDialogButtonBox::ActionRole);
        start->setObjectName("firstPlayPreparationStart"); layout->addWidget(buttons);
        enum class Step { Ge, Umu, Runtime };
        QList<Step> steps;
        if (needGe) steps.append(Step::Ge);
        if (needUmu) steps.append(Step::Umu);
        int next = 0;
        bool checkedRuntime = false;
        Step current = Step::Ge;
        QString requestConfig, variant;
        QByteArray output, errors;
        std::unique_ptr<QTemporaryDir> probe;
        QElapsedTimer timer;
        QTimer clock(&dialog); clock.setInterval(1000);
        QObject::connect(&clock, &QTimer::timeout, &dialog, [&] {
            const auto seconds = timer.elapsed() / 1000;
            elapsed->setText(QString("Elapsed: %1 s").arg(seconds));
            activity->setText(QString("Working%1").arg(QString(seconds % 4, '.')));
        });
        auto drain = [&] {
            const auto newOutput = dialog.process.readAllStandardOutput();
            const auto newErrors = dialog.process.readAllStandardError();
            output = (output + newOutput).right(64 * 1024);
            errors = (errors + newErrors).right(64 * 1024);
            const auto chunk = (newOutput + newErrors).right(16 * 1024);
            if (!chunk.isEmpty()) {
                const auto lines = QString::fromUtf8(chunk).remove('\r').split('\n', Qt::SkipEmptyParts);
                for (const auto &line : lines) details->appendPlainText(line.left(500));
            }
        };
        QObject::connect(&dialog.process, &QProcess::readyReadStandardOutput, &dialog, drain);
        QObject::connect(&dialog.process, &QProcess::readyReadStandardError, &dialog, drain);
        std::function<void()> advance;
        advance = [&] {
            if (next == steps.size() && !checkedRuntime) {
                checkedRuntime = true;
                variant = runtimeVariant();
                if (!variant.isEmpty()) {
                    for (const auto &path : {data + "/runtime", data + "/runtime/umu", runtimePath(variant), data + "/cache"})
                        if (QFileInfo(path).isSymbolicLink() || canonicalPath(path) != path) {
                            phase->setText("Managed runtime/cache paths must stay inside Games.");
                            start->setEnabled(false); return;
                        }
                    if (!runtimeReady(variant)) steps.append(Step::Runtime);
                }
            }
            if (next == steps.size()) { dialog.accept(); return; }
            current = steps[next];
            const auto label = current == Step::Ge ? "GE-Proton " + geVersion :
                               current == Step::Umu ? "umu launcher" : "Steam Linux Runtime " + variant;
            phase->setText("Preparing " + label + "…");
            activity->setText("Ready"); start->setText("Start setup"); start->setEnabled(true);
            QTimer::singleShot(0, start, &QPushButton::click);
        };
        QObject::connect(&dialog.process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            QFile::remove(requestConfig); requestConfig.clear();
            clock.stop(); activity->setText("Stopped"); start->setText("Retry"); start->setEnabled(true);
            phase->setText("Could not start preparation: " + dialog.process.errorString());
        });
        QObject::connect(&dialog.process, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus exit) {
            drain(); clock.stop(); QFile::remove(requestConfig); requestConfig.clear();
            if (dialog.cancelled) { dialog.done(QDialog::Rejected); return; }
            bool complete = code == 0 && exit == QProcess::NormalExit;
            try {
                if (current == Step::Ge) {
                    complete = complete && QString::fromUtf8(output).trimmed() == gePath && QFileInfo(gePath + "/proton").isFile();
                    if (complete) game.runner = gePath;
                } else if (current == Step::Umu) {
                    const auto installed = managedUmuPath(library.root());
                    complete = complete && QString::fromUtf8(output).trimmed() == installed && !resolveProgram(installed).isEmpty();
                    if (complete) {
                        auto settings = library.settings(); settings["umu"] = installed; library.setSettings(settings);
                    }
                } else complete = runtimeReady(variant); // umu may complete the runtime before a disposable prefix fails.
            } catch (const std::exception &error) {
                details->appendPlainText(QString::fromUtf8(error.what())); complete = false;
            }
            if (complete) {
                details->appendPlainText("Completed."); ++next;
                QTimer::singleShot(0, &dialog, advance);
            } else {
                activity->setText("Stopped"); start->setText("Retry"); start->setEnabled(true);
                phase->setText("Preparation did not complete. Review the details and Retry, or Cancel to try again later.");
            }
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(start, &QPushButton::clicked, &dialog, [&] {
            if (dialog.process.state() != QProcess::NotRunning) return;
            try {
                output.clear(); errors.clear(); dialog.cancelled = false;
                QString program; QStringList arguments;
                auto environment = externalToolEnvironment();
                if (current == Step::Ge) {
                    const auto saved = readJson(data + "/ge-proton.json");
                    QJsonObject config{{"schema", 1}, {"version", geVersion}, {"architecture", "x86_64"}, {"auto_download", true}};
                    if (saved["version"] == geVersion && saved.contains("sha512")) config["sha512"] = saved["sha512"];
                    writeJson(data + "/ge-proton.json", config);
                    requestConfig = data + "/ge-request-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
                    writeJson(requestConfig, config);
                    program = installerPython(); arguments = {installerHelper("install-ge.py", geHelper), library.root(), "--config", requestConfig};
                } else if (current == Step::Umu) {
                    program = installerPython(); arguments = {installerHelper("install-umu.py", umuHelper), library.root()};
                } else {
                    probe = std::make_unique<QTemporaryDir>(data + "/runtime-probe-XXXXXX");
                    if (!probe->isValid()) throw std::runtime_error("Cannot create temporary runtime setup data inside Games.");
                    auto command = launchCommand(library, game, true);
                    program = command.program; arguments = {""}; environment = command.environment;
                    environment.insert("WINEPREFIX", probe->path() + "/prefix");
                    environment.insert("UMU_NO_PROTON", "0"); environment.insert("UMU_ZENITY", "0"); environment.insert("UMU_LOG", "1");
                }
                if (program.isEmpty()) throw std::runtime_error("Required launcher or Python 3 was not found.");
                dialog.process.setProcessEnvironment(environment); dialog.process.setWorkingDirectory(data);
                dialog.process.setProgram(program); dialog.process.setArguments(arguments);
                phase->setText(current == Step::Ge ? "Downloading and verifying GE-Proton " + geVersion + "…" :
                               current == Step::Umu ? "Downloading and verifying umu…" : "Preparing Steam Linux Runtime " + variant + "…");
                activity->setText("Working…"); start->setEnabled(false);
                timer.start(); elapsed->setText("Elapsed: 0 s"); clock.start(); dialog.process.start();
            } catch (const std::exception &error) {
                QFile::remove(requestConfig); requestConfig.clear();
                phase->setText(QString::fromUtf8(error.what())); start->setText("Retry"); start->setEnabled(true);
            }
        });
        const auto policy = readJson(data + "/ge-proton.json");
        if (needGe && policy.contains("auto_download") && !policy["auto_download"].toBool()) {
            current = Step::Ge;
            activity->setText("Ready");
            phase->setText("Automatic GE downloads are disabled for this library. Start setup to allow this installation.");
        } else QTimer::singleShot(0, &dialog, advance);
        const bool accepted = dialog.exec() == QDialog::Accepted;
        if (dialog.process.state() != QProcess::NotRunning) { dialog.process.kill(); dialog.process.waitForFinished(3000); }
        QFile::remove(requestConfig);
        return accepted;
    } catch (const std::exception &error) {
        QMessageBox::warning(parent, "Cannot prepare game", QString::fromUtf8(error.what()));
        return false;
    }
}
}
