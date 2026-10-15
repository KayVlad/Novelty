#include "window_internal.h"

namespace novelty {
namespace {
struct SetupHint { bool pending = false; QString message; };
SetupHint setupHint(const Library &lib, const Game &game, const QString &status) {
    // Active and failure states should retain their more urgent status colour.
    if (QStringList{"Unavailable", "Folder changed", "Path needs rename", "Recovery required",
                    "Needs attention", "Save needs attention", "Sync pending", "Save conflict",
                    "Interrupted", "Starting", "Preparing", "Configuring", "Running"}.contains(status)) return {};
    QStringList messages;
    if (game.executable.isEmpty())
        messages.append(game.installPending ? "Choose a game executable after installation finishes." :
                        "No executable selected yet. Choose one on Play or in Manage → Overview.");
    try {
        if (loadSaveConfig(lib, game.id).mappings.isEmpty())
            messages.append("No save folder found yet. Novelty checks again after Play; choose one in Manage → Saves if needed.");
    } catch (const std::exception &) {
        messages.append("Save settings need review in Manage → Saves.");
    }
    return {!messages.isEmpty(), messages.join(" ")};
}
}
QVariant GameModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() < 0 || i.row() >= games.size() || i.column() < 0 || i.column() >= 3) return {};
    const auto &g = games[i.row()];
    if (role == Qt::DisplayRole) {
        switch (i.column()) { case 0: return g.name; case 1: return g.engine; default: return states.value(i.row()); }
    }
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role == Qt::ForegroundRole && i.column() == 2)
        return statusColor(states.value(i.row()), setupPending.value(i.row()));
    if (role == EngineRole) return g.engine;
    if (role == StatusRole) return states.value(i.row());
    if (role == SetupPendingRole) return setupPending.value(i.row());
    if (role == LaunchableRole) return actionsAllowed && launchable.value(i.row());
    if (role == ManageableRole) return actionsAllowed && manageable.value(i.row());
    if (role == RemoteRole) return remoteGames.contains(g.id);
    if (role == CoverRole && remoteGames.contains(g.id)) return remoteGames.value(g.id).cover;
    if (role == CoverRole) return games[i.row()].metadata.isEmpty() ? QString() :
        metadataCoverPath(coverDataRoot, games[i.row()].id);
    if (role == Qt::ToolTipRole) return i.column() == 1 ? g.evidence : g.folder;
    return {};
}
QVariant GameModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o == Qt::Horizontal && role == Qt::DisplayRole) return QStringList{"Game", "Engine", "Status"}.value(section);
    return {};
}
void GameModel::load(const Library &lib, const QList<PeerGame> &peers) {
    beginResetModel(); games = lib.games(); coverDataRoot = lib.dataRoot(); states.clear(); launchable.clear(); manageable.clear(); setupPending.clear(); remoteGames.clear();
    QSet<QString> installed, linked;
    for (const auto &g : games) {
        try {
            const auto config = loadSaveConfig(lib, g.id);
            installed.insert(config.profileGameId);
            for (const auto &link : config.peerLinks) linked.insert(link.fingerprint + ':' + link.profileGameId);
        } catch (const std::exception &) { /* The local status already indicates settings recovery. */ }
        const auto state = lib.status(g);
        states.append(state); setupPending.append(setupHint(lib, g, state).pending);
        const bool available = !lib.busy(g.id);
        manageable.append(available);
        launchable.append(available && !g.installPending && QFileInfo(g.folder).isDir() &&
                          (g.executable.isEmpty() || QFileInfo(g.executable).isFile()));
    }
    for (const auto &peer : peers) {
        const auto remoteId = peer.descriptor["profile_game_id"].toString();
        if (installed.contains(remoteId) || linked.contains(peer.peer.fingerprint + ':' + remoteId)) continue;
        Game game; game.id = peer.id(); game.name = peer.descriptor["name"].toString();
        game.engine = peer.descriptor["engine"].toString(); game.metadata = peer.descriptor["metadata"].toObject();
        game.folder = "On " + peer.peer.name;
        remoteGames.insert(game.id, peer); games.append(game);
        states.append(peer.online ? "Not installed" : "Device offline");
        launchable.append(peer.online); manageable.append(false); setupPending.append(false);
    }
    endResetModel();
}
void GameModel::unavailable() {
    beginResetModel(); states.clear(); launchable.clear(); manageable.clear(); setupPending.clear();
    for (qsizetype row = 0; row < games.size(); ++row) { states.append("Unavailable"); setupPending.append(false); }
    endResetModel();
}
bool GameModel::updateState(const Library &lib, const QString &id) {
    for (int row = 0; row < games.size(); ++row) if (games[row].id == id) {
        const auto state = lib.status(games[row]);
        const bool pendingSetup = setupHint(lib, games[row], state).pending;
        const bool canManage = !lib.busy(id);
        const bool canLaunch = canManage && !games[row].installPending && QFileInfo(games[row].folder).isDir() &&
                               (games[row].executable.isEmpty() || QFileInfo(games[row].executable).isFile());
        if (states.value(row) == state && setupPending.value(row) == pendingSetup &&
            manageable.value(row) == canManage && launchable.value(row) == canLaunch) return false;
        states[row] = state; setupPending[row] = pendingSetup; manageable[row] = canManage; launchable[row] = canLaunch;
        emit dataChanged(index(row, 0), index(row, 2), {Qt::DisplayRole, Qt::ForegroundRole,
                         StatusRole, SetupPendingRole, LaunchableRole, ManageableRole});
        return true;
    }
    return false;
}
Window::Window(const QString &root, const QString &launcherExecutable, QUrl metadataApiBase)
    : launcherExecutable_(launcherExecutable.isEmpty() ? QCoreApplication::applicationFilePath() : launcherExecutable),
      metadataApiBase_(std::move(metadataApiBase)) {
    discoveryClock_.start();
    autoMetadataClient_ = new VndbClient(this, metadataApiBase_);
    setWindowTitle("Novelty"); resize(1280, 800); setMinimumSize(900, 620);
    auto central = new QWidget(this); setCentralWidget(central);
    auto layout = new QVBoxLayout(central); layout->setContentsMargins(20, 16, 20, 12); layout->setSpacing(16);
    auto top = new QHBoxLayout; top->setSpacing(12);
    auto brand = new QLabel("Novelty", this); brand->setObjectName("brand");
    top->addWidget(brand); top->addStretch();
    auto add = new QPushButton(uiIcon("plus"), "Add game…", this); add->setObjectName("addGameButton");
    auto config = new QPushButton(uiIcon("gear"), "Settings", this);
    top->addWidget(add); top->addWidget(config); layout->addLayout(top);
    auto filter = new QHBoxLayout; filter->setSpacing(10);
    search_ = new QLineEdit(this); search_->setObjectName("librarySearch"); search_->setPlaceholderText("Search games…");
    search_->addAction(uiIcon("search"), QLineEdit::LeadingPosition); search_->setClearButtonEnabled(true); filter->addWidget(search_, 1);
    auto modes = new QWidget(this); auto modeLayout = new QHBoxLayout(modes); modeLayout->setContentsMargins(0, 0, 0, 0); modeLayout->setSpacing(0);
    auto gridButton = new QToolButton(this); auto listButton = new QToolButton(this);
    gridButton->setObjectName("gridViewButton"); listButton->setObjectName("listViewButton");
    gridButton->setIcon(uiIcon("grid")); listButton->setIcon(uiIcon("list"));
    gridButton->setToolTip("Cover grid"); listButton->setToolTip("List view");
    gridButton->setAccessibleName("Cover grid"); listButton->setAccessibleName("List view");
    auto group = new QButtonGroup(this); group->setExclusive(true);
    for (auto button : {gridButton, listButton}) { button->setCheckable(true); button->setFixedSize(44, 44); group->addButton(button); modeLayout->addWidget(button); }
    filter->addWidget(modes); scanButton_ = new QPushButton(uiIcon("rescan"), "Rescan", this); filter->addWidget(scanButton_); layout->addLayout(filter);
    auto body = new QHBoxLayout; body->setSpacing(18);
    model_ = new GameModel(this); proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_); proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive); proxy_->setFilterKeyColumn(-1);
    table_ = new QTableView(this); table_->setObjectName("gameList"); table_->setModel(proxy_); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide(); table_->verticalHeader()->setDefaultSectionSize(52);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->setShowGrid(false); table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    listPlay_ = new QAction(uiIcon("play"), "Play", table_); listPlay_->setObjectName("listPlayAction");
    listManage_ = new QAction(uiIcon("manage"), "Manage…", table_); listManage_->setObjectName("listManageAction");
    listInstall_ = new QAction(uiIcon("plus"), "Install", table_); listInstall_->setObjectName("listInstallAction");
    listInstall_->setVisible(false); table_->addAction(listInstall_);
    connect(listInstall_, &QAction::triggered, this, [this] { installFromPeer(); });
    listManage_->setShortcut(QKeySequence("Ctrl+M")); listManage_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    table_->addAction(listPlay_); table_->addAction(listManage_); table_->setContextMenuPolicy(Qt::ActionsContextMenu);
    table_->setToolTip("Double-click to play. Right-click for Play and Manage, or press Ctrl+M to manage the selected game.");
    connect(listPlay_, &QAction::triggered, this, [this] { launch(); }); connect(listManage_, &QAction::triggered, this, [this] { manage(); });
    grid_ = new GameGrid(this); grid_->setModel(proxy_); grid_->setSelectionModel(table_->selectionModel());
    grid_->contextAction = [this](const QModelIndex &index, const QPoint &position) {
        table_->setCurrentIndex(index);
        const auto id = selected().id;
        auto menu = new QMenu(this); menu->setObjectName("cardMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
        if (index.data(RemoteRole).toBool()) {
            auto install = menu->addAction(uiIcon("plus"), "Install"); install->setObjectName("cardInstallAction");
            install->setEnabled(index.data(LaunchableRole).toBool());
            connect(install, &QAction::triggered, this, [this] { installFromPeer(); });
            menu->popup(position); return;
        }
        auto manageAction = menu->addAction(uiIcon("manage"), "Manage…"); manageAction->setObjectName("cardManageAction");
        auto removeAction = menu->addAction(uiIcon("trash"), "Delete…"); removeAction->setObjectName("cardDeleteAction");
        connect(manageAction, &QAction::triggered, this, [this] { manage(); });
        connect(removeAction, &QAction::triggered, this, [this, id] { removeGame(id, nullptr); });
        menu->popup(position);
    };
    views_ = new QStackedWidget(this); views_->addWidget(grid_); views_->addWidget(table_); body->addWidget(views_, 1);
    const bool useList = loadAppSettings()["libraryView"].toString("grid") == "list";
    views_->setCurrentIndex(useList ? 1 : 0); (useList ? listButton : gridButton)->setChecked(true);
    connect(gridButton, &QToolButton::clicked, this, [this] {
        // A table click may make Engine/Status the current column; cards use the title column.
        const auto index = table_->currentIndex();
        if (index.isValid()) table_->setCurrentIndex(index.siblingAtColumn(0));
        views_->setCurrentIndex(0);
        try { setAppSetting("libraryView", "grid"); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    });
    connect(listButton, &QToolButton::clicked, this, [this] {
        views_->setCurrentIndex(1);
        try { setAppSetting("libraryView", "list"); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    });
    auto panel = new QWidget(this); panel->setObjectName("detailPanel"); panel->setFixedWidth(340);
    auto p = new QVBoxLayout(panel); p->setContentsMargins(16, 16, 16, 16); p->setSpacing(12);
    auto scroll = new QScrollArea(panel); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Reserve scrollbar width: aspect-ratio cover height otherwise toggles the bar,
    // recursively alternating the content width when a peer cover arrives.
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->viewport()->setObjectName("detailViewport");
    auto content = new QWidget(scroll); content->setObjectName("detailContent"); auto info = new QVBoxLayout(content); info->setContentsMargins(0, 0, 4, 0); info->setSpacing(10);
    cover_ = new Cover(content); info->addWidget(cover_);
    title_ = new QLabel("Your next story", content); title_->setObjectName("gameTitle"); title_->setWordWrap(true); title_->setTextFormat(Qt::PlainText); info->addWidget(title_);
    originalTitle_ = new QLabel(content); originalTitle_->setObjectName("gameOriginalTitle"); originalTitle_->setWordWrap(true);
    originalTitle_->setTextFormat(Qt::PlainText); originalTitle_->setProperty("muted", true); originalTitle_->hide(); info->addWidget(originalTitle_);
    status_ = new QLabel(content); status_->setObjectName("gameStatus"); info->addWidget(status_);
    setupState_ = new QLabel(content); setupState_->setObjectName("gameSetupStatus");
    setupState_->setWordWrap(true); setupState_->setTextFormat(Qt::PlainText); setupState_->hide(); info->addWidget(setupState_);
    syncState_ = new QLabel(content); syncState_->setObjectName("gameSyncStatus");
    syncState_->setWordWrap(true); syncState_->setTextFormat(Qt::PlainText); info->addWidget(syncState_);
    description_ = new QLabel("No description yet.", content); description_->setObjectName("gameDescription"); description_->setWordWrap(true); description_->setTextFormat(Qt::PlainText); info->addWidget(description_);
    tags_ = new QLabel("No tags yet", content); tags_->setObjectName("metadataTags"); tags_->setTextFormat(Qt::PlainText); tags_->setWordWrap(true); info->addWidget(tags_);
    auto time = new QHBoxLayout; auto timeLabel = new QLabel("Expected play time", content); timeLabel->setProperty("muted", true); time->addWidget(timeLabel);
    playtimeValue_ = new QLabel("—", content); playtimeValue_->setObjectName("metadataPlaytime"); time->addWidget(playtimeValue_, 1); info->addLayout(time);
    detail_ = new QLabel(content); detail_->setWordWrap(true); detail_->setTextFormat(Qt::PlainText); detail_->setProperty("muted", true); info->addWidget(detail_); info->addStretch();
    scroll->setWidget(content); p->addWidget(scroll, 1); body->addWidget(panel); layout->addLayout(body, 1);
    auto footer = new QHBoxLayout; notice_ = new QLabel("", this); notice_->setObjectName("libraryNotice"); notice_->setTextFormat(Qt::PlainText); notice_->setWordWrap(true); footer->addWidget(notice_, 1);
    rootLabel_ = new QLabel("Choose your Games folder to begin.", this); rootLabel_->setProperty("muted", true); rootLabel_->setTextFormat(Qt::RichText); rootLabel_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard); rootLabel_->setMaximumWidth(380); rootLabel_->setObjectName("gamesFolderLink"); rootLabel_->setCursor(Qt::PointingHandCursor); footer->addWidget(rootLabel_); layout->addLayout(footer);
    setStyleSheet(R"(
        QWidget { background: #0a1423; color: #dce5f1; font-size: 14px; }
        QLabel { background: transparent; }
        QLabel[muted="true"], QLabel#libraryNotice, QLabel#gameDescription { color: #9eafc5; font-size: 13px; }
        QLabel#gameOriginalTitle { color: #9eafc5; font-size: 12px; }
        QLabel#gameSetupStatus { color: #8dcfff; font-size: 13px; }
        QLabel#brand { font-size: 27px; font-weight: bold; }
        QLabel#gameTitle { font-size: 25px; font-weight: bold; }
        QWidget#detailPanel { background: #122237; border: 1px solid #263b55; border-radius: 9px; }
        QWidget#detailContent, QWidget#detailViewport, QScrollArea { background: #122237; }
        QLabel#metadataTags { background: #21344d; border-radius: 4px; padding: 4px 8px; color: #b7c8dc; font-size: 12px; }
        QFrame#separator { color: #30425b; }
        QLineEdit, QPlainTextEdit, QComboBox { background: #15263b; border: 1px solid #2e425f; border-radius: 6px; padding: 11px; selection-background-color: #315f87; }
        QPushButton, QToolButton { background: #1b2c43; border: 1px solid #354b69; border-radius: 6px; padding: 10px 14px; min-height: 22px; }
        QDialog QLineEdit, QDialog QPlainTextEdit, QDialog QComboBox { padding: 6px 8px; }
        QDialog QPushButton, QDialog QToolButton { padding: 6px 10px; min-height: 20px; }
        QToolButton { padding: 0; }
        QPushButton:hover, QToolButton:hover { background: #29415f; }
        QPushButton:focus, QToolButton:focus, QLineEdit:focus, QComboBox:focus { border: 1px solid #71b0e0; }
        QPushButton:disabled { color: #77899f; border-color: #2a3b51; }
        QPushButton#addGameButton, QToolButton:checked { background: #315f87; border-color: #4c80aa; color: #eef5ff; }
        QPushButton#addGameButton:hover { background: #3e729e; }
        QListView { background: transparent; border: none; outline: none; }
        QTableView { background: #102036; alternate-background-color: #15263b; border: 1px solid #2e425f; border-radius: 6px; selection-background-color: #315f87; }
        QTableView::item { padding-left: 9px; padding-right: 12px; }
        QMenu { background: #15263b; color: #e6edf7; border: 1px solid #3a5270; padding: 5px; }
        QMenu::item { padding: 9px 22px 9px 12px; }
        QMenu::item:selected { background: #315f87; }
        QHeaderView::section { background: #1b2c43; color: #bcd2e4; padding: 12px; border: none; }
        QScrollArea { border: none; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QScrollBar::handle:vertical { background: #354d6c; border-radius: 3px; min-height: 32px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QTabBar::tab { background: #1b2c43; padding: 10px; margin-right: 3px; }
        QTabBar::tab:selected { background: #315f87; }
    )");
    connect(add, &QPushButton::clicked, this, [this] { addGame(); });
    connect(config, &QPushButton::clicked, this, [this] { settings(); });
    connect(rootLabel_, &QLabel::linkActivated, this, [this] {
        if (!rootPath_.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(rootPath_))) message("Could not open the Games folder in your file explorer.");
    });
    connect(scanButton_, &QPushButton::clicked, this, [this] { scan(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) {
        proxy_->setFilterFixedString(text);
        if (!table_->currentIndex().isValid() && proxy_->rowCount() > 0) table_->selectRow(0);
        selectGame();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { selectGame(); });
    connect(table_, &QTableView::doubleClicked, this, [this] { launch(); });
    // activated covers double-click and keyboard activation without launching twice.
    connect(grid_, &QListView::activated, this, [this] { launch(); });
    scanDelay_.setSingleShot(true); scanDelay_.setInterval(1500);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { scanDelay_.start(); });
    connect(&scanDelay_, &QTimer::timeout, this, [this] { if (pendingLaunch_.isEmpty()) scan(); });
    scanPoll_.setInterval(60);
    connect(&scanPoll_, &QTimer::timeout, this, [this] {
        if (importing_) return;
        if (!pendingScan_.valid() || pendingScan_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        scanPoll_.stop(); scanButton_->setEnabled(true);
        try {
            auto found = pendingScan_.get();
            if (removing_) return;
            if (!library_ || library_->root() != scanRoot_ || pendingScanGeneration_ != scanGeneration_) { scan(); return; }
            const int moved = library_->reconcileFolders(found);
            QStringList known; for (const auto &g : library_->games()) known.append(g.folder);
            found.candidates.removeIf([&](const Candidate &c) { return known.contains(c.folder); });
            const auto ready = copyTracker_.observe(found.candidates, discoveryClock_.elapsed());
            QSet<QString> existing; for (const auto &g : library_->games()) existing.insert(g.id);
            const int added = library_->import(ready, library_->settings()["runner"].toString());
            if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
            const auto failed = watcher_.addPaths(found.watchDirectories);
            refresh();
            for (const auto &g : library_->games()) if (!existing.contains(g.id)) queueAutomaticMetadata(g.id);
            if (copyTracker_.waiting()) {
                message(QString("%1 new folder(s): waiting for copying to settle… Existing games remain available.").arg(copyTracker_.waiting()));
                scanDelay_.start(2000);
            } else {
                message(QString("%1 games · %2 new · %3 relinked.").arg(model_->games.size()).arg(added).arg(moved) +
                        (failed.isEmpty() ? "" : " Some folders could not be watched; use Rescan after copying."));
                if (moved) scanDelay_.start();
            }
            if (!found.recoveryMessages.isEmpty()) message(found.recoveryMessages.join('\n'));
        } catch (const std::exception &e) { copyTracker_.clear(); refresh(); message(QString::fromUtf8(e.what())); }
    });
    launchPoll_.setInterval(500);
    connect(&launchPoll_, &QTimer::timeout, this, [this] {
        try {
            if (!library_) return;
            if (!rootAvailable()) {
                launchPoll_.stop(); pendingLaunch_.clear(); pendingPrepare_ = false; refresh();
                message("Games library unavailable. Reconnect it and press Rescan."); return;
            }
            const auto j = readJson(library_->gamePath(pendingLaunch_, "status.json"));
            const auto state = j["state"].toString();
            if (model_->updateState(*library_, pendingLaunch_)) selectGame();
            const bool active = library_->supervisorActive(pendingLaunch_);
            const auto recovery = active ? QString() : library_->recoveryIssue(pendingLaunch_);
            if (!active && (state != "Starting" || !recovery.isEmpty())) {
                const auto completedId = pendingLaunch_;
                const bool completedInstaller = pendingInstaller_;
                const bool completedPrepare = pendingPrepare_;
                launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false; pendingPrepare_ = false;
                refresh(); message(recovery.isEmpty() ? j["message"].toString(state) : recovery); scanDelay_.start();
                if (completedPrepare && recovery.isEmpty() && state == "Ready" &&
                    !loadSaveServerConfig(*library_).enabled &&
                    !importedSaveCandidates(*library_, completedId).isEmpty()) {
                    try {
                        const auto root = library_->root();
                        auto work = std::async(std::launch::async, [root, completedId] {
                            Library worker(root);
                            return adoptInitialPeerSave(worker, completedId);
                        });
                        RemovalProgress progress(this); progress.setWindowTitle("Applying peer saves");
                        auto layout = new QVBoxLayout(&progress);
                        layout->addWidget(new QLabel("Applying verified peer progress…", &progress));
                        progress.resize(420, 100);
                        QTimer poll; poll.setInterval(60);
                        connect(&poll, &QTimer::timeout, &progress, [&] {
                            if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
                        });
                        poll.start(); progress.exec(); poll.stop();
                        if (work.get()) { refresh(); message("Game ready with peer saves."); }
                    } catch (const std::exception &e) {
                        message("Game prepared. Peer saves still need attention: " + QString::fromUtf8(e.what()));
                    }
                }
                if (completedInstaller && recovery.isEmpty() && (state == "Ready" || state == "Installation pending") &&
                    library_->game(completedId).executable.isEmpty()) chooseExecutable(completedId);
                if (completedInstaller && recovery.isEmpty()) queueAutomaticMetadata(completedId);
            } else if (!active && QDateTime::currentMSecsSinceEpoch() - launchStarted_ > 10000) {
                launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false; pendingPrepare_ = false;
                message("The launch supervisor did not start. Check the command-line output."); selectGame();
            }
        } catch (const std::exception &e) { launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false; pendingPrepare_ = false; message(QString::fromUtf8(e.what())); }
    });
    syncTimer_.setInterval(30000);
    connect(&syncTimer_, &QTimer::timeout, this, [this] {
        refreshPeerGames(); retryPendingServerSaves(); selectGame();
    });
    selectGame();
    const auto initial = root.isEmpty() ? loadAppSettings()["gamesRoot"].toString() : root;
    if (!initial.isEmpty()) {
        rootPath_ = canonicalPath(initial); updateRootLink();
        const auto remembered = loadAppSettings()["gamesRoot"].toString();
        rootRequiresDatabase_ = !remembered.isEmpty() && canonicalPath(remembered) == rootPath_;
        try { openRoot(initial, !rootRequiresDatabase_); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    }
}
Window::~Window() {
    syncTimer_.stop();
    if (serverRetryProcess_ && serverRetryProcess_->state() != QProcess::NotRunning) {
        serverRetryProcess_->kill(); serverRetryProcess_->waitForFinished(1000);
    }
    peerDiscovery_.reset();
    for (auto connection : catalogueConnections_) { connection->close(); delete connection; }
    catalogueConnections_.clear();
    for (auto connection : autoConnections_) { connection->close(); delete connection; }
    autoConnections_.clear();
    activePeerIds_.clear();
    saveListener_.reset();
}
void Window::retryPendingServerSaves() {
    if (!library_ || !rootAvailable() || serverRetryProcess_ || !pendingLaunch_.isEmpty() || importing_ || removing_) return;
    try {
        if (!loadSaveServerConfig(*library_).enabled) return;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto &game : library_->games()) {
            if (library_->status(game) != "Sync pending" || library_->busy(game.id) ||
                serverNextRetry_.value(game.id) > now) continue;
            try { if (loadSaveConfig(*library_, game.id).mappings.isEmpty()) continue; }
            catch (const std::exception &) { continue; }
            const auto root = library_->root(), id = game.id;
            auto process = new QProcess(this);
            serverRetryProcess_ = process;
            const auto complete = [this, process, root, id](bool succeeded) {
                if (serverRetryProcess_ != process) return;
                serverRetryProcess_ = nullptr;
                process->deleteLater();
                if (!library_ || library_->root() != root) return;
                const auto state = library_->status(library_->game(id));
                if (succeeded || state == "Ready" || state == "Save conflict") {
                    serverRetryFailures_.remove(id); serverNextRetry_.remove(id);
                } else {
                    const auto failures = std::min(4, serverRetryFailures_.value(id) + 1);
                    serverRetryFailures_.insert(id, failures);
                    serverNextRetry_.insert(id, QDateTime::currentMSecsSinceEpoch() +
                                               qint64(30000) * (1 << failures));
                }
                model_->updateState(*library_, id); selectGame();
            };
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [complete](int code, QProcess::ExitStatus exit) { complete(exit == QProcess::NormalExit && code == 0); });
            connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) complete(false);
            });
            process->setProgram(launcherExecutable_);
            process->setArguments({"--root", root, "--sync-server-game", id});
            process->start();
            return;
        }
    } catch (const std::exception &error) { message(QString::fromUtf8(error.what())); }
}
void Window::message(const QString &s) { notice_->setText(s); }
void Window::updateRootLink() {
    rootLabel_->setToolTip(rootPath_);
    const auto label = QFontMetrics(rootLabel_->font()).elidedText(rootPath_, Qt::ElideMiddle, 375).toHtmlEscaped();
    rootLabel_->setText(rootPath_.isEmpty() ? "Choose a Games folder in Settings." : "<a style='color:#9eafc5' href='games-folder'>" + label + "</a>");
}
Game Window::selected() const {
    auto i = proxy_->mapToSource(table_->currentIndex());
    return i.isValid() ? model_->games.value(i.row()) : Game{};
}
bool Window::rootAvailable() const {
    return library_ && QFileInfo(library_->root()).isDir() && QFileInfo(library_->dataRoot() + "/library.sqlite").isFile();
}
void Window::openRoot(const QString &root, bool allowCreation) {
    if (!allowCreation && !QFileInfo(QDir(root).filePath(".novelty/library.sqlite")).isFile())
        throw std::runtime_error("Your Games library is unavailable. Reconnect it and press Rescan, or choose another Games folder.");
    auto next = std::make_unique<Library>(root);
    auto saveRecovery = next->recoverSettingsChanges();
    saveRecovery.append(recoverSaveRestores(*next));
    saveRecovery.append(recoverPeerSaveTransfers(*next));
    saveRecovery.append(recoverSaveObservations(*next));
    saveRecovery.append(recoverPendingSaveSnapshots(*next));
    setAppSetting("gamesRoot", next->root());
    peerDiscovery_.reset();
    for (auto connection : catalogueConnections_) { connection->close(); delete connection; }
    catalogueConnections_.clear(); cataloguePeers_.clear(); peerGames_.clear(); peerLastAttempt_.clear();
    for (auto connection : autoConnections_) { connection->close(); delete connection; }
    autoConnections_.clear();
    activePeerIds_.clear();
    saveListener_.reset();
    library_ = std::move(next); rootPath_ = library_->root(); copyTracker_.clear(); scanDelay_.stop();
    lanListenIssue_.clear();
    saveListener_ = std::make_unique<LanListener>(*library_, false, this);
    saveListener_->onConnection = [this](LanConnection *connection) {
        connection->onReady = [this, connection](const QString &) {
            auto exchange = new LanSaveExchange(*library_, *connection, connection);
            auto games = new LanGameExchange(*library_, *connection, *exchange, connection);
            games->previewData = [this](const QString &id) {
                const auto image = cachedCover(metadataCoverPath(library_->dataRoot(), id));
                QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
                if (!image.isNull()) image.scaled(240, 336, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "JPEG", 65);
                return QJsonObject{{"cover", bytes.size() <= 30 * 1024 ? QString::fromLatin1(bytes.toBase64()) : QString()}};
            };
            // Legacy unsolicited offers are deferred by LanSaveExchange. The listener
            // remains available for game installation and requested manual transfers.
        };
        connection->onError = [connection](const QString &) { connection->deleteLater(); };
        return true;
    };
    if (!saveListener_->listen(QHostAddress::AnyIPv4, 38373) &&
        !saveListener_->listen(QHostAddress::AnyIPv4, 0)) {
        lanListenIssue_ = "LAN save listening is unavailable: " + saveListener_->errorString();
        saveListener_.reset();
    }
    syncTimer_.start();
    if (saveListener_) {
        peerDiscovery_ = std::make_unique<LanDiscovery>(*library_, saveListener_->serverPort());
        peerDiscovery_->onPeer = [this](const TrustedPeer &peer) { queryPeerGames(peer); };
    }
    QTimer::singleShot(1000, this, [this] { refreshPeerGames(); });
    ++autoMetadataGeneration_; autoMetadataQueue_.clear(); autoMetadataQueued_.clear(); autoMetadataActive_ = false;
    rootRequiresDatabase_ = true; ++scanGeneration_;
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    watcher_.addPath(rootPath_);
    for (auto g : library_->games()) if (library_->supervisorActive(g.id)) {
        pendingLaunch_ = g.id; launchStarted_ = QDateTime::currentMSecsSinceEpoch(); launchPoll_.start(); break;
    }
    updateRootLink(); refresh();
    if (!saveRecovery.isEmpty()) message(saveRecovery.join("\n"));
    QTimer::singleShot(0, this, [this] { scan(); });
}
void Window::chooseRoot() {
    if (!pendingLaunch_.isEmpty()) { message("Wait until this launch finishes before switching libraries."); return; }
    const auto path = QFileDialog::getExistingDirectory(this, "Choose your main Games folder", library_ ? library_->root() : QDir::homePath());
    if (path.isEmpty()) return;
    try { openRoot(path); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
void Window::refresh() {
    const auto id = selected().id;
    QList<PeerGame> peers;
    for (const auto &catalogue : peerGames_) peers.append(catalogue);
    std::sort(peers.begin(), peers.end(), [](const PeerGame &a, const PeerGame &b) { return a.id() < b.id(); });
    if (rootAvailable()) model_->load(*library_, peers);
    else if (library_) model_->unavailable();
    for (int row = 0; row < model_->games.size(); ++row) if (model_->games[row].id == id) {
        table_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, 0))); break;
    }
    if (!table_->currentIndex().isValid() && proxy_->rowCount() > 0) table_->selectRow(0);
    selectGame();
}
void Window::selectGame() {
    const auto g = selected(); const bool valid = library_ && !g.id.isEmpty();
    const bool accessible = rootAvailable();
    model_->actionsAllowed = accessible && pendingLaunch_.isEmpty() && !removing_ && !importing_; grid_->viewport()->update();
    const auto index = table_->currentIndex();
    const bool remote = index.data(RemoteRole).toBool();
    listPlay_->setVisible(!remote); listManage_->setVisible(!remote); listInstall_->setVisible(remote);
    listInstall_->setEnabled(index.data(LaunchableRole).toBool());
    listPlay_->setEnabled(index.data(LaunchableRole).toBool()); listManage_->setEnabled(index.data(ManageableRole).toBool());
    GameMetadata metadata;
    if (valid && !g.metadata.isEmpty()) {
        metadata.cover = remote ? model_->remoteGames.value(g.id).cover : cachedCover(metadataCoverPath(library_->dataRoot(), g.id));
        metadata.description = g.metadata["description"].toString();
        metadata.tags = metadataStrings(g.metadata["tags"]).mid(0, 12);
        metadata.playtime = g.metadata["expected_play_time"].toString();
    }
    metadata.remote = remote;
    if (remote && !metadata.cover.isNull()) metadata.cover = metadata.cover.convertToFormat(QImage::Format_Grayscale8);
    cover_->setGame(valid ? g.name : QString(), metadata);
    description_->setText(metadata.description.isEmpty() ? "No description yet." :
        (metadata.description.size() > 500 ? metadata.description.left(500).trimmed() + "…" : metadata.description));
    playtimeValue_->setText(metadata.playtime.value_or("—")); tags_->setText(metadata.tags.isEmpty() ? "No tags yet" : metadata.tags.join(" · "));
    if (valid) {
        if (remote) {
            const auto peer = model_->remoteGames.value(g.id);
            title_->setText(g.name); originalTitle_->hide(); setupState_->hide(); syncState_->hide();
            status_->setText(peer.online ? "●  Not installed" : "●  Device offline");
            status_->setStyleSheet("color: #91a3b8; font-size: 13px;");
            detail_->setText(peer.online ? "Available on " + peer.peer.name + ". Double-click or right-click → Install." :
                "Open Novelty on " + peer.peer.name + " to install this game.");
            detail_->show(); return;
        }
        title_->setText(g.name); const auto state = accessible ? library_->status(g) : "Unavailable";
        originalTitle_->setText(g.metadata["original_title"].toString());
        originalTitle_->setVisible(!originalTitle_->text().isEmpty());
        const auto setup = accessible ? setupHint(*library_, g, state) : SetupHint{};
        status_->setText("●  " + state); status_->setStyleSheet("color: " + statusColor(state, setup.pending).name() + "; font-size: 13px;");
        setupState_->setText(setup.message); setupState_->setVisible(setup.pending);
        syncState_->clear();
        if (accessible && QFileInfo::exists(library_->gamePath(g.id, "save-transfer-undo.json"))) {
            try {
                if (!loadSaveServerConfig(*library_).enabled)
                    syncState_->setText("Last manual save transfer can be undone in Manage → Saves.");
            } catch (const std::exception &) { /* The status gives the settings issue. */ }
        }
        syncState_->setVisible(!syncState_->text().isEmpty());
        detail_->setText(!accessible ? "Reconnect your Games folder and press Rescan." : launchPathIssue(g));
        if (accessible && state == "Needs setup")
            detail_->setText("Play will verify setup in the existing prefix before starting the game. You can also use Prepare in Manage → Runtime.");
        if (accessible && state == "Recovery required") detail_->setText(library_->recoveryIssue(g.id));
        if (accessible && (state == "Save needs attention" || state == "Needs attention" ||
                           state == "Sync pending" || state == "Save conflict")) {
            const auto last = readJson(library_->gamePath(g.id, "status.json"));
            detail_->setText(last["state"] == state ? last["message"].toString() : pendingSaveSnapshotIssue(*library_, g.id));
        }
        detail_->setVisible(!detail_->text().isEmpty());
    } else {
        title_->setText("Your next story"); status_->setText("No game selected"); status_->setStyleSheet("color: #9eafc5; font-size: 13px;");
        setupState_->clear(); setupState_->hide();
        syncState_->clear(); syncState_->hide();
        originalTitle_->hide();
        detail_->setText(!search_->text().isEmpty() ? "No games match your search." : "Choose a Games folder in Settings, or use Add game to select a folder or run an installer."); detail_->show();
    }
}
void Window::scan() {
    refreshPeerGames();
    if (removing_ || importing_ || pendingScan_.valid() || !pendingLaunch_.isEmpty()) return;
    if (!library_) {
        if (!rootPath_.isEmpty()) try { openRoot(rootPath_, !rootRequiresDatabase_); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
        return;
    }
    if (!rootAvailable()) { copyTracker_.clear(); refresh(); message("Games library unavailable. Reconnect it and press Rescan."); return; }
    scanRoot_ = library_->root(); pendingScanGeneration_ = scanGeneration_; auto root = scanRoot_;
    QStringList known; for (const auto &g : model_->games) if (!model_->remoteGames.contains(g.id)) known.append(g.folder);
    pendingScan_ = std::async(std::launch::async, [root, known] {
        Library worker(root); auto recovered = worker.recoverSettingsChanges();
        recovered.append(worker.recoverRemovals()); recovered.append(worker.recoverFolderImports());
        recovered.append(worker.recoverInstallations());
        recovered.append(recoverSaveRestores(worker)); recovered.append(recoverPeerSaveTransfers(worker));
        recovered.append(recoverSaveObservations(worker));
        recovered.append(recoverPendingSaveSnapshots(worker));
        auto found = scanDiscovery(root, known); found.recoveryMessages = recovered; return found;
    });
    scanButton_->setEnabled(false); message("Checking for new games…"); scanPoll_.start();
}
void Window::addGame() {
    if (!library_) { chooseRoot(); if (!library_) return; }
    if (!pendingLaunch_.isEmpty() || removing_ || importing_) { message("Wait for the current operation to finish before adding a game."); return; }
    QDialog dialog(this); dialog.setObjectName("addGameDialog"); dialog.setWindowTitle("Add game"); dialog.resize(740, 540);
    auto layout = new QVBoxLayout(&dialog); auto tabs = new QTabWidget(&dialog); tabs->setObjectName("addGameTabs"); layout->addWidget(tabs);
    auto folderTab = new QWidget(tabs); auto folderForm = new QFormLayout(folderTab);
    auto folder = new QLineEdit(&dialog); folder->setObjectName("addGameFolder");
    auto folderNote = new QLabel("Move a game folder into Games, then detect its engine and executable. If it is already directly inside Games, Novelty scans it in place.", &dialog); folderNote->setWordWrap(true);
    folderForm->addRow(folderNote); folderForm->addRow("Game folder", pathField(folder, &dialog, true, "Select game folder"));
    auto nativeMode = new QCheckBox("Run a native Linux executable directly", &dialog);
    nativeMode->setObjectName("addGameNativeMode"); folderForm->addRow(nativeMode);
    auto nativePath = new QLineEdit(&dialog); nativePath->setObjectName("addGameNativeExecutable");
    nativePath->setPlaceholderText("Executable file inside the game folder");
    auto nativePicker = pathField(nativePath, &dialog, false, "Select Linux game executable", folder);
    nativePicker->setEnabled(false); folderForm->addRow("Linux executable", nativePicker);
    connect(nativeMode, &QCheckBox::toggled, nativePicker, &QWidget::setEnabled);
    tabs->addTab(folderTab, "Select folder");
    auto installerTab = new QWidget(tabs); auto form = new QFormLayout(installerTab);
    auto installer = new QLineEdit(&dialog); installer->setObjectName("addGameInstaller");
    auto name = new QLineEdit(&dialog); name->setObjectName("addGameName");
    auto runner = runnerBox(&dialog, library_->settings()["runner"].toString(), library_->root()); runner->setObjectName("addGameRunner");
    form->addRow("Windows installer (.exe)", pathField(installer, &dialog, false, "Select Windows installer"));
    form->addRow("Game / new folder name", name); form->addRow("Proton directory", runner);
    auto installerLanguage = new QComboBox(&dialog); installerLanguage->setObjectName("addGameInstallerLanguage");
    installerLanguage->addItem("System default", QString()); installerLanguage->addItem("Japanese text (legacy installer)", "ja_JP.UTF-8");
    installerLanguage->setToolTip("Use Japanese locale for old non-Unicode installers. This locale also stays selected for the installed game; change it later in Manage → Runtime.");
    form->addRow("Installer text", installerLanguage);
    auto manageGe = new QPushButton("Open Proton Manager in Settings…", &dialog); form->addRow(manageGe);
    connect(manageGe, &QPushButton::clicked, &dialog, [&, runner] {
        settings();
        for (const auto &path : installedRunners(library_->root()))
            if (runner->findText(path) < 0) runner->addItem(path);
    });
    auto destination = new QLabel(&dialog); destination->setObjectName("installerDestination"); destination->setTextFormat(Qt::PlainText);
    destination->setWordWrap(true); destination->setTextInteractionFlags(Qt::TextSelectableByMouse); form->addRow(destination);
    auto updateDestination = [=, this] { destination->setText("In the installer, choose N:\\ as the destination. It maps to:\n" +
        library_->root() + '/' + (name->text().trimmed().isEmpty() ? "<game name>" : name->text().trimmed()) +
        "\n\nThe installer runs with umu / Proton in this game's own prefix. After it closes, Novelty scans the folder. If it offers to launch the game, leave that unchecked."); };
    connect(name, &QLineEdit::textChanged, &dialog, updateDestination); updateDestination();
    tabs->addTab(installerTab, "Run installer");
    auto error = new QLabel(&dialog); error->setObjectName("addGameError"); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    auto submit = buttons->button(QDialogButtonBox::Ok); submit->setObjectName("addGameSubmit"); submit->setMinimumWidth(130); submit->setText("Move to Games");
    connect(tabs, &QTabWidget::currentChanged, &dialog, [=](int index) { submit->setText(index == 0 ? "Move to Games" : "Run installer"); error->clear(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QString addedId, importWarning; bool installing = false, movedFolder = false;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            installing = tabs->currentIndex() == 1;
            if (installing) addedId = library_->createInstallation(name->text(), installer->text(), runner->currentText(), installerLanguage->currentData().toString()).id;
            else {
                const auto chosen = folder->text();
                if (chosen.trimmed().isEmpty() || !QFileInfo(chosen).isDir()) throw std::runtime_error("Choose an existing game folder.");
                const auto root = library_->root();
                const auto nativeLauncher = nativeMode->isChecked() ? nativePath->text().trimmed() : QString();
                if (nativeMode->isChecked() && nativeLauncher.isEmpty())
                    throw std::runtime_error("Choose the Linux executable inside this game folder.");
                auto transfer = std::async(std::launch::async, [root, chosen, nativeLauncher] {
                    Library worker(root); return worker.addFolder(chosen, nativeLauncher);
                });
                importing_ = true;
                RemovalProgress progress(&dialog); progress.setWindowTitle("Moving game into Games");
                auto progressLayout = new QVBoxLayout(&progress);
                auto progressText = new QLabel("Moving game into Games and detecting it…", &progress);
                progressLayout->addWidget(progressText); progress.setWindowModality(Qt::ApplicationModal);
                QEventLoop wait; QTimer poll;
                connect(&poll, &QTimer::timeout, &wait, [&] {
                    if (transfer.wait_for(std::chrono::seconds(0)) == std::future_status::ready) wait.quit();
                });
                poll.start(40); progress.show(); wait.exec(); progress.close(); importing_ = false;
                const auto result = transfer.get(); addedId = result.game.id; importWarning = result.warning; movedFolder = result.moved;
            }
            dialog.accept();
        } catch (const std::exception &e) { error->setText(QString::fromUtf8(e.what())); }
    });
    if (dialog.exec() != QDialog::Accepted) return;
    search_->clear(); refresh();
    for (int row = 0; row < model_->games.size(); ++row) if (model_->games[row].id == addedId)
        table_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, 0)));
    if (installing) launch(false, {}, {}, true);
    else {
        message(importWarning.isEmpty() ? (movedFolder ? "Game folder moved into Games and scanned." : "Game folder scanned in Games.") : importWarning);
        queueAutomaticMetadata(addedId);
        scanDelay_.start();
        if (library_->game(addedId).executable.isEmpty()) chooseExecutable(addedId);
    }
}
void Window::queueAutomaticMetadata(const QString &id) {
    if (!rootAvailable() || id.isEmpty() || autoMetadataQueued_.contains(id)) return;
    try {
        const auto g = library_->game(id);
        if (!g.metadata.isEmpty() || g.name.trimmed().isEmpty() || g.installPending) return;
        autoMetadataQueued_.insert(id); autoMetadataQueue_.append(id);
        startNextAutomaticMetadata();
    } catch (const std::exception &) { /* A removed game needs no metadata. */ }
}
void Window::startNextAutomaticMetadata() {
    if (autoMetadataActive_ || !rootAvailable()) return;
    while (!autoMetadataQueue_.isEmpty()) {
        const auto id = autoMetadataQueue_.takeFirst();
        if (!autoMetadataQueued_.contains(id)) continue;
        Game game;
        try { game = library_->game(id); }
        catch (const std::exception &) { autoMetadataQueued_.remove(id); continue; }
        if (!game.metadata.isEmpty() || game.installPending) { autoMetadataQueued_.remove(id); continue; }
        autoMetadataActive_ = true;
        const int generation = autoMetadataGeneration_;
        const auto finish = [this, generation, id] {
            if (generation != autoMetadataGeneration_) return;
            autoMetadataQueued_.remove(id); autoMetadataActive_ = false;
            QTimer::singleShot(250, this, [this] { startNextAutomaticMetadata(); });
        };
        autoMetadataClient_->search(game.name, [this, generation, id, name = game.name, finish](const QJsonArray &hits, const QString &error) {
            if (generation != autoMetadataGeneration_) return;
            const auto match = error.isEmpty() ? bestVndbMatch(name, hits) : QString();
            if (match.isEmpty()) { finish(); return; }
            autoMetadataClient_->fetch(match, [this, generation, id, finish](const QJsonObject &data, const QByteArray &image, const QString &) {
                if (generation != autoMetadataGeneration_) return;
                if (!data.isEmpty() && rootAvailable()) try {
                    // A manual link or correction made while the request was in flight wins.
                    if (autoMetadataQueued_.contains(id) && library_->game(id).metadata.isEmpty()) {
                        library_->setMetadata(id, data);
                        if (!image.isEmpty()) {
                            const auto path = metadataCoverPath(library_->dataRoot(), id);
                            if (saveMetadataCover(path, image)) invalidateCover(path);
                        }
                        refresh();
                    }
                } catch (const std::exception &) { /* Network and storage failures leave Play usable. */ }
                finish();
            });
        });
        return;
    }
}
bool Window::chooseExecutable(const QString &id) {
    if (!library_ || id.isEmpty()) return false;
    try {
        const auto g = library_->game(id);
        if (!g.executable.isEmpty() || library_->busy(id)) return false;
        const auto candidates = inspect(g.folder, library_->root()).executables;
        QDialog dialog(this); dialog.setObjectName("selectExecutableDialog");
        dialog.setWindowTitle("Choose game executable — " + g.name); dialog.resize(610, 380);
        auto layout = new QVBoxLayout(&dialog);
        auto note = new QLabel(candidates.isEmpty() ?
            "No game executable was detected. Browse for a Windows .exe or native Linux executable inside this folder, or choose Later." :
            "Choose the game to launch. Setup and uninstall helpers are left out of this list. You can change this later in Manage.", &dialog);
        note->setWordWrap(true); layout->addWidget(note);
        auto choices = new QListWidget(&dialog); choices->setObjectName("executableCandidates");
        for (const auto &candidate : candidates) {
            auto item = new QListWidgetItem(QDir(g.folder).relativeFilePath(candidate), choices);
            item->setData(Qt::UserRole, candidate);
        }
        if (candidates.isEmpty()) choices->hide(); else layout->addWidget(choices);
        auto selected = new QLineEdit(&dialog); selected->setObjectName("selectedExecutable");
        selected->setPlaceholderText("Game executable inside " + g.folder);
        auto form = new QFormLayout; form->addRow("Executable", pathField(selected, &dialog, false, "Choose game executable"));
        layout->addLayout(form);
        connect(choices, &QListWidget::currentItemChanged, &dialog, [selected](QListWidgetItem *item) {
            if (item) selected->setText(item->data(Qt::UserRole).toString());
        });
        auto error = new QLabel(&dialog); error->setObjectName("executableChoiceError");
        error->setTextFormat(Qt::PlainText); error->setWordWrap(true); layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto use = buttons->addButton("Use executable", QDialogButtonBox::AcceptRole);
        use->setObjectName("useExecutable"); layout->addWidget(buttons);
        buttons->button(QDialogButtonBox::Cancel)->setText("Later");
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(use, &QPushButton::clicked, &dialog, [&] {
            try { library_->selectExecutable(id, selected->text().trimmed()); dialog.accept(); }
            catch (const std::exception &e) { error->setText(QString::fromUtf8(e.what())); }
        });
        if (dialog.exec() == QDialog::Accepted) {
            refresh(); message("Game executable selected. Play is available with the saved settings.");
            return true;
        }
        message("Choose an executable when you're ready to play.");
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    return false;
}
} // namespace novelty
