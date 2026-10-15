#include "window_internal.h"

namespace novelty {
void Window::settings() {
    QJsonObject currentSettings;
    try { currentSettings = library_ ? library_->settings() : defaultLibrarySettings(); }
    catch (const std::exception &error) { message(QString::fromUtf8(error.what())); return; }
    QDialog dialog(this); dialog.setObjectName("settingsDialog"); dialog.setWindowTitle("Settings"); dialog.resize(750, 520);
    auto layout = new QVBoxLayout(&dialog);
    auto scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto content = new QWidget(scroll); auto body = new QVBoxLayout(content); body->setContentsMargins(0, 0, 4, 0);
    auto form = new QFormLayout;
    auto folderField = new QLineEdit(rootPath_, &dialog); folderField->setObjectName("settingsGamesFolder");
    form->addRow("Games folder", pathField(folderField, &dialog, true, "Choose your main Games folder"));
    auto s = currentSettings; auto backend = backendBox(&dialog, s["backend"].toString(), false);
    auto runner = runnerBox(&dialog, s["runner"].toString(), library_ ? library_->root() : QString());
    runner->setObjectName("settingsRunner");
    auto umu = new QLineEdit(s["umu"].toString(), &dialog);
    auto steam = new QLineEdit(s["steam"].toString(), &dialog);
    form->addRow("Fallback backend", backend); form->addRow("Default Proton directory", runner);
    auto defaultRecipe = new QComboBox(&dialog); defaultRecipe->setObjectName("defaultRecipe");
    if (library_) try {
        for (const auto &recipe : loadRecipes(library_->root())) if (recipe.tier == "general")
            defaultRecipe->addItem(recipe.name, recipe.id);
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    const auto savedDefault = s["default_recipe"].toString();
    defaultRecipe->setCurrentIndex(defaultRecipe->findData(savedDefault));
    form->addRow("Default recipe", defaultRecipe);
    auto presetActions = new QHBoxLayout;
    auto exportLibrary = new QPushButton("Export library preset…", &dialog);
    auto importLibrary = new QPushButton("Import library preset…", &dialog);
    presetActions->addWidget(exportLibrary); presetActions->addWidget(importLibrary);
    form->addRow(presetActions);
    connect(exportLibrary, &QPushButton::clicked, &dialog, [&, this] {
        try {
            if (!library_) throw std::runtime_error("Choose a Games folder first.");
            const auto id = askPresetId(&dialog, "Export library preset", "my-library-defaults");
            if (id.isEmpty()) return;
            QStringList omissions;
            const auto document = libraryPresetDocument(library_->settings(), id, library_->root(), &omissions);
            const auto path = QFileDialog::getSaveFileName(&dialog, "Export library preset", id + ".json", "JSON files (*.json)");
            if (path.isEmpty()) return;
            QSaveFile file(path);
            const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                throw std::runtime_error("Could not write library preset.");
            QMessageBox::information(&dialog, "Preset exported", omissions.isEmpty() ?
                "Portable library defaults exported." : "Portable library defaults exported.\n" + omissions.join("\n"));
        } catch (const std::exception &e) { QMessageBox::warning(&dialog, "Cannot export preset", QString::fromUtf8(e.what())); }
    });
    connect(importLibrary, &QPushButton::clicked, &dialog, [&, this] {
        try {
            if (!library_) throw std::runtime_error("Choose a Games folder first.");
            const auto path = QFileDialog::getOpenFileName(&dialog, "Import library preset", {}, "JSON files (*.json)");
            if (path.isEmpty()) return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read library preset.");
            const auto proposed = applyLibraryPreset(library_->settings(), parseLibraryPreset(file.read(64 * 1024 + 1)), library_->root());
            const auto summary = "Default recipe: " + proposed["default_recipe"].toString() +
                "\nBackend: " + proposed["backend"].toString() +
                "\nRunner: " + proposed["runner"].toString() +
                "\n\nOnly these portable defaults will be loaded into Settings. Save Settings to keep them.";
            if (QMessageBox::question(&dialog, "Load library preset?", summary) != QMessageBox::Yes) return;
            backend->setCurrentIndex(backend->findData(proposed["backend"].toString()));
            runner->setCurrentText(proposed["runner"].toString());
            defaultRecipe->setCurrentIndex(defaultRecipe->findData(proposed["default_recipe"].toString()));
        } catch (const std::exception &e) { QMessageBox::warning(&dialog, "Cannot import preset", QString::fromUtf8(e.what())); }
    });
    auto editRecipes = new QPushButton("Add or edit recipes…", &dialog); editRecipes->setObjectName("editRecipes");
    form->addRow(editRecipes);
    connect(editRecipes, &QPushButton::clicked, &dialog, [&, this] {
        const auto selectedDefault = defaultRecipe->currentData().toString();
        recipes();
        defaultRecipe->clear();
        if (!library_) return;
        try { for (const auto &recipe : loadRecipes(library_->root())) if (recipe.tier == "general")
            defaultRecipe->addItem(recipe.name, recipe.id); }
        catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
        defaultRecipe->setCurrentIndex(defaultRecipe->findData(selectedDefault));
    });
    bool managerPinChosen = false;
    auto protonManager = new QPushButton("Proton Manager…", &dialog); protonManager->setObjectName("protonManager");
    form->addRow(protonManager);
    connect(protonManager, &QPushButton::clicked, &dialog, [&, this] {
        if (!library_) { QMessageBox::warning(&dialog, "Proton Manager", "Choose a Games folder first."); return; }
        const auto selected = manageProton(&dialog, *library_, *geUpdates_, runner->currentText());
        if (!selected.isEmpty()) {
            if (runner->findText(selected) < 0) runner->addItem(selected);
            runner->setCurrentText(selected); managerPinChosen = true;
        }
    });
    form->addRow("umu executable", pathField(umu, &dialog, false, "Select umu-run"));
    auto getUmu = new QPushButton("Get umu…", &dialog); getUmu->setObjectName("getUmuSettings"); form->addRow(getUmu);
    connect(getUmu, &QPushButton::clicked, &dialog, [&] {
        const auto installed = provisionUmu(&dialog, folderField->text());
        if (!installed.isEmpty()) umu->setText(installed);
    });
    form->addRow("Steam executable", pathField(steam, &dialog, false, "Select Steam")); body->addLayout(form);
    SaveServerConfig existingServer;
    try { if (library_) existingServer = loadSaveServerConfig(*library_); }
    catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    auto serverEnabled = new QCheckBox("Automatically sync saves with my server", &dialog);
    serverEnabled->setObjectName("saveServerEnabled"); serverEnabled->setChecked(existingServer.enabled);
    body->addWidget(serverEnabled);
    auto serverForm = new QFormLayout;
    auto serverUrl = new QLineEdit(existingServer.url.toString(), &dialog);
    serverUrl->setObjectName("saveServerUrl"); serverUrl->setPlaceholderText("https://my-save-server.example");
    auto serverToken = new QLineEdit(QString::fromLatin1(existingServer.token), &dialog);
    serverToken->setObjectName("saveServerToken"); serverToken->setEchoMode(QLineEdit::Password);
    serverForm->addRow("Server address", serverUrl); serverForm->addRow("Access token", serverToken);
    body->addLayout(serverForm);
    auto serverNote = new QLabel("When enabled, Play checks this server and uploads changed saves. Offline Play remains available. Peer save replacement is available after turning server sync off.", &dialog);
    serverNote->setWordWrap(true); body->addWidget(serverNote);
    if (library_ && !library_->managedRuntimeStorage()) {
        auto migrate = new QPushButton("Migrate legacy umu runtime into Games…", &dialog);
        migrate->setObjectName("migrateLegacyRuntime"); body->addWidget(migrate);
        connect(migrate, &QPushButton::clicked, &dialog, [&, this] {
            const auto source = QFileDialog::getExistingDirectory(&dialog, "Choose the existing umu runtime folder",
                                                                   legacyUmuRuntimePath());
            if (source.isEmpty()) return;
            const auto root = library_->root();
            QDialog progress(&dialog); progress.setWindowTitle("Migrate umu runtime"); progress.resize(450, 140); progress.setModal(true);
            auto progressLayout = new QVBoxLayout(&progress);
            auto progressText = new QLabel("Copying and verifying the legacy runtime inside Games…", &progress);
            progressText->setWordWrap(true); progressLayout->addWidget(progressText);
            auto cancel = new QPushButton("Cancel", &progress); progressLayout->addWidget(cancel, 0, Qt::AlignRight);
            connect(cancel, &QPushButton::clicked, &progress, &QDialog::reject);
            std::promise<void> promise; auto result = promise.get_future();
            std::jthread worker([root, source, promise = std::move(promise)](std::stop_token stop) mutable {
                try { Library independent(root); migrateLegacyUmuRuntime(independent, source, stop); promise.set_value(); }
                catch (...) { promise.set_exception(std::current_exception()); }
            });
            QTimer poll(&progress); poll.setInterval(100);
            bool finished = false, succeeded = false; QString issue;
            connect(&poll, &QTimer::timeout, &progress, [&] {
                if (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
                finished = true; poll.stop();
                try { result.get(); succeeded = true; }
                catch (const std::exception &e) { issue = QString::fromUtf8(e.what()); }
                progress.accept();
            });
            poll.start(); progress.exec();
            if (!finished) {
                poll.stop(); worker.request_stop();
                progressText->setText("Stopping migration and removing the incomplete Games-local copy…");
                cancel->setEnabled(false); progress.show();
                QEventLoop cleanup;
                QTimer cleanupPoll(&cleanup); cleanupPoll.setInterval(100);
                connect(&cleanupPoll, &QTimer::timeout, &cleanup, [&] {
                    if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) cleanup.quit();
                });
                cleanupPoll.start(); cleanup.exec(); progress.hide();
                finished = true;
                try { result.get(); succeeded = true; }
                catch (const std::exception &e) { issue = QString::fromUtf8(e.what()); }
            }
            worker.join();
            if (succeeded) { migrate->setEnabled(false); message("Legacy umu runtime verified inside Games. The original remains available to other libraries."); }
            else if (finished) QMessageBox::warning(&dialog, "Runtime migration", issue);
        });
    }
    auto devicesButton = new QPushButton("Devices and pairing…", &dialog);
    devicesButton->setObjectName("devicesSettings"); devicesButton->setEnabled(bool(library_));
    connect(devicesButton, &QPushButton::clicked, &dialog, [this] { devices(); });
    body->addWidget(devicesButton);
    auto note = new QLabel("Recipes define game settings. Library Proton and umu choices are used when the recipe leaves those selectors blank; Manage can override them per game.\n\nProton Manager installs and removes GE builds. Choose the library default above. Get umu installs the launcher for this library. Manage → Add to Steam creates a shortcut to Novelty's existing launch path.", &dialog); note->setWordWrap(true); body->addWidget(note);
    scroll->setWidget(content); layout->addWidget(scroll, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    try {
        const auto folder = folderField->text().trimmed();
        if (folder.isEmpty()) throw std::runtime_error("Choose a Games folder.");
        const bool changedRoot = !library_ || canonicalPath(folder) != library_->root();
        if (changedRoot) {
            if (!pendingLaunch_.isEmpty()) throw std::runtime_error("Wait until this launch finishes before switching libraries.");
            openRoot(folder);
        }
        s["backend"] = backend->currentData().toString(); s["runner"] = runner->currentText();
        if (!changedRoot) {
            if (defaultRecipe->currentIndex() < 0) throw std::runtime_error("Choose an available default recipe.");
            s["default_recipe"] = defaultRecipe->currentData().toString();
        }
        s["umu"] = umu->text(); s["steam"] = steam->text();
        if (!changedRoot) {
            SaveServerConfig configured;
            configured.enabled = serverEnabled->isChecked();
            configured.url = QUrl(serverUrl->text().trimmed());
            configured.token = serverToken->text().toLatin1();
            configured.serverId = existingServer.serverId;
            const bool serverChanged = configured.enabled != existingServer.enabled ||
                configured.url != existingServer.url || configured.token != existingServer.token;
            if (serverChanged) {
                for (const auto &running : library_->games())
                    if (library_->busy(running.id))
                        throw std::runtime_error("Wait until active games close before changing save server settings.");
            }
            if (serverChanged && configured.enabled) {
                const auto url = configured.url;
                const auto token = configured.token;
                auto check = std::async(std::launch::async, [url, token] {
                    SaveServerClient client(url, token); return client.identity();
                });
                RemovalProgress progress(this); progress.setWindowTitle("Checking save server");
                auto progressLayout = new QVBoxLayout(&progress);
                progressLayout->addWidget(new QLabel("Checking the server identity and access token…", &progress));
                QTimer poll; poll.setInterval(60);
                connect(&poll, &QTimer::timeout, &progress, [&] {
                    if (check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
                });
                poll.start(); progress.exec(); poll.stop();
                configured.serverId = check.get();
            }
            if (serverChanged) {
                saveSaveServerConfig(*library_, configured);
                for (const auto &game : library_->games()) {
                    const auto state = library_->status(game);
                    if (configured.enabled && state == "Ready") {
                        try {
                            if (!loadSaveConfig(*library_, game.id).mappings.isEmpty())
                                library_->recordStatus(game.id, "Sync pending", "Checking this game's saves with the server.");
                        } catch (const std::exception &) { /* Save settings remain visible in Manage. */ }
                    }
                    else if (!configured.enabled && (state == "Sync pending" || state == "Save conflict"))
                        library_->recordStatus(game.id, "Ready", "Automatic server sync is off. Local save history remains available.");
                }
                serverRetryFailures_.clear(); serverNextRetry_.clear();
                if (configured.enabled) QTimer::singleShot(0, this, [this] { retryPendingServerSaves(); });
            }
        }
        // A selected Games folder owns its own defaults. Switching to a copied
        // library must retain its relocated runner and umu paths.
        if (!changedRoot) {
            if (managerPinChosen || s["runner"] != library_->settings()["runner"])
                syncGeProvisioningPolicy(*library_, s["runner"].toString());
            library_->setSettings(s);
        }
        refresh();
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
void Window::recipes() {
    if (!library_) return;
    QDialog dialog(this); dialog.setObjectName("recipesDialog"); dialog.setWindowTitle("Recipes"); dialog.resize(800, 600);
    auto layout = new QVBoxLayout(&dialog);
    auto note = new QLabel("Game recipes match first; Settings chooses the fallback from General. All conditions must pass; any needs one match when present. Paths are relative to the game folder.", &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto row = new QHBoxLayout; layout->addLayout(row, 1);
    auto list = new QListWidget(&dialog); list->setObjectName("recipeList"); list->setMinimumWidth(220); row->addWidget(list, 1);
    auto right = new QVBoxLayout; row->addLayout(right, 3);
    auto tier = new QComboBox(&dialog); tier->setObjectName("recipeTier");
    tier->addItem("General", "general"); tier->addItem("Games", "games"); right->addWidget(tier);
    auto editor = new QPlainTextEdit(&dialog); editor->setObjectName("recipeJson"); right->addWidget(editor, 1);
    auto error = new QLabel(&dialog); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); right->addWidget(error);
    auto actions = new QHBoxLayout; right->addLayout(actions);
    auto add = new QPushButton("New", &dialog); add->setObjectName("newRecipe"); actions->addWidget(add);
    auto importButton = new QPushButton("Import…", &dialog); importButton->setObjectName("importRecipe"); actions->addWidget(importButton);
    auto save = new QPushButton("Save", &dialog); save->setObjectName("saveRecipe"); actions->addWidget(save);
    auto remove = new QPushButton("Delete", &dialog); remove->setObjectName("deleteRecipe"); actions->addWidget(remove);
    auto close = new QPushButton("Close", &dialog); actions->addWidget(close);
    QString currentId, currentTier;
    const auto root = library_->root();
    const auto reload = [&] {
        list->clear();
        for (const auto &folder : {"general", "games"}) {
            const auto directory = recipeRoot(root) + '/' + folder;
            for (const auto &file : QDir(directory).entryList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
                const auto id = file.left(file.size() - 5), path = directory + '/' + file;
                QString label = id;
                QFile source(path);
                if (source.open(QIODevice::ReadOnly)) try { label = parseRecipe(source.read(256 * 1024 + 1), path, folder).name; }
                    catch (const std::exception &) { label += " (invalid)"; }
                auto item = new QListWidgetItem(QString("%1 / %2").arg(folder, label), list);
                item->setData(Qt::UserRole, QString(folder) + '/' + id);
            }
        }
    };
    connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        if (!item) return;
        const auto key = item->data(Qt::UserRole).toString();
        currentTier = key.section('/', 0, 0); currentId = key.section('/', 1, 1);
        tier->setCurrentIndex(tier->findData(currentTier)); tier->setEnabled(false);
        QFile source(recipePath(root, currentTier, currentId));
        if (source.open(QIODevice::ReadOnly)) editor->setPlainText(QString::fromUtf8(source.readAll()));
        error->clear(); remove->setEnabled(true);
    });
    connect(add, &QPushButton::clicked, &dialog, [&] {
        list->clearSelection(); currentId.clear(); currentTier.clear(); tier->setEnabled(true);
        const auto id = "new-recipe-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        const auto selectedTier = tier->currentData().toString();
        const QJsonObject match = selectedTier == "general" ? QJsonObject{} :
            QJsonObject{{"all", QJsonArray{QJsonObject{{"file", "MyGame.exe"}}}}};
        const QJsonObject settings = selectedTier == "general" ? QJsonObject{} :
            QJsonObject{{"executable", "MyGame.exe"}};
        editor->setPlainText(QString::fromUtf8(QJsonDocument(QJsonObject{{"schema", 1}, {"id", id},
            {"name", "New recipe"}, {"inherits", "general-game"}, {"category", "game"}, {"match", match}, {"settings", settings}}).toJson(QJsonDocument::Indented)));
        remove->setEnabled(false); error->clear();
    });
    connect(importButton, &QPushButton::clicked, &dialog, [&] {
        try {
            const auto path = QFileDialog::getOpenFileName(&dialog, "Import recipe", {}, "JSON files (*.json)");
            if (path.isEmpty()) return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read recipe.");
            const auto bytes = file.read(256 * 1024 + 1);
            const auto selectedTier = tier->currentData().toString();
            const auto parsed = parseRecipe(bytes, path, selectedTier);
            if (QFileInfo::exists(recipePath(root, selectedTier, parsed.id)) &&
                QMessageBox::question(&dialog, "Replace recipe?", "Replace the existing " + parsed.id + " recipe?") != QMessageBox::Yes) return;
            saveRecipe(root, selectedTier, bytes); reload();
            error->setText("Imported. Existing games retain their saved settings.");
        } catch (const std::exception &e) { error->setText(QString::fromUtf8(e.what())); }
    });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        try {
            const auto selectedTier = tier->currentData().toString();
            const auto bytes = editor->toPlainText().toUtf8();
            const auto parsed = parseRecipe(bytes, {}, selectedTier);
            if (!currentId.isEmpty() && (currentId != parsed.id || currentTier != selectedTier))
                throw std::runtime_error("To change a recipe ID or folder, create a new recipe and delete the old one.");
            saveRecipe(root, selectedTier, bytes);
            currentId = parsed.id; currentTier = selectedTier; reload();
            for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole) == currentTier + '/' + currentId)
                { list->setCurrentRow(i); break; }
            error->setText("Saved. Existing games keep their applied settings until you choose this recipe in Manage.");
        } catch (const std::exception &e) { error->setText(QString::fromUtf8(e.what())); }
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (currentId.isEmpty()) return;
        if (currentTier == "general" && library_->settings()["default_recipe"].toString() == currentId) {
            error->setText("Choose another default recipe in Settings before deleting this one."); return;
        }
        if (QMessageBox::question(&dialog, "Delete recipe?", "Delete this recipe file? Existing game settings will be kept.") != QMessageBox::Yes) return;
        try { removeRecipe(root, currentTier, currentId); currentId.clear(); currentTier.clear(); editor->clear(); reload(); remove->setEnabled(false); error->clear(); }
        catch (const std::exception &e) { error->setText(QString::fromUtf8(e.what())); }
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    reload(); remove->setEnabled(false); dialog.exec();
}
} // namespace novelty
