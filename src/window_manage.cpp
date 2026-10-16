#include "playtime.h"
#include <QtCore/qlocale.h>
#include "window_internal.h"
#include <QtCore/qregularexpression.h>

namespace novelty {
void Window::manage() {
    auto g = selected(); if (!library_ || g.id.isEmpty() || model_->remoteGames.contains(g.id)) return;
    QJsonObject librarySettings;
    try { librarySettings = library_->settings(); }
    catch (const std::exception &error) { message(QString::fromUtf8(error.what())); return; }
    if (library_->busy(g.id)) {
        message(library_->supervisorActive(g.id) ? "This game has an operation in progress. Retry when it finishes." : library_->recoveryIssue(g.id)); return;
    }
    QDialog d(this); d.setObjectName("manageDialog"); d.setWindowTitle("Manage — " + g.name); d.resize(860, 650);
    auto layout = new QVBoxLayout(&d); auto tabs = new QTabWidget(&d); layout->addWidget(tabs, 1);
    tabs->setObjectName("manageTabs");
    auto overviewScroll = new QScrollArea(&d); overviewScroll->setWidgetResizable(true); overviewScroll->setFrameShape(QFrame::NoFrame);
    auto overview = new QWidget(overviewScroll); auto overviewForm = new QFormLayout(overview);
    auto overviewRow = [&](const QString &label, const QString &value) {
        auto field = new QLabel(value.isEmpty() ? "—" : value, overview); field->setWordWrap(true); field->setTextFormat(Qt::PlainText); field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        overviewForm->addRow(label, field); return field;
    };
    if (g.installPending) {
        auto hint = new QLabel("Installation is pending. Choose N:\\ inside the installer, then let Novelty detect the installed files. These actions use saved runtime settings.", &d); hint->setWordWrap(true); overviewForm->addRow(hint);
        auto installer = new QLineEdit(g.installer, &d); installer->setObjectName("pendingInstaller");
        overviewForm->addRow("Installer", pathField(installer, &d, false, "Select Windows installer"));
        auto retry = new QPushButton("Run / retry installer", &d); retry->setObjectName("retryInstaller"); overviewForm->addRow(retry);
        auto detect = new QPushButton("Detect installed files", &d); detect->setObjectName("detectInstallation"); overviewForm->addRow(detect);
        connect(retry, &QPushButton::clicked, &d, [&, installer] {
            try {
                auto saved = library_->game(g.id); saved.installer = installer->text();
                extraLaunchCommand(*library_, saved, {saved.installer, {}, {}}); library_->save(saved);
                d.reject(); refresh(); launch(false, {}, {}, true);
            } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot run installer", QString::fromUtf8(e.what())); }
        });
        connect(detect, &QPushButton::clicked, &d, [&] {
            try {
                const auto result = library_->detectInstallation(g.id);
                d.reject(); refresh(); message(result);
                if (library_->game(g.id).executable.isEmpty()) chooseExecutable(g.id);
            }
            catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot detect files", QString::fromUtf8(e.what())); }
        });
    }
    overviewRow("Status", library_->status(g)); overviewRow("Engine", g.engine);
    auto overviewFolder = overviewRow("Game folder", g.folder);
    auto overviewExecutable = overviewRow("Executable", g.executable);
    auto overviewCwd = overviewRow("Working directory", g.workingDirectory);
    overviewRow("Backend", g.backend == "steam" ? "Steam shortcut" : g.backend == "native" ? "Native Linux" : "umu / Proton");
    overviewRow("Preset", g.recipeId.isEmpty() ? "Legacy/manual" : g.recipeId);
    overviewRow("Proton directory", g.backend == "native" ? "Not needed" : g.backend == "steam" ? "Managed by Steam" : g.runner);
    overviewRow("Prefix", g.backend == "native" ? "Not needed" : g.backend == "steam" ? "Managed by Steam" : library_->prefix(g.id));
    overviewRow("Accepted runner", g.runnerRevision); overviewRow("Locale", g.locale.isEmpty() ? "System default" : g.locale);
    overviewRow("Steam launch ID", g.steamId); overviewRow("Arguments", g.arguments.join('\n'));
    QStringList overviewEnvironment; for (auto it = g.environment.begin(); it != g.environment.end(); ++it) overviewEnvironment.append(it.key() + '=' + it.value());
    overviewRow("Environment", overviewEnvironment.join('\n')); overviewRow("Engine evidence", g.evidence);
    auto statisticsTitle = new QLabel("Playtime / Statistics", overview);
    statisticsTitle->setStyleSheet("font-weight: bold; margin-top: 12px;"); overviewForm->addRow(statisticsTitle);
    auto statisticsTotal = overviewRow("Time played", "0m"); statisticsTotal->setObjectName("statisticsTotal");
    auto statisticsCount = overviewRow("Recorded sessions", "0"); statisticsCount->setObjectName("statisticsCount");
    auto statisticsLast = overviewRow("Last played", "Never"); statisticsLast->setObjectName("statisticsLastPlayed");
    auto statisticsCurrent = overviewRow("Current session", "—"); statisticsCurrent->setObjectName("statisticsCurrentSession");
    auto history = new QListWidget(overview); history->setObjectName("statisticsHistory"); history->setMaximumHeight(150);
    overviewForm->addRow("Recent sessions", history);
    auto statisticsNote = overviewRow({}, g.backend == "steam" ?
        "Tracking is unavailable for Steam shortcuts: Novelty cannot monitor the actual Steam game process." :
        "Recorded on this device. Interrupted sessions include only their last committed checkpoint.");
    const auto renderStatistics = [&] {
        try {
            const auto stats = readPlaytime(library_->root(), g.id);
            statisticsTotal->setText(formatTrackedTime(stats.totalMs));
            statisticsCount->setText(QString::number(stats.sessions)); statisticsLast->setText(formatLastPlayed(stats.lastPlayed));
            statisticsCurrent->setText(g.backend == "steam" ? "Unavailable for Steam" : stats.active ? formatTrackedTime(stats.currentMs, true) : "—");
            QStringList lines;
            for (const auto &session : stats.recent)
                lines.append(QLocale().toString(session.started.toLocalTime(), QLocale::ShortFormat) + " · " +
                    formatTrackedTime(session.durationMs, true) + " · " + session.state);
            if (lines.isEmpty()) lines.append("No recorded sessions yet.");
            QStringList previous; for (int i = 0; i < history->count(); ++i) previous.append(history->item(i)->text());
            if (previous != lines) { history->clear(); history->addItems(lines); }
        } catch (const std::exception &error) {
            statisticsTotal->setText("Unavailable"); statisticsNote->setText(QString::fromUtf8(error.what()));
        }
    };
    renderStatistics(); QTimer statisticsPoll(&d); statisticsPoll.setInterval(1000);
    connect(&statisticsPoll, &QTimer::timeout, &d, renderStatistics); statisticsPoll.start();
    overviewScroll->setWidget(overview); tabs->addTab(overviewScroll, "Overview");
    auto metadataScroll = new QScrollArea(&d); metadataScroll->setWidgetResizable(true); metadataScroll->setFrameShape(QFrame::NoFrame);
    auto metadataTab = new QWidget(metadataScroll); auto metadataLayout = new QVBoxLayout(metadataTab);
    auto metadataSearchRow = new QHBoxLayout;
    auto metadataQuery = new QLineEdit(g.name, metadataTab); metadataQuery->setObjectName("metadataQuery");
    metadataQuery->setPlaceholderText("Search VNDB by title"); metadataSearchRow->addWidget(metadataQuery, 1);
    auto metadataSearch = new QPushButton("Search", metadataTab); metadataSearch->setObjectName("metadataSearch"); metadataSearchRow->addWidget(metadataSearch);
    metadataLayout->addLayout(metadataSearchRow);
    auto metadataResults = new QListWidget(metadataTab); metadataResults->setObjectName("metadataResults");
    metadataResults->setMaximumHeight(140); metadataResults->hide(); metadataLayout->addWidget(metadataResults);
    auto linkMetadata = new QPushButton("Link selected title", metadataTab); linkMetadata->setObjectName("metadataLink");
    linkMetadata->hide(); metadataLayout->addWidget(linkMetadata, 0, Qt::AlignLeft);
    auto metadataStatus = new QLabel(metadataTab); metadataStatus->setObjectName("metadataStatus");
    metadataStatus->setWordWrap(true); metadataStatus->setTextFormat(Qt::PlainText); metadataLayout->addWidget(metadataStatus);
    auto metadataForm = new QFormLayout;
    auto metadataField = [&](const QString &label, const QString &objectName) {
        auto value = new QLabel(metadataTab); value->setObjectName(objectName); value->setWordWrap(true);
        value->setTextFormat(Qt::PlainText); value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        metadataForm->addRow(label, value); return value;
    };
    auto vndbLink = new QLabel(metadataTab); vndbLink->setObjectName("metadataVndbId");
    vndbLink->setTextFormat(Qt::RichText); vndbLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    vndbLink->setCursor(Qt::PointingHandCursor); metadataForm->addRow("VNDB ID", vndbLink);
    auto matchedTitle = metadataField("VNDB title", "metadataMatchedTitle");
    auto original = metadataField("Original title", "metadataOriginalTitle");
    auto expected = metadataField("Expected play time", "metadataExpectedTime");
    auto developers = metadataField("Developer", "metadataDevelopers");
    auto publishers = metadataField("Publishers (all releases)", "metadataPublishers");
    auto contentTags = metadataField("Content tags", "metadataContentTags");
    auto description = new QPlainTextEdit(metadataTab); description->setObjectName("metadataDescription");
    description->setReadOnly(true); description->setMaximumHeight(170); metadataForm->addRow("Description", description);
    metadataLayout->addLayout(metadataForm);
    auto metadataActions = new QHBoxLayout;
    auto refreshMetadata = new QPushButton("Refresh", metadataTab); refreshMetadata->setObjectName("metadataRefresh");
    auto unlinkMetadata = new QPushButton("Unlink", metadataTab); unlinkMetadata->setObjectName("metadataUnlink");
    metadataActions->addWidget(refreshMetadata); metadataActions->addWidget(unlinkMetadata); metadataActions->addStretch();
    metadataLayout->addLayout(metadataActions);
    auto attribution = new QLabel("Metadata from VNDB. New games are matched automatically; search, relink or refresh here to correct a result. Cached details work offline.", metadataTab);
    attribution->setWordWrap(true); attribution->setProperty("muted", true); metadataLayout->addWidget(attribution);
    metadataLayout->addStretch(); metadataScroll->setWidget(metadataTab); tabs->addTab(metadataScroll, "Metadata");
    auto client = new VndbClient(&d, metadataApiBase_);
    auto updateMetadata = [&] {
        const auto data = g.metadata;
        const auto id = data["id"].toString();
        vndbLink->setText(validVndbId(id) ? "<a style='color:#77bbdd' href='https://vndb.org/" + id + "'>" + id + "</a>" : "—");
        matchedTitle->setText(data["title"].toString("—")); original->setText(data["original_title"].toString("—"));
        expected->setText(data["expected_play_time"].toString("—"));
        developers->setText(metadataStrings(data["developers"]).join(" · ").isEmpty() ? "—" : metadataStrings(data["developers"]).join(" · "));
        publishers->setText(metadataStrings(data["publishers"]).join(" · ").isEmpty() ? "—" : metadataStrings(data["publishers"]).join(" · "));
        contentTags->setText(metadataStrings(data["tags"]).join(" · ").isEmpty() ? "—" : metadataStrings(data["tags"]).join(" · "));
        description->setPlainText(data["description"].toString());
        refreshMetadata->setEnabled(validVndbId(id)); unlinkMetadata->setEnabled(validVndbId(id));
    };
    connect(vndbLink, &QLabel::linkActivated, &d, [this](const QString &link) {
        const QUrl url(link);
        if (url.scheme() == "https" && url.host() == "vndb.org" && validVndbId(url.path().mid(1))) QDesktopServices::openUrl(url);
    });
    auto fetchMetadata = [&](const QString &id) {
        metadataStatus->setText("Fetching VNDB metadata…");
        linkMetadata->setEnabled(false); refreshMetadata->setEnabled(false); unlinkMetadata->setEnabled(false); metadataSearch->setEnabled(false);
        client->fetch(id, [&, id](const QJsonObject &data, const QByteArray &coverBytes, const QString &warning) {
            linkMetadata->setEnabled(true); metadataSearch->setEnabled(true);
            if (data.isEmpty()) { metadataStatus->setText(warning); updateMetadata(); return; }
            try {
                const auto oldId = g.metadata["id"].toString();
                autoMetadataQueued_.remove(g.id);
                library_->setMetadata(g.id, data); g.metadata = data;
                const auto path = metadataCoverPath(library_->dataRoot(), g.id);
                if (!coverBytes.isEmpty()) {
                    if (!saveMetadataCover(path, coverBytes)) metadataStatus->setText("Linked, but the cover could not be saved.");
                } else if (oldId != id) QFile::remove(path);
                metadataResults->clear(); metadataResults->hide(); linkMetadata->hide();
                invalidateCover(path); updateMetadata(); refresh();
                if (metadataStatus->text().isEmpty() || metadataStatus->text() == "Fetching VNDB metadata…")
                    metadataStatus->setText(warning.isEmpty() ? "VNDB metadata saved for offline use." : "Metadata saved. " + warning);
                else if (!warning.isEmpty()) metadataStatus->setText(metadataStatus->text() + '\n' + warning);
            } catch (const std::exception &e) { metadataStatus->setText(QString::fromUtf8(e.what())); updateMetadata(); }
        });
    };
    connect(metadataSearch, &QPushButton::clicked, &d, [&, client] {
        metadataSearch->setEnabled(false); metadataStatus->setText("Searching VNDB…");
        metadataResults->clear(); metadataResults->hide(); linkMetadata->hide();
        client->search(metadataQuery->text(), [&, client](const QJsonArray &hits, const QString &error) {
            metadataSearch->setEnabled(true);
            if (!error.isEmpty()) { metadataStatus->setText(error); return; }
            for (const auto &value : hits) {
                const auto hit = value.toObject(); auto label = hit["title"].toString();
                const auto alternate = hit["alttitle"].toString();
                if (!alternate.isEmpty() && alternate != label) label += " · " + alternate;
                label += " (" + hit["id"].toString() + ')';
                auto item = new QListWidgetItem(label, metadataResults); item->setData(Qt::UserRole, hit["id"].toString());
            }
            metadataStatus->setText(hits.isEmpty() ? "No VNDB matches. Try another title." : "Select the matching title, then link it.");
            if (!hits.isEmpty()) {
                metadataResults->setFixedHeight(qMin(140, 8 + hits.size() * 27));
                metadataResults->show(); metadataResults->setCurrentRow(0); linkMetadata->show();
            }
        });
    });
    connect(metadataQuery, &QLineEdit::returnPressed, metadataSearch, &QPushButton::click);
    connect(linkMetadata, &QPushButton::clicked, &d, [&] {
        if (auto item = metadataResults->currentItem()) fetchMetadata(item->data(Qt::UserRole).toString());
    });
    connect(metadataResults, &QListWidget::itemActivated, &d, [&](QListWidgetItem *item) {
        if (item) fetchMetadata(item->data(Qt::UserRole).toString());
    });
    connect(refreshMetadata, &QPushButton::clicked, &d, [&] { fetchMetadata(g.metadata["id"].toString()); });
    connect(unlinkMetadata, &QPushButton::clicked, &d, [&] {
        try {
            autoMetadataQueued_.remove(g.id);
            library_->setMetadata(g.id, {}); g.metadata = {};
            const auto path = metadataCoverPath(library_->dataRoot(), g.id); QFile::remove(path); invalidateCover(path);
            updateMetadata(); refresh(); metadataStatus->setText("VNDB link removed. Local game settings are unchanged.");
        } catch (const std::exception &e) { metadataStatus->setText(QString::fromUtf8(e.what())); }
    });
    updateMetadata();
    auto launchTab = new QWidget(&d); auto f = new QFormLayout(launchTab);
    auto name = new QLineEdit(g.name, &d); auto exe = new QLineEdit(g.executable, &d); auto cwd = new QLineEdit(g.workingDirectory, &d);
    name->setObjectName("gameName");
    auto backend = backendBox(&d, g.backend); auto steamId = new QLineEdit(g.steamId, &d);
    auto args = new QPlainTextEdit(g.arguments.join('\n'), &d); args->setPlaceholderText("One argument per line; spaces within a line are preserved."); args->setMaximumHeight(100);
    f->addRow("Name", name); f->addRow("Executable", pathField(exe, &d, false, "Select game executable"));
    if (g.executable.isEmpty()) {
        auto choose = new QPushButton("Choose game executable…", &d); choose->setObjectName("chooseGameExecutable");
        f->addRow(choose);
        connect(choose, &QPushButton::clicked, &d, [this, &d, id = g.id] { d.reject(); chooseExecutable(id); });
    }
    f->addRow("Working directory", pathField(cwd, &d, true, "Select working directory"));
    f->addRow("Backend", backend); f->addRow("Steam launch ID", steamId); f->addRow("Arguments", args);
    auto location = new QLabel(g.folder, &d); location->setWordWrap(true); location->setTextFormat(Qt::PlainText);
    f->addRow("Game folder", location);
    auto relink = new QPushButton("Relink folder…", &d); relink->setObjectName("relinkFolderButton");
    relink->setEnabled(g.backend != "steam");
    relink->setToolTip(g.backend != "steam" ? "Keep this game's settings when its folder moves." : "Steam shortcut paths must currently be updated in Steam.");
    f->addRow(relink);
    connect(relink, &QPushButton::clicked, &d, [this, &g, exe, cwd, location, overviewFolder, overviewExecutable, overviewCwd] {
        relinkGame();
        const auto updated = library_->game(g.id);
        if (updated.folder != g.folder || updated.executable != g.executable || updated.workingDirectory != g.workingDirectory || updated.folderIdentity != g.folderIdentity) {
            g = updated; exe->setText(g.executable); cwd->setText(g.workingDirectory); location->setText(g.folder);
            overviewFolder->setText(g.folder); overviewExecutable->setText(g.executable); overviewCwd->setText(g.workingDirectory);
        }
    });
    tabs->addTab(launchTab, "Launch");
    auto runtimeScroll = new QScrollArea(&d); runtimeScroll->setWidgetResizable(true); runtimeScroll->setFrameShape(QFrame::NoFrame);
    auto runtimeTab = new QWidget(runtimeScroll); auto runtimeForm = new QFormLayout(runtimeTab);
    auto recipeChoice = new QComboBox(&d); recipeChoice->setObjectName("matchingRecipes");
    auto applyMatchedRecipe = new QPushButton("Preview and apply preset…", &d); applyMatchedRecipe->setObjectName("applyMatchedRecipe");
    auto recipeState = new QLabel("Current: " + (g.recipeId.isEmpty() ? "Legacy/manual" : g.recipeId) +
        (g.recipeOverrides.isEmpty() ? QString() : " (Modified)"), &d);
    recipeState->setWordWrap(true); recipeState->setTextFormat(Qt::PlainText);
    try {
        const auto found = inspect(g.folder, library_->root()).executables;
        const auto selection = selectRecipe(library_->root(), g.folder, found,
            librarySettings["default_recipe"].toString());
        for (const auto &recipe : selection.matches) recipeChoice->addItem(recipe.name, recipe.id);
        recipeChoice->insertSeparator(recipeChoice->count());
        for (const auto &recipe : loadRecipes(library_->root()))
            if (recipeChoice->findData(recipe.id) < 0) recipeChoice->addItem(recipe.name + " (manual)", recipe.id);
        if (recipeChoice->findData(g.recipeId) >= 0) recipeChoice->setCurrentIndex(recipeChoice->findData(g.recipeId));
    } catch (const std::exception &e) { recipeChoice->addItem(QString::fromUtf8(e.what())); applyMatchedRecipe->setEnabled(false); }
    runtimeForm->addRow("Preset", recipeChoice); runtimeForm->addRow(recipeState); runtimeForm->addRow(applyMatchedRecipe);
    connect(applyMatchedRecipe, &QPushButton::clicked, &d, [this, &d, recipeChoice, id = g.id] {
        try {
            const auto selectedId = recipeChoice->currentData().toString();
            if (selectedId.isEmpty()) throw std::runtime_error("Choose a preset.");
            const auto current = library_->game(id);
            const auto proposed = library_->previewRecipe(id, selectedId);
            const auto before = recipeSettingsSnapshot(current), after = recipeSettingsSnapshot(proposed);
            QStringList changes;
            QSet<QString> keys;
            for (auto it = before.begin(); it != before.end(); ++it) keys.insert(it.key());
            for (auto it = after.begin(); it != after.end(); ++it) keys.insert(it.key());
            for (const auto &key : keys) if (before[key] != after[key])
                changes.append(key + ": " + QString::fromUtf8(QJsonDocument(QJsonArray{before[key], after[key]}).toJson(QJsonDocument::Compact)));
            if (current.runner != proposed.runner) changes.append("installed runner: " + current.runner + " → " + proposed.runner);
            for (const auto &recipe : loadRecipes(library_->root())) if (recipe.id == selectedId &&
                recipe.tier != "general" && !recipeMatches(recipe, current.folder, inspect(current.folder, library_->root()).executables))
                changes.prepend("This preset does not match the detected game files. You are choosing it manually.");
            const auto summary = (changes.isEmpty() ? "No effective settings change." : changes.join("\n")) +
                "\n\nThe existing prefix, save mappings and server link stay in place. A changed runner or new Winetricks request may change the prefix during a later confirmed Prepare.";
            if (QMessageBox::question(&d, "Apply preset?", summary, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            library_->applyRecipe(id, selectedId);
            d.accept(); refresh(); message("Recipe applied. Review the game's executable, runner and save paths before Play.");
        } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot apply recipe", QString::fromUtf8(e.what())); }
    });
    auto exportPreset = new QPushButton("Export current preset…", &d); exportPreset->setObjectName("exportGameRecipe");
    runtimeForm->addRow(exportPreset);
    connect(exportPreset, &QPushButton::clicked, &d, [this, &d, id = g.id] {
        try {
            const auto saved = library_->game(id);
            const auto proposedId = askPresetId(&d, "Export game preset", saved.name.toLower().replace(' ', '-') + "-custom");
            if (proposedId.isEmpty()) return;
            QStringList omissions;
            QJsonObject document;
            const auto recipes = loadRecipes(library_->root());
            const Recipe *parent = nullptr;
            for (const auto &recipe : recipes) if (recipe.id == saved.recipeId) { parent = &recipe; break; }
            document = gameRecipeDocument(saved, proposedId, library_->root(), parent, &omissions);
            const auto path = QFileDialog::getSaveFileName(&d, "Export game preset", proposedId + ".json", "JSON files (*.json)");
            if (path.isEmpty()) return;
            const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
            QString destinationTier;
            for (const auto &tier : {"general", "games"})
                if (canonicalPath(QFileInfo(path).absolutePath()) == canonicalPath(recipeRoot(library_->root()) + '/' + tier))
                    destinationTier = QString::fromLatin1(tier);
            if (!destinationTier.isEmpty()) {
                if (QFileInfo(path).fileName() != proposedId + ".json")
                    throw std::runtime_error("A library recipe filename must match its recipe ID.");
                saveRecipe(library_->root(), destinationTier, bytes);
            } else {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                    throw std::runtime_error("Could not write game preset.");
            }
            QMessageBox::information(&d, "Preset exported", "Saved a manual game preset for review.\n" + omissions.join("\n"));
        } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot export preset", QString::fromUtf8(e.what())); }
    });
    auto runner = runnerBox(&d, g.runnerSelector.isEmpty() ? g.runner : g.runnerSelector, library_->root());
    bool runnerEdited = false;
    connect(runner, &QComboBox::currentTextChanged, &d, [&] { runnerEdited = true; });
    auto locale = new QComboBox(&d); locale->setEditable(true);
    locale->addItems({"", "ja_JP.UTF-8", "en_US.UTF-8"}); locale->setCurrentText(g.locale);
    auto requestedWinetricks = new QLineEdit(g.winetricks.join(", "), &d);
    requestedWinetricks->setObjectName("requestedWinetricks");
    requestedWinetricks->setPlaceholderText("None (clean prefix)");
    requestedWinetricks->setToolTip("Comma-separated approved components. Saving requests them; Prepare asks before installing. Removing a request does not uninstall it from the prefix.");
    auto effective = new QLabel(&d); effective->setObjectName("effectiveSetup");
    effective->setWordWrap(true); effective->setTextFormat(Qt::PlainText);
    auto updateEffective = [=, this] {
        if (backend->currentData() != "umu") {
            effective->setText(backend->currentData() == "native" ?
                "Native Linux game: no Proton runner or Wine prefix is used." :
                "Steam manages this shortcut's runner and prefix."); return;
        }
        QString runnerSource, runnerValue = runner->currentText().trimmed();
        if (!runnerValue.isEmpty()) runnerSource = "Per-game Proton";
        else {
            const auto libraryRunner = librarySettings["runner"].toString();
            if (!libraryRunner.isEmpty()) {
                runnerSource = "Library default Proton"; runnerValue = libraryRunner;
            } else {
                runnerValue = resolveRunnerSelector(library_->root(), "ge-proton-latest");
                runnerSource = "Latest installed GE-Proton";
            }
        }
        if (runnerValue.isEmpty()) { runnerSource = "Proton"; runnerValue = "choose at first Play"; }
        effective->setText("Saved preset: " + (g.recipeId.isEmpty() ? "Legacy/manual" : g.recipeId) +
                           "\n" + runnerSource + ": " + runnerValue +
                           "\nLocale: " + (locale->currentText().isEmpty() ? "inherit system" : locale->currentText()) +
                           "\nArguments and environment overrides are editable in Launch and Advanced.");
    };
    auto prefix = new QLabel(library_->prefix(g.id), &d); prefix->setWordWrap(true); prefix->setTextInteractionFlags(Qt::TextSelectableByMouse);
    prefix->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto evidence = new QLabel(g.evidence, &d); evidence->setWordWrap(true); evidence->setTextFormat(Qt::PlainText);
    runtimeForm->addRow("Proton directory", runner); runtimeForm->addRow("Windows language / locale (blank = inherit)", locale);
    runtimeForm->addRow("Winetricks components", requestedWinetricks);
    runtimeForm->addRow("Effective setup", effective);
    connect(runner, &QComboBox::currentTextChanged, &d, updateEffective);
    connect(locale, &QComboBox::currentTextChanged, &d, updateEffective);
    connect(backend, &QComboBox::currentIndexChanged, &d, updateEffective);
    updateEffective();
    locale->setToolTip("Choose ja_JP.UTF-8 for older Japanese installers that display garbled text. Save before retrying an installer; this also sets the game's launch locale.");
    if (g.source == "installer") {
        auto localeNote = new QLabel("For garbled Japanese installer text, choose ja_JP.UTF-8 and Save. To rerun a completed installer, use Run another executable below.", &d);
        localeNote->setWordWrap(true); runtimeForm->addRow(localeNote);
    }
    auto openProtonManager = new QPushButton("Open Proton Manager in Settings…", &d);
    openProtonManager->setObjectName("openProtonManager"); runtimeForm->addRow(openProtonManager);
    connect(openProtonManager, &QPushButton::clicked, &d, [&, runner] {
        settings();
        for (const auto &path : installedRunners(library_->root())) if (runner->findText(path) < 0) runner->addItem(path);
    });
    auto revision = new QLabel("Accepted runner: " + g.runnerRevision + "\nSaving settings accepts the selected runner's current version.", &d);
    revision->setWordWrap(true); runtimeForm->addRow(revision);
    runtimeForm->addRow("Dedicated prefix", prefix); runtimeForm->addRow("Engine evidence", evidence);
    auto prepare = new QPushButton("Prepare with saved settings", &d); runtimeForm->addRow(prepare);
    prepare->setEnabled(g.backend == "umu");
    prepare->setToolTip("Run prefix setup again in place using the saved configuration. Novelty does not delete the prefix or saves.");
    connect(prepare, &QPushButton::clicked, &d, [this, &d] { d.reject(); launch(true); });
    auto toolsRow = new QHBoxLayout;
    for (const auto &tool : {QString("winecfg"), QString("regedit")}) {
        auto action = new QPushButton(tool == "winecfg" ? "Wine configuration…" : "Registry editor…", &d);
        action->setObjectName(tool + "Button"); action->setEnabled(g.backend == "umu");
        action->setToolTip("Open with saved settings in this game's prefix. Other game operations wait until the tool closes.");
        toolsRow->addWidget(action);
        connect(action, &QPushButton::clicked, &d, [this, &d, tool] { d.reject(); launch(false, tool); });
    }
    runtimeForm->addRow(toolsRow);
    auto debug = new QPushButton("Launch once with Wine debug", &d); debug->setObjectName("debugLaunchButton");
    debug->setEnabled(g.backend == "umu" && !g.installPending && QFileInfo(g.executable).isFile());
    debug->setToolTip("Capture Wine loader and exception traces in the next launch log (up to 1 MiB). Saved settings stay unchanged.");
    runtimeForm->addRow(debug);
    connect(debug, &QPushButton::clicked, &d, [this, &d] { d.reject(); launch(false, {}, {}, false, true); });
    auto runExtra = new QPushButton("Run another executable…", &d); runExtra->setObjectName("runExtraButton");
    runExtra->setEnabled(g.backend == "umu"); runtimeForm->addRow(runExtra);
    connect(runExtra, &QPushButton::clicked, &d, [this, &d] { runExecutable(&d); });
    runtimeScroll->setWidget(runtimeTab); tabs->addTab(runtimeScroll, "Runtime");
    auto savesTab = new QWidget(&d); auto savesLayout = new QVBoxLayout(savesTab);
    auto savesIntro = new QLabel("Novelty suggests possible save locations. Select a location below or browse to one, then press Save to confirm it. Confirmed saves are backed up after Play.", savesTab);
    savesIntro->setWordWrap(true); savesLayout->addWidget(savesIntro);
    auto discoveryStatus = new QLabel(savesTab); discoveryStatus->setObjectName("saveDiscoveryStatus");
    discoveryStatus->setWordWrap(true); discoveryStatus->setTextFormat(Qt::PlainText);
    savesLayout->addWidget(discoveryStatus);
    auto serverLinkLabel = new QLabel("Server game link", savesTab);
    auto serverGameKey = new QLineEdit(savesTab); serverGameKey->setObjectName("serverGameKey");
    serverGameKey->setToolTip("Independent installations can use the same explicit key to share one server save state. P2P installations already share a stable default key.");
    try {
        const auto config = loadSaveConfig(*library_, g.id);
        serverGameKey->setText(config.serverGameKey);
        serverGameKey->setPlaceholderText(config.profileGameId);
        const bool serverMode = loadSaveServerConfig(*library_).enabled;
        serverLinkLabel->setVisible(serverMode); serverGameKey->setVisible(serverMode);
    } catch (const std::exception &) {
        serverLinkLabel->hide(); serverGameKey->hide();
    }
    savesLayout->addWidget(serverLinkLabel); savesLayout->addWidget(serverGameKey);
    auto mappingRow = new QHBoxLayout;
    auto mappingPath = new QLineEdit(savesTab); mappingPath->setObjectName("saveLocation");
    mappingPath->setPlaceholderText("Save folder or file, for example UserData");
    mappingPath->setToolTip("Type a path relative to this game, or Browse within its folder or Wine prefix. Save this change with the Manage dialog's Save button.");
    auto browseSave = new QPushButton("Browse…", savesTab);
    mappingRow->addWidget(mappingPath, 1); mappingRow->addWidget(browseSave);
    savesLayout->addLayout(mappingRow);
    auto candidateLabel = new QLabel("Possible save folders", savesTab);
    savesLayout->addWidget(candidateLabel);
    auto saveCandidates = new QListWidget(savesTab); saveCandidates->setObjectName("saveLocationCandidates");
    saveCandidates->setMaximumHeight(140);
    savesLayout->addWidget(saveCandidates);
    auto legacyPaths = new QLabel(savesTab); legacyPaths->setWordWrap(true); legacyPaths->setTextFormat(Qt::PlainText);
    savesLayout->addWidget(legacyPaths);
    savesLayout->addWidget(new QLabel("Backup history", savesTab));
    auto snapshots = new QListWidget(savesTab); snapshots->setObjectName("saveSnapshots");
    savesLayout->addWidget(snapshots, 1);
    auto snapshotActions = new QHBoxLayout;
    auto restoreSnapshot = new QPushButton("Restore selected…", savesTab);
    restoreSnapshot->setObjectName("restoreSaveSnapshot");
    auto syncFromPeer = new QPushButton("Transfer saves…", savesTab); syncFromPeer->setObjectName("syncSavesFromPeer");
    auto undoPeerTransfer = new QPushButton("Undo last transfer…", savesTab);
    undoPeerTransfer->setObjectName("undoPeerSaveTransfer");
    auto resolveServer = new QPushButton("Resolve server saves…", savesTab);
    resolveServer->setObjectName("resolveServerSaves");
    snapshotActions->addWidget(restoreSnapshot); snapshotActions->addWidget(syncFromPeer);
    snapshotActions->addWidget(undoPeerTransfer); snapshotActions->addWidget(resolveServer); snapshotActions->addStretch();
    savesLayout->addLayout(snapshotActions);
    syncFromPeer->setEnabled(false); undoPeerTransfer->setEnabled(false); resolveServer->hide();
    auto saveMessage = new QLabel(savesTab); saveMessage->setWordWrap(true); saveMessage->setTextFormat(Qt::PlainText);
    savesLayout->addWidget(saveMessage);
    auto renderSnapshots = [this, snapshots, id = g.id] {
        snapshots->clear();
        for (const auto &snapshot : listSaveSnapshots(*library_, id)) {
            auto item = new QListWidgetItem(snapshot.created + " · " + snapshot.reason + " · " +
                                            QString::number(snapshot.files) + " files · " +
                                            QString::number(snapshot.bytes) + " bytes", snapshots);
            item->setData(Qt::UserRole, snapshot.id);
        }
    };
    const auto displaySaveLocation = [this, g](const SaveMapping &mapping) {
        if (mapping.scope == "game") {
            auto folder = QDir(library_->root()).relativeFilePath(g.folder);
            if (folder == "." || folder == ".." || folder.startsWith("../"))
                folder = QFileInfo(g.folder).fileName();
            return '/' + QDir::cleanPath(folder + '/' + mapping.path);
        }
        const auto scope = mapping.scope == "appdata" ? "AppData" :
                           mapping.scope == "documents" ? "Documents" : "Prefix";
        return QString(scope) + ":/" + mapping.path;
    };
    auto renderSaveStatus = [this, discoveryStatus, mappingPath, candidateLabel, saveCandidates,
                             legacyPaths, g, displaySaveLocation] {
        const auto config = loadSaveConfig(*library_, g.id);
        saveCandidates->clear();
        const auto candidates = config.mappings.isEmpty() ? saveDiscoveryCandidates(*library_, g.id) : QList<SaveMapping>{};
        const bool needsChoice = !candidates.isEmpty();
        candidateLabel->setVisible(needsChoice);
        saveCandidates->setVisible(needsChoice);
        for (const auto &candidate : candidates)
            if (needsChoice) new QListWidgetItem(displaySaveLocation(candidate), saveCandidates);
        if (config.mappings.isEmpty()) {
            discoveryStatus->setText(needsChoice ?
                "Possible save locations found. Select one below, then press Save to confirm." :
                "Not found — select a save folder or file. Novelty will check again after Play.");
            legacyPaths->clear();
            return;
        }
        discoveryStatus->setText("Confirmed: " + displaySaveLocation(config.mappings.first()));
        if (config.mappings.size() > 1) {
            QStringList others;
            for (const auto &mapping : config.mappings) others.append(displaySaveLocation(mapping));
            legacyPaths->setText(QString::number(config.mappings.size()) +
                " save locations are configured. They remain protected unless you replace them with a new path:\n" +
                others.join("\n"));
        } else legacyPaths->clear();
        if (!mappingPath->isModified()) mappingPath->setText(displaySaveLocation(config.mappings.first()));
    };
    try {
        renderSnapshots(); renderSaveStatus();
        const bool serverMode = loadSaveServerConfig(*library_).enabled;
        syncFromPeer->setEnabled(!serverMode);
        undoPeerTransfer->setEnabled(!serverMode && QFileInfo::exists(library_->gamePath(g.id, "save-transfer-undo.json")));
        resolveServer->setVisible(serverMode && library_->status(g) == "Save conflict");
        if (serverMode) saveMessage->setText("Automatic server sync is enabled. Peer save transfer is available when it is turned off in Settings.");
    } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    connect(saveCandidates, &QListWidget::currentItemChanged, &d, [mappingPath, saveMessage](QListWidgetItem *item) {
        if (!item) return;
        mappingPath->setText(item->text());
        mappingPath->setModified(true);
        saveMessage->clear();
    });
    connect(browseSave, &QPushButton::clicked, &d, [&, mappingPath, displaySaveLocation] {
        QMenu choices(&d);
        auto folder = choices.addAction("Select folder…");
        auto file = choices.addAction("Select file…");
        auto selected = choices.exec(browseSave->mapToGlobal(QPoint(0, browseSave->height())));
        if (!selected) return;
        const auto chosen = selected == folder ?
            QFileDialog::getExistingDirectory(&d, "Choose save folder", g.folder) :
            QFileDialog::getOpenFileName(&d, "Choose save file", g.folder);
        if (chosen.isEmpty()) return;
        try {
            mappingPath->setText(displaySaveLocation(saveMappingFromPath(*library_, g, chosen)));
            mappingPath->setModified(true); saveMessage->clear();
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    auto editedSaveMappings = [this, &g, mappingPath, serverGameKey]() -> std::optional<SaveConfig> {
        if (!mappingPath->isModified() && !serverGameKey->isModified()) return std::nullopt;
        auto config = loadSaveConfig(*library_, g.id);
        if (serverGameKey->isModified()) config.serverGameKey = serverGameKey->text().trimmed();
        if (mappingPath->isModified()) {
            const auto path = mappingPath->text().trimmed();
            if (path.isEmpty()) {
                config.mappings.clear(); config.discoveryState = "searching"; config.discoverySource.clear();
            } else {
                config.mappings = {saveMappingFromPath(*library_, g, path)};
                config.discoveryState = "identified"; config.discoverySource = "manual";
            }
        }
        return config;
    };
    connect(syncFromPeer, &QPushButton::clicked, &d, [&, renderSnapshots, renderSaveStatus] {
        try {
            if (mappingPath->isModified()) { saveMessage->setText("Save the new location before transferring saves."); return; }
            syncSavesFromPeer(g.id, &d);
            renderSnapshots(); renderSaveStatus();
            undoPeerTransfer->setEnabled(QFileInfo::exists(library_->gamePath(g.id, "save-transfer-undo.json")));
            saveMessage->clear();
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(undoPeerTransfer, &QPushButton::clicked, &d, [&, renderSnapshots, renderSaveStatus] {
        if (mappingPath->isModified()) { saveMessage->setText("Save the new location before undoing a transfer."); return; }
        try {
            const auto root = library_->root(), id = g.id;
            auto check = std::async(std::launch::async, [root, id] {
                Library worker(root); return peerTransferUndo(worker, id);
            });
            RemovalProgress checking(&d); checking.setWindowTitle("Checking transfer Undo");
            auto checkingLayout = new QVBoxLayout(&checking);
            checkingLayout->addWidget(new QLabel("Verifying the saved Undo point and current progress…", &checking));
            QTimer checkPoll; checkPoll.setInterval(60);
            connect(&checkPoll, &QTimer::timeout, &checking, [&] {
                if (check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) checking.accept();
            });
            checkPoll.start(); checking.exec(); checkPoll.stop();
            const auto undo = check.get();
            if (undo.rollbackSnapshotId.isEmpty()) { saveMessage->setText("No transfer Undo is available."); return; }
            const auto warning = undo.liveChanged ?
                "Current saves changed since the transfer. Novelty will keep them in Backup history, then restore the state from before the transfer. Continue?" :
                "Restore the saves from before the last peer transfer?";
            if (QMessageBox::question(&d, "Undo last transfer?", warning,
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            auto work = std::async(std::launch::async, [root, id, preserveChanged = undo.liveChanged] {
                Library worker(root); undoPeerSaveTransfer(worker, id, preserveChanged);
            });
            RemovalProgress restoring(&d); restoring.setWindowTitle("Undoing peer transfer");
            auto restoringLayout = new QVBoxLayout(&restoring);
            restoringLayout->addWidget(new QLabel("Restoring the saved pre-transfer state…", &restoring));
            QTimer restorePoll; restorePoll.setInterval(60);
            connect(&restorePoll, &QTimer::timeout, &restoring, [&] {
                if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) restoring.accept();
            });
            restorePoll.start(); restoring.exec(); restorePoll.stop(); work.get();
            renderSnapshots(); renderSaveStatus(); undoPeerTransfer->setEnabled(false);
            saveMessage->setText("The prior saves were restored. Any changed progress was kept in Backup history.");
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(resolveServer, &QPushButton::clicked, &d, [&, renderSnapshots, renderSaveStatus] {
        if (mappingPath->isModified() || serverGameKey->isModified()) { saveMessage->setText("Save the new location or server link before resolving server saves."); return; }
        try {
            const auto config = loadSaveServerConfig(*library_);
            const auto root = library_->root(), id = g.id;
            auto check = std::async(std::launch::async, [root, id, config] {
                Library worker(root); return reviewServerSaveConflict(worker, id, config);
            });
            RemovalProgress checking(&d); checking.setWindowTitle("Checking server saves");
            auto checkingLayout = new QVBoxLayout(&checking);
            checkingLayout->addWidget(new QLabel("Checking current local files and the server revision…", &checking));
            QTimer checkPoll; checkPoll.setInterval(60);
            connect(&checkPoll, &QTimer::timeout, &checking, [&] {
                if (check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) checking.accept();
            });
            checkPoll.start(); checking.exec(); checkPoll.stop();
            const auto preview = check.get();
            QMessageBox choice(&d);
            choice.setWindowTitle("Resolve server saves");
            choice.setIcon(QMessageBox::Warning);
            choice.setText("Choose which complete save state to keep for " + g.name + ".");
            choice.setInformativeText("Local: " + QString::number(preview.verifiedLive["files"].toInt()) +
                " files. Server revision " + QString::number(preview.remote.revision) + ": " +
                QString::number(preview.remote.files) + " files" +
                (preview.remote.revision > 0 && preview.remote.files == 0 ? " (committed empty state)" : "") +
                ". The replaced local state stays in Backup history. The choice is checked again before anything changes.");
            auto useLocal = choice.addButton("Keep local saves", QMessageBox::AcceptRole);
            auto useServer = choice.addButton("Use server saves", QMessageBox::DestructiveRole);
            useServer->setEnabled(preview.remoteCompatible);
            if (!preview.remoteCompatible) choice.setInformativeText(choice.informativeText() + "\n" + preview.remoteIssue);
            choice.addButton(QMessageBox::Cancel);
            choice.exec();
            if (choice.clickedButton() != useLocal && choice.clickedButton() != useServer) return;
            const bool local = choice.clickedButton() == useLocal;
            auto work = std::async(std::launch::async, [root, id, config, preview, local] {
                Library worker(root); resolveServerSaveConflict(worker, id, config, preview, local);
            });
            RemovalProgress resolving(&d); resolving.setWindowTitle("Resolving server saves");
            auto resolvingLayout = new QVBoxLayout(&resolving);
            resolvingLayout->addWidget(new QLabel("Verifying and applying the chosen save state…", &resolving));
            QTimer resolvePoll; resolvePoll.setInterval(60);
            connect(&resolvePoll, &QTimer::timeout, &resolving, [&] {
                if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) resolving.accept();
            });
            resolvePoll.start(); resolving.exec(); resolvePoll.stop(); work.get();
            renderSnapshots(); renderSaveStatus(); resolveServer->hide();
            saveMessage->setText("Chosen saves accepted by the server. Replaced local progress remains in Backup history.");
            refresh();
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(restoreSnapshot, &QPushButton::clicked, &d, [&, renderSnapshots, renderSaveStatus] {
        if (mappingPath->isModified()) { saveMessage->setText("Save the new location before restoring."); return; }
        const auto item = snapshots->currentItem(); if (!item) { saveMessage->setText("Select a snapshot first."); return; }
        if (QMessageBox::question(&d, "Restore saves?", "Restore the selected snapshot? Novelty will first make a verified backup of the current mapped saves.",
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        try {
            const auto root = library_->root(), id = g.id, snapshotId = item->data(Qt::UserRole).toString();
            auto work = std::async(std::launch::async, [root, id, snapshotId] {
                Library worker(root); restoreSaveSnapshot(worker, id, snapshotId);
            });
            RemovalProgress progress(&d); progress.setWindowTitle("Restoring saves");
            auto progressLayout = new QVBoxLayout(&progress);
            progressLayout->addWidget(new QLabel("Verifying and restoring mapped saves…", &progress)); progress.resize(420, 100);
            QTimer poll; poll.setInterval(60);
            connect(&poll, &QTimer::timeout, &progress, [&] {
                if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
            });
            poll.start(); progress.exec(); poll.stop(); work.get();
            renderSnapshots(); renderSaveStatus(); saveMessage->setText("Saves restored. The replaced state was kept as a snapshot.");
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    auto savesScroll = new QScrollArea(&d); savesScroll->setWidgetResizable(true); savesScroll->setFrameShape(QFrame::NoFrame);
    savesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    savesScroll->setWidget(savesTab); tabs->addTab(savesScroll, "Saves");
    auto advanceTab = new QWidget(&d); auto al = new QVBoxLayout(advanceTab);
    auto advancedForm = new QFormLayout;
    auto umuSelector = new QLineEdit(g.umuLauncher, advanceTab); umuSelector->setObjectName("gameUmuLauncher");
    umuSelector->setPlaceholderText("Blank = library launcher; umu-latest, umu-1.4.4 or an executable path");
    auto umuGameId = new QLineEdit(g.umuGameId, advanceTab); umuGameId->setObjectName("gameUmuGameId");
    umuGameId->setPlaceholderText("Blank = umu-default");
    auto gameStore = new QLineEdit(g.store, advanceTab); gameStore->setObjectName("gameStore");
    gameStore->setPlaceholderText("Blank = none");
    advancedForm->addRow("umu launcher", umuSelector); advancedForm->addRow("umu game ID", umuGameId);
    advancedForm->addRow("Store", gameStore); al->addLayout(advancedForm);
    auto hint = new QLabel("Environment variables: one KEY=value per line. DLL overrides can be set with WINEDLLOVERRIDES. Runner and prefix are managed separately.", &d); hint->setWordWrap(true); al->addWidget(hint);
    QStringList lines; for (auto it = g.environment.begin(); it != g.environment.end(); ++it) lines.append(it.key() + '=' + it.value());
    auto env = new QPlainTextEdit(lines.join('\n'), &d); al->addWidget(env); tabs->addTab(advanceTab, "Advanced");
    auto logTab = new QWidget(&d); auto ll = new QVBoxLayout(logTab);
    auto log = new QPlainTextEdit(&d); log->setReadOnly(true); QFile file(library_->gamePath(g.id, "launch.log"));
    if (file.open(QIODevice::ReadOnly)) log->setPlainText(QString::fromUtf8(file.read(1024 * 1024)));
    else log->setPlainText("No launch log yet.");
    try {
        const auto operation = readJson(library_->gamePath(g.id, "extra-launch.json"));
        if (!operation.isEmpty()) log->appendPlainText("\nLast one-time executable operation:\n" + QString::fromUtf8(QJsonDocument(operation).toJson()));
    } catch (const std::exception &e) { log->appendPlainText("\nCannot read the one-time operation record: " + QString::fromUtf8(e.what())); }
    ll->addWidget(log);
    auto exportReport = new QPushButton("Export diagnostic report…", &d); exportReport->setObjectName("exportDiagnosticButton");
    exportReport->setToolTip("Save the game status, setup/operation state and current launch log as readable JSON. Review paths and log output before sharing.");
    ll->addWidget(exportReport);
    connect(exportReport, &QPushButton::clicked, &d, [this, &d, id = g.id] {
        const auto destination = QFileDialog::getSaveFileName(&d, "Export diagnostic report", QDir::home().filePath("novelty-" + id + "-diagnostic.json"), "JSON report (*.json)");
        if (destination.isEmpty()) return;
        try {
            const auto payload = QJsonDocument(diagnosticReport(*library_, id)).toJson(QJsonDocument::Indented);
            QSaveFile report(destination);
            if (!report.open(QIODevice::WriteOnly) || report.write(payload) != payload.size() || !report.commit())
                throw std::runtime_error(("Cannot save diagnostic report: " + report.errorString()).toStdString());
            message("Diagnostic report saved to " + destination);
        } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot export report", QString::fromUtf8(e.what())); }
    });
    auto last = readJson(library_->gamePath(g.id, "status.json")); auto error = new QLabel(last["message"].toString(), &d); error->setWordWrap(true); error->setTextFormat(Qt::PlainText); ll->addWidget(error);
    tabs->addTab(logTab, "Logs");
    auto bottom = new QHBoxLayout; auto remove = new QPushButton("Remove game…", &d); remove->setObjectName("removeGameButton");
    auto addToSteam = new QPushButton("Add to Steam", &d); addToSteam->setObjectName("addToSteamButton");
    addToSteam->setEnabled(g.backend == "umu" && !g.executable.isEmpty() && !g.installPending);
    addToSteam->setToolTip("Add a non-Steam shortcut that launches this saved game through Novelty.");
    const auto removalIssue = library_->removalIssue(g);
    remove->setEnabled(removalIssue.isEmpty()); remove->setToolTip(removalIssue);
    if (!removalIssue.isEmpty()) { auto note = new QLabel(removalIssue, &d); note->setWordWrap(true); layout->addWidget(note); }
    remove->setStyleSheet("QPushButton { color: #edb2b2; border-color: #865459; } QPushButton:hover { background: #4b2934; }");
    bottom->addWidget(remove); bottom->addStretch(); bottom->addWidget(addToSteam);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &d); bottom->addWidget(buttons); layout->addLayout(bottom);
    connect(remove, &QPushButton::clicked, &d, [this, &d, &g] { removeGame(g.id, &d); });
    connect(addToSteam, &QPushButton::clicked, &d, [this, &d, &g] {
        try {
            const auto userdata = QDir::homePath() + "/.local/share/Steam/userdata";
            QStringList accounts;
            for (const auto &account : QDir(userdata).entryInfoList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name)) {
                bool numeric = !account.fileName().isEmpty();
                for (const auto ch : account.fileName()) numeric &= ch.isDigit();
                if (numeric && QFileInfo(account.absoluteFilePath() + "/config").isDir()) accounts.append(account.fileName());
            }
            if (accounts.isEmpty()) throw std::runtime_error("No Steam user profile was found. Sign in to Steam once, then retry.");
            QString account = accounts.first();
            if (accounts.size() > 1) {
                QDialog choose(&d); choose.setWindowTitle("Steam account");
                auto select = new QVBoxLayout(&choose);
                select->addWidget(new QLabel("Add this shortcut to which Steam account?", &choose));
                auto choices = new QComboBox(&choose); choices->addItems(accounts); select->addWidget(choices);
                auto actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &choose);
                select->addWidget(actions);
                connect(actions, &QDialogButtonBox::accepted, &choose, &QDialog::accept);
                connect(actions, &QDialogButtonBox::rejected, &choose, &QDialog::reject);
                if (choose.exec() != QDialog::Accepted) return;
                account = choices->currentText();
            }
            const auto path = userdata + '/' + account + "/config/shortcuts.vdf";
            const auto packagedLauncher = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Novelty");
            const auto launcher = QFileInfo(packagedLauncher).isFile() && QFileInfo(packagedLauncher).isExecutable() ?
                                  canonicalPath(packagedLauncher) : QCoreApplication::applicationFilePath();
            const auto result = addSteamShortcut(*library_, library_->game(g.id), path, launcher);
            QMessageBox::information(&d, "Steam shortcut", result.added ?
                "Added to Steam. Restart Steam to load the new shortcut. It will launch this game through Novelty's saved settings." :
                "This game already has a Novelty shortcut in that Steam account.");
        } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot add to Steam", QString::fromUtf8(e.what())); }
    });
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &d, [&] {
        try {
            g.name = name->text().trimmed(); g.executable = exe->text(); g.workingDirectory = cwd->text();
            if (runnerEdited) g.selectRunner(runner->currentText());
            g.locale = locale->currentText(); g.backend = backend->currentData().toString(); g.steamId = steamId->text().trimmed();
            const auto launcher = umuSelector->text().trimmed(), gameId = umuGameId->text().trimmed();
            if (!launcher.isEmpty() && !QDir::isAbsolutePath(launcher) &&
                !QRegularExpression("^umu-(latest|[0-9]+(?:[.][0-9]+)*)$").match(launcher).hasMatch())
                throw std::runtime_error("Choose a managed umu selector or an absolute launcher path.");
            if (!gameId.isEmpty() && !QRegularExpression("^[A-Za-z0-9_-]{1,80}$").match(gameId).hasMatch())
                throw std::runtime_error("umu game IDs may contain letters, digits, underscores and dashes (up to 80 characters).");
            if (g.umuLauncher != launcher) {
                g.umuLauncher = launcher; g.umuResolved = launcher.isEmpty() ? QString() : resolveUmuSelector(library_->root(), launcher);
            }
            g.umuGameId = gameId; g.store = gameStore->text().trimmed();
            g.winetricks.clear();
            for (const auto &entry : requestedWinetricks->text().split(',', Qt::SkipEmptyParts)) {
                const auto verb = entry.trimmed();
                if (verb.isEmpty()) continue;
                if (!g.winetricks.contains(verb)) g.winetricks.append(verb);
            }
            validateWinetricks(g.winetricks);
            if (!g.winetricks.isEmpty() && g.backend != "umu")
                throw std::runtime_error("Winetricks components require the umu backend.");
            g.arguments = args->toPlainText().split('\n', Qt::SkipEmptyParts); g.environment.clear();
            for (auto line : env->toPlainText().split('\n', Qt::SkipEmptyParts)) {
                const int pos = line.indexOf('='); if (pos <= 0) throw std::runtime_error("Environment entries must use KEY=value.");
                g.environment.insert(line.left(pos).trimmed(), line.mid(pos + 1));
            }
            const auto saveConfig = editedSaveMappings();
            const bool savePathChanged = mappingPath->isModified();
            const bool serverLinkChanged = serverGameKey->isModified();
            library_->save(g, saveConfig ? &*saveConfig : nullptr);
            mappingPath->setModified(false); serverGameKey->setModified(false); d.accept();
            if (savePathChanged && !loadSaveConfig(*library_, g.id).mappings.isEmpty()) {
                const auto root = library_->root(), id = g.id;
                auto work = std::async(std::launch::async, [root, id] {
                    Library worker(root); return createSaveSnapshot(worker, id);
                });
                RemovalProgress progress(this); progress.setWindowTitle("Checking saved progress");
                auto progressLayout = new QVBoxLayout(&progress);
                progressLayout->addWidget(new QLabel("Verifying the selected save location…", &progress));
                progress.resize(420, 100);
                QTimer poll; poll.setInterval(60);
                connect(&poll, &QTimer::timeout, &progress, [&] {
                    if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
                });
                poll.start(); progress.exec(); poll.stop();
                try { work.get(); }
                catch (const std::exception &e) {
                    if (!QString::fromUtf8(e.what()).startsWith("No saved data yet")) {
                        const auto issue = "Save location was saved, but progress could not be captured: " + QString::fromUtf8(e.what());
                        library_->recordStatus(g.id, "Save needs attention", issue); message(issue);
                    }
                }
            }
            if ((savePathChanged || serverLinkChanged) && library_->status(library_->game(g.id)) != "Save needs attention") {
                try {
                    if (loadSaveServerConfig(*library_).enabled) {
                        library_->recordStatus(g.id, "Sync pending", "Checking updated save settings with the server.");
                        QTimer::singleShot(0, this, [this] { retryPendingServerSaves(); });
                    }
                } catch (const std::exception &) { /* Settings remain available for review. */ }
            }
        } catch (const std::exception &e) { QMessageBox::warning(&d, "Cannot save", QString::fromUtf8(e.what())); }
    }); d.exec(); refresh();
}
void Window::removeGame(const QString &id, QDialog *manageDialog) {
    if (!library_ || id.isEmpty() || removing_) return;
    try {
        const auto g = library_->game(id);
        const auto plan = library_->planRemoval(g.id);
        QMessageBox confirm(manageDialog ? static_cast<QWidget *>(manageDialog) : this);
        confirm.setObjectName("removeGameDialog"); confirm.setWindowTitle("Delete game?");
        confirm.setIcon(QMessageBox::Warning);
        confirm.setTextFormat(Qt::PlainText);
        confirm.setText("Delete “" + g.name + "” and its local files?");
        confirm.setInformativeText("This removes the game folder, Novelty prefix (including local saves), settings and logs. This cannot be undone.");
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirm.setDefaultButton(QMessageBox::No);
        auto syncedSaves = new QCheckBox("Delete synced saves", &confirm);
        syncedSaves->setObjectName("deleteSyncedSaves"); syncedSaves->setEnabled(false);
        syncedSaves->setToolTip("Removing a local game does not delete snapshots already stored on paired devices.");
        confirm.setCheckBox(syncedSaves);
        confirm.button(QMessageBox::Yes)->setObjectName("confirmRemoval");
        if (confirm.exec() != QMessageBox::Yes) return;
        removing_ = true; ++scanGeneration_; scanDelay_.stop(); copyTracker_.clear(); selectGame();
        const auto root = library_->root();
        auto work = std::async(std::launch::async, [root, plan] { Library lib(root); return lib.remove(plan); });
        RemovalProgress progress(manageDialog ? static_cast<QWidget *>(manageDialog) : this); progress.setObjectName("removalProgress"); progress.setWindowTitle("Removing game"); progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
        auto progressLayout = new QVBoxLayout(&progress); progressLayout->addWidget(new QLabel("Removing " + g.name + "…", &progress)); progress.resize(420, 100);
        QTimer poll; poll.setInterval(60); connect(&poll, &QTimer::timeout, &progress, [&] { if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept(); });
        poll.start(); progress.exec(); poll.stop();
        const auto result = work.get(); removing_ = false; if (manageDialog) manageDialog->reject(); refresh(); scanDelay_.start();
        if (result.cleanupPaths.isEmpty() && result.recoveryMessage.isEmpty()) message("Removed " + g.name + " and its local game data.");
        else {
            message("Removed the library entry; some files still need cleanup.");
            QMessageBox::warning(this, "Removal incomplete", result.recoveryMessage + "\n\nUse Rescan to retry recovery. Remaining folders:\n\n" + result.cleanupPaths.join("\n\n"));
        }
    } catch (const std::exception &e) {
        removing_ = false; refresh(); scanDelay_.start(); QMessageBox::warning(manageDialog, "Cannot remove game", QString::fromUtf8(e.what()));
    }
}
void Window::relinkGame() {
    const auto g = selected(); if (!library_ || g.id.isEmpty() || model_->remoteGames.contains(g.id)) return;
    QDialog dialog(this); dialog.setObjectName("relinkDialog"); dialog.setWindowTitle("Relink folder — " + g.name); dialog.resize(800, 520);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout;
    auto folder = new QLineEdit(g.folder, &dialog); folder->setObjectName("relinkFolder");
    auto exe = new QLineEdit(&dialog); exe->setObjectName("relinkExecutable");
    auto cwd = new QLineEdit(&dialog); cwd->setObjectName("relinkWorkingDirectory");
    form->addRow("New game folder", pathField(folder, &dialog, true, "Locate this game's folder"));
    form->addRow("Executable", pathField(exe, &dialog, false, "Choose this game's executable"));
    form->addRow("Working directory", pathField(cwd, &dialog, true, "Choose working directory")); layout->addLayout(form);
    auto preview = new QLabel(&dialog); preview->setTextFormat(Qt::PlainText); preview->setWordWrap(true); layout->addWidget(preview, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    auto apply = buttons->button(QDialogButtonBox::Save); apply->setText("Relink"); apply->setObjectName("applyRelink");
    std::optional<RelinkPlan> plan;
    auto updatePreview = [&](bool resetPaths) {
        try {
            auto next = library_->planRelink(g.id, folder->text(), resetPaths ? QString() : exe->text(), resetPaths ? QString() : cwd->text());
            if (resetPaths) {
                exe->blockSignals(true); cwd->blockSignals(true);
                exe->setText(next.updated.executable); cwd->setText(next.updated.workingDirectory);
                exe->blockSignals(false); cwd->blockSignals(false);
            }
            auto text = "Keep this game's name, settings and prefix:\n" + library_->prefix(g.id) + "\n\nGame files will remain in the selected folder.";
            if (!next.duplicateId.isEmpty()) text += "\n\nThe unused discovered entry “" + next.duplicateName + "” will be replaced by this existing game entry. It has no setup or user configuration.";
            if (!next.updated.executable.isEmpty() && !QFileInfo(next.updated.executable).isFile()) text += "\n\nChoose the executable: its previous relative path is missing.";
            const auto pathIssue = launchPathIssue(next.updated); if (!pathIssue.isEmpty()) text += "\n\n" + pathIssue;
            preview->setText(text); plan = next; apply->setEnabled(true);
        } catch (const std::exception &e) { plan.reset(); apply->setEnabled(false); preview->setText(QString::fromUtf8(e.what())); }
    };
    connect(folder, &QLineEdit::textChanged, &dialog, [&] { updatePreview(true); });
    connect(exe, &QLineEdit::textChanged, &dialog, [&] { updatePreview(false); });
    connect(cwd, &QLineEdit::textChanged, &dialog, [&] { updatePreview(false); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!plan) return;
        try { library_->relink(*plan); dialog.accept(); }
        catch (const std::exception &e) { QMessageBox::warning(&dialog, "Cannot relink", QString::fromUtf8(e.what())); updatePreview(false); }
    });
    updatePreview(true);
    if (dialog.exec() == QDialog::Accepted) { refresh(); message("Folder relinked. This game's settings and prefix were kept."); scanDelay_.start(); }
}
} // namespace novelty
