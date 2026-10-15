#include "window_internal.h"

namespace novelty {
void Window::runExecutable(QDialog *manageDialog) {
    const auto g = selected(); if (!library_ || g.id.isEmpty() || model_->remoteGames.contains(g.id)) return;
    QDialog dialog(manageDialog); dialog.setObjectName("extraLaunchDialog"); dialog.setWindowTitle("Run in prefix — " + g.name); dialog.resize(720, 470);
    auto layout = new QVBoxLayout(&dialog);
    auto note = new QLabel("Run a patch or installer using this game's saved runner and prefix. Changes made by the program persist; the saved game executable and arguments stay unchanged.", &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto form = new QFormLayout;
    auto executable = new QLineEdit(&dialog); executable->setObjectName("extraExecutable");
    auto cwd = new QLineEdit(&dialog); cwd->setObjectName("extraCwd"); cwd->setPlaceholderText("Blank = executable's folder");
    auto arguments = new QPlainTextEdit(&dialog); arguments->setObjectName("extraArguments");
    arguments->setPlaceholderText("One argument per line. Spaces within each line are preserved."); arguments->setMaximumHeight(120);
    form->addRow("Windows executable", pathField(executable, &dialog, false, "Choose a patch or installer .exe"));
    form->addRow("Working directory", pathField(cwd, &dialog, true, "Choose working directory"));
    form->addRow("Arguments", arguments); layout->addLayout(form);
    auto status = new QLabel(&dialog); status->setObjectName("extraLaunchStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto run = buttons->addButton("Run in this prefix", QDialogButtonBox::AcceptRole); run->setObjectName("confirmExtraLaunch"); layout->addWidget(buttons);
    auto request = [&] { return ExtraLaunch{executable->text().trimmed(), cwd->text().trimmed(), arguments->toPlainText().split('\n', Qt::SkipEmptyParts)}; };
    auto validate = [&] {
        try {
            if (library_->busy(g.id)) throw std::runtime_error("This game's prefix is in use. Wait until it is free.");
            const auto command = extraLaunchCommand(*library_, library_->game(g.id), request());
            status->setText("Working directory: " + command.cwd + "\nPrefix: " + library_->prefix(g.id)); run->setEnabled(true);
        } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); run->setEnabled(false); }
    };
    connect(executable, &QLineEdit::textChanged, &dialog, validate); connect(cwd, &QLineEdit::textChanged, &dialog, validate);
    connect(arguments, &QPlainTextEdit::textChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { validate(); if (run->isEnabled()) dialog.accept(); });
    validate(); if (dialog.exec() != QDialog::Accepted) return;
    const auto extra = request(); manageDialog->reject(); launch(false, {}, extra);
}
bool Window::prepareLaunchRuntime(Game &g) {
    if (g.backend != "umu") return true;
    const auto originalRunner = g.runner;
    const auto originalUmu = g.umuResolved;
    if (!g.umuLauncher.isEmpty() && g.umuResolved.isEmpty())
        g.umuResolved = resolveUmuSelector(library_->root(), g.umuLauncher);
    QString geVersionToInstall;
    if (g.runner.isEmpty() && ((geUpdates_ && !geUpdates_->installingVersion().isEmpty()) || geInstallInProgress(library_->root()))) {
        auto selector = g.runnerSelector.isEmpty() ? library_->settings()["runner"].toString() : g.runnerSelector;
        const bool unspecified = selector.isEmpty();
        if (unspecified) selector = "ge-proton-latest";
        g.runner = resolveRunnerSelector(library_->root(), selector);
        if (g.runner.isEmpty() && unspecified) {
            for (const auto &fallback : {"proton-latest", "proton-experimental"}) {
                g.runner = resolveRunnerSelector(library_->root(), QString::fromLatin1(fallback));
                if (!g.runner.isEmpty()) break;
            }
            if (g.runner.isEmpty()) {
                const auto available = installedRunners(library_->root());
                if (!available.isEmpty()) g.runner = available.first();
            }
        }
        if (g.runner.isEmpty()) {
            message("No fully installed matching Proton is available. Choose an installed runner in Manage; Play does not wait for background updates.");
            return false;
        }
        if (isRunnerAlias(selector) && (!unspecified || g.runner == resolveRunnerSelector(library_->root(), selector))) g.runnerSelector = selector;
        g.runnerRevision = runnerVersion(g.runner);
    }
    if (!g.runner.isEmpty() && !runnerInstallationReady(library_->root(), g.runner)) {
        message("The selected Proton installation is missing or unverified: " + g.runner + ". Reconnect it or select another runner in Manage.");
        return false;
    }
    if (g.runner.isEmpty()) {
        QString chosen;
        if (!g.runnerSelector.isEmpty()) {
            chosen = resolveRunnerSelector(library_->root(), g.runnerSelector);
            if (chosen.isEmpty() && (g.runnerSelector == "ge-proton-latest" || g.runnerSelector.startsWith("GE-Proton"))) {
                geVersionToInstall = g.runnerSelector;
                if (g.runnerSelector != "ge-proton-latest")
                    chosen = library_->dataRoot() + "/runners/" + g.runnerSelector;
            } else if (chosen.isEmpty()) {
                message("The recipe's Proton selector has no installed match: " + g.runnerSelector); return false;
            }
        }
        if (!g.runnerSelector.isEmpty()) {
            g.runner = chosen;
        } else {
            const auto preferred = library_->settings()["runner"].toString();
            const auto available = installedRunners(library_->root());
            chosen = resolveRunnerSelector(library_->root(), preferred.isEmpty() ? "ge-proton-latest" : preferred);
            if (!preferred.isEmpty() && QDir::isAbsolutePath(preferred) && chosen.isEmpty()) {
                message("The default Proton path is unavailable: " + preferred + ". Reconnect it or change the default in Settings."); return false;
            }
            if (chosen.isEmpty() && !preferred.isEmpty() && !QDir::isAbsolutePath(preferred)) {
                if (preferred == "ge-proton-latest" || preferred.startsWith("GE-Proton")) geVersionToInstall = preferred;
                else { message("The default Proton selector has no installed match: " + preferred); return false; }
                chosen = preferred;
            }
            if (chosen.isEmpty() && available.size() == 1) chosen = available.first();
            else if (chosen.isEmpty() && available.size() > 1) {
                QDialog choice(this); choice.setObjectName("firstPlayRunnerDialog"); choice.setWindowTitle("Choose Proton for first Play");
                auto layout = new QVBoxLayout(&choice);
                layout->addWidget(new QLabel("Select an installed Proton version for this game. You can change it later in Manage.", &choice));
                auto runner = new QComboBox(&choice); runner->setObjectName("firstPlayRunner"); runner->addItems(available); layout->addWidget(runner);
                auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &choice); layout->addWidget(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &choice, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &choice, &QDialog::reject);
                if (choice.exec() != QDialog::Accepted) { message("Play preparation was cancelled."); return false; }
                chosen = runner->currentText();
            } else if (chosen.isEmpty()) {
                message("Preparing Proton for first Play…");
                geVersionToInstall = "ge-proton-latest";
                chosen = "ge-proton-latest"; // Resolved by the first-install setup step, never by a background-update wait.
            }
            if (chosen.isEmpty()) return false;
            g.runner = chosen;
            if (isRunnerAlias(preferred)) g.runnerSelector = preferred;
            else if (preferred.isEmpty() &&
                     (geVersionToInstall == "ge-proton-latest" || chosen == resolveRunnerSelector(library_->root(), "ge-proton-latest")))
                g.runnerSelector = "ge-proton-latest";
        }
    }
    const auto settingsBefore = library_->settings();
    if (!provisionFirstPlay(this, *library_, g, geVersionToInstall)) { message("Play preparation was cancelled. Your game and existing settings were kept."); return false; }
    if (g.runner != originalRunner || g.umuResolved != originalUmu) {
        library_->save(g); // Accept the selected runner only after the full first-Play setup succeeds.
        g = library_->game(g.id);
        refresh();
    } else if (settingsBefore != library_->settings()) refresh();
    return true;
}
void Window::launch(bool prepareOnly, const QString &tool, const std::optional<ExtraLaunch> &extra, bool installing, bool debugWine) {
    auto g = selected(); if (!library_ || g.id.isEmpty() || !pendingLaunch_.isEmpty() || importing_) return;
    if (model_->remoteGames.contains(g.id)) { if (!prepareOnly && tool.isEmpty() && !extra && !installing) installFromPeer(); return; }
    try {
        if (!rootAvailable()) throw std::runtime_error("Games library unavailable. Reconnect it and press Rescan.");
        if (!library_->recoveryIssue(g.id).isEmpty()) throw std::runtime_error(library_->recoveryIssue(g.id).toStdString());
        if (library_->busy(g.id)) throw std::runtime_error("This game is already running or preparing.");
        if (!installing && g.installPending) throw std::runtime_error("Finish or detect the installation before playing.");
        if (!installing && !prepareOnly && tool.isEmpty() && !extra && g.executable.isEmpty()) {
            if (!QFileInfo(g.folder).isDir()) throw std::runtime_error("Game folder unavailable. Relink it in Manage.");
            if (!chooseExecutable(g.id)) return;
            g = library_->game(g.id);
        }
        if (!prepareLaunchRuntime(g)) return;
        QString approvedWinetricks;
        const auto pendingComponents = pendingWinetricks(*library_, g);
        if (!pendingComponents.isEmpty()) {
            const auto details = "Install these Winetricks components into this game's existing prefix?\n\n" +
                pendingComponents.join(", ") + "\n\nRunner: " + g.runner +
                "\nPrefix: " + library_->prefix(g.id) +
                "\n\nThe installation may download files or show prompts. It changes the prefix and cannot be undone by removing a recipe entry. Saves will be kept.";
            if (QMessageBox::question(this, "Prepare game components", details,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            approvedWinetricks = winetricksApproval(*library_, g);
        }
        if (installing) extraLaunchCommand(*library_, g, {g.installer, {}, {}});
        else if (extra) extraLaunchCommand(*library_, g, *extra);
        else if (tool.isEmpty()) launchCommand(*library_, g, prepareOnly); else prefixToolCommand(*library_, g, tool);
        library_->recordStatus(g.id, "Starting");
        QProcess p; p.setProgram(launcherExecutable_);
        QStringList arguments{"--root", library_->root(), installing ? "--install" : prepareOnly ? "--prepare" : "--run", g.id};
        if (!tool.isEmpty()) arguments.append({"--prefix-tool", tool});
        if (debugWine) arguments.append("--debug-wine");
        if (!approvedWinetricks.isEmpty()) arguments.append("--approve-winetricks=" + approvedWinetricks);
        if (extra) {
            arguments.append("--extra-exe=" + extra->executable); arguments.append("--extra-cwd=" + extra->workingDirectory);
            for (const auto &argument : extra->arguments) arguments.append("--extra-arg=" + argument);
        }
        p.setArguments(arguments);
        p.setWorkingDirectory(g.workingDirectory);
        if (!p.startDetached()) throw std::runtime_error("Could not start the launch supervisor.");
        pendingLaunch_ = g.id; pendingInstaller_ = installing; pendingPrepare_ = prepareOnly; launchStarted_ = QDateTime::currentMSecsSinceEpoch();
        launchPoll_.start(); selectGame(); message(debugWine ? "Launching once with Wine debug…" : installing ? "Opening installer… Choose N:\\ as the destination." : extra ? "Starting executable…" : !tool.isEmpty() ? "Opening " + tool + "…" : prepareOnly ? "Preparing game…" : "Starting game…");
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
} // namespace novelty
