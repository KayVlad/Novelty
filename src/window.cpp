#include "window.h"
#include "provisioning.h"
#include "profiles.h"
#include "metadata.h"
#include "saves.h"
#include "steam_shortcut.h"
#include "devices.h"
#include "lan_connection.h"
#include "lan_saves.h"
#include "lan_sync.h"
#include "lan_games.h"
#include "sync_queue.h"
#include "runtime_migration.h"
#include "pairing.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qsettings.h>
#include <QtCore/qsavefile.h>
#include <QtCore/quuid.h>
#include <QtGui/qdesktopservices.h>
#include <QtGui/qfontmetrics.h>
#include <QtGui/qaction.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qdialogbuttonbox.h>
#include <QtWidgets/qfiledialog.h>
#include <QtWidgets/qformlayout.h>
#include <QtWidgets/qgridlayout.h>
#include <QtWidgets/qheaderview.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qmenu.h>
#include <QtWidgets/qplaintextedit.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qtabwidget.h>
#include <QtWidgets/qtableview.h>
#include <QtWidgets/qstackedwidget.h>
#include <QtWidgets/qscrollarea.h>
#include <QtWidgets/qtoolbutton.h>
#include <QtWidgets/qbuttongroup.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qlistwidget.h>
#include <QtGui/qvalidator.h>
#include <QtNetwork/qhostaddress.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <stdexcept>
#include <optional>
#include <algorithm>

namespace novelty {
namespace {
class RemovalProgress : public QDialog {
public:
    using QDialog::QDialog;
    void reject() override {} // Permanent cleanup cannot be cancelled after confirmation.
};
QWidget *pathField(QLineEdit *edit, QWidget *parent, bool directory, const QString &caption) {
    auto box = new QWidget(parent); auto row = new QHBoxLayout(box); row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(edit); auto browse = new QPushButton("Browse…", box); row->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, box, [edit, box, directory, caption] {
        auto path = directory ? QFileDialog::getExistingDirectory(box, caption, edit->text())
                              : QFileDialog::getOpenFileName(box, caption, edit->text());
        if (!path.isEmpty()) edit->setText(path);
    }); return box;
}
QComboBox *backendBox(QWidget *parent, const QString &current) {
    auto box = new QComboBox(parent); box->addItem("umu — standalone Proton", "umu");
    box->addItem("Steam — existing shortcut", "steam");
    box->setCurrentIndex(current == "steam" ? 1 : 0); return box;
}
QComboBox *runnerBox(QWidget *parent, const QString &current, const QString &libraryRoot) {
    auto box = new QComboBox(parent); box->setEditable(true); box->setMinimumContentsLength(30);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (auto path : installedRunners(libraryRoot)) box->addItem(path);
    box->setCurrentText(current); return box;
}
QStringList metadataStrings(const QJsonValue &value) {
    QStringList result; for (const auto &item : value.toArray()) if (!item.toString().isEmpty()) result.append(item.toString());
    return result;
}
QStringList localLanAddresses() {
    QStringList addresses;
    ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) return addresses;
    for (auto entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {};
        auto address = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
            QString::fromLatin1(text) != "127.0.0.1") addresses.append(QString::fromLatin1(text));
    }
    ::freeifaddrs(interfaces);
    addresses.removeDuplicates();
    return addresses;
}
}
QVariant GameModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() < 0 || i.row() >= games.size() || i.column() < 0 || i.column() >= 3) return {};
    const auto &g = games[i.row()];
    if (role == Qt::DisplayRole) {
        switch (i.column()) { case 0: return g.name; case 1: return g.engine; default: return states.value(i.row()); }
    }
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role == EngineRole) return g.engine;
    if (role == StatusRole) return states.value(i.row());
    if (role == LaunchableRole) return actionsAllowed && launchable.value(i.row());
    if (role == ManageableRole) return actionsAllowed && manageable.value(i.row());
    if (role == CoverRole) return games[i.row()].metadata.isEmpty() ? QString() :
        metadataCoverPath(coverDataRoot, games[i.row()].id);
    if (role == Qt::ToolTipRole) return i.column() == 1 ? g.evidence : g.folder;
    return {};
}
QVariant GameModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o == Qt::Horizontal && role == Qt::DisplayRole) return QStringList{"Game", "Engine", "Status"}.value(section);
    return {};
}
void GameModel::load(const Library &lib) {
    beginResetModel(); games = lib.games(); coverDataRoot = lib.dataRoot(); states.clear(); launchable.clear(); manageable.clear();
    for (const auto &g : games) {
        states.append(lib.status(g));
        const bool available = !lib.busy(g.id);
        manageable.append(available);
        launchable.append(available && !g.installPending && QFileInfo(g.folder).isDir() &&
                          (g.executable.isEmpty() || QFileInfo(g.executable).isFile()));
    }
    endResetModel();
}
void GameModel::unavailable() {
    beginResetModel(); states.clear(); launchable.clear(); manageable.clear();
    for (qsizetype row = 0; row < games.size(); ++row) states.append("Unavailable");
    endResetModel();
}
bool GameModel::updateState(const Library &lib, const QString &id) {
    for (int row = 0; row < games.size(); ++row) if (games[row].id == id) {
        const auto state = lib.status(games[row]);
        const bool canManage = !lib.busy(id);
        const bool canLaunch = canManage && !games[row].installPending && QFileInfo(games[row].folder).isDir() &&
                               (games[row].executable.isEmpty() || QFileInfo(games[row].executable).isFile());
        if (states.value(row) == state && manageable.value(row) == canManage && launchable.value(row) == canLaunch) return false;
        states[row] = state; manageable[row] = canManage; launchable[row] = canLaunch;
        emit dataChanged(index(row, 0), index(row, 2), {Qt::DisplayRole, StatusRole, LaunchableRole, ManageableRole});
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
    listManage_->setShortcut(QKeySequence("Ctrl+M")); listManage_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    table_->addAction(listPlay_); table_->addAction(listManage_); table_->setContextMenuPolicy(Qt::ActionsContextMenu);
    table_->setToolTip("Double-click to play. Right-click for Play and Manage, or press Ctrl+M to manage the selected game.");
    connect(listPlay_, &QAction::triggered, this, [this] { launch(); }); connect(listManage_, &QAction::triggered, this, [this] { manage(); });
    grid_ = new GameGrid(this); grid_->setModel(proxy_); grid_->setSelectionModel(table_->selectionModel());
    grid_->contextAction = [this](const QModelIndex &index, const QPoint &position) {
        table_->setCurrentIndex(index);
        const auto id = selected().id;
        auto menu = new QMenu(this); menu->setObjectName("cardMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
        auto manageAction = menu->addAction(uiIcon("manage"), "Manage…"); manageAction->setObjectName("cardManageAction");
        auto removeAction = menu->addAction(uiIcon("trash"), "Delete…"); removeAction->setObjectName("cardDeleteAction");
        connect(manageAction, &QAction::triggered, this, [this] { manage(); });
        connect(removeAction, &QAction::triggered, this, [this, id] { removeGame(id, nullptr); });
        menu->popup(position);
    };
    views_ = new QStackedWidget(this); views_->addWidget(grid_); views_->addWidget(table_); body->addWidget(views_, 1);
    const bool useList = QSettings().value("libraryView", "grid").toString() == "list";
    views_->setCurrentIndex(useList ? 1 : 0); (useList ? listButton : gridButton)->setChecked(true);
    connect(gridButton, &QToolButton::clicked, this, [this] {
        // A table click may make Engine/Status the current column; cards use the title column.
        const auto index = table_->currentIndex();
        if (index.isValid()) table_->setCurrentIndex(index.siblingAtColumn(0));
        views_->setCurrentIndex(0); QSettings().setValue("libraryView", "grid");
    });
    connect(listButton, &QToolButton::clicked, this, [this] { views_->setCurrentIndex(1); QSettings().setValue("libraryView", "list"); });
    auto panel = new QWidget(this); panel->setObjectName("detailPanel"); panel->setFixedWidth(340);
    auto p = new QVBoxLayout(panel); p->setContentsMargins(16, 16, 16, 16); p->setSpacing(12);
    auto scroll = new QScrollArea(panel); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setObjectName("detailViewport");
    auto content = new QWidget(scroll); content->setObjectName("detailContent"); auto info = new QVBoxLayout(content); info->setContentsMargins(0, 0, 4, 0); info->setSpacing(10);
    cover_ = new Cover(content); info->addWidget(cover_);
    title_ = new QLabel("Your next story", content); title_->setObjectName("gameTitle"); title_->setWordWrap(true); title_->setTextFormat(Qt::PlainText); info->addWidget(title_);
    originalTitle_ = new QLabel(content); originalTitle_->setObjectName("gameOriginalTitle"); originalTitle_->setWordWrap(true);
    originalTitle_->setTextFormat(Qt::PlainText); originalTitle_->setProperty("muted", true); originalTitle_->hide(); info->addWidget(originalTitle_);
    status_ = new QLabel(content); status_->setObjectName("gameStatus"); info->addWidget(status_);
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
        QLabel#brand { font-size: 27px; font-weight: bold; }
        QLabel#gameTitle { font-size: 25px; font-weight: bold; }
        QWidget#detailPanel { background: #122237; border: 1px solid #263b55; border-radius: 9px; }
        QWidget#detailContent, QWidget#detailViewport, QScrollArea { background: #122237; }
        QLabel#metadataTags { background: #21344d; border-radius: 4px; padding: 4px 8px; color: #b7c8dc; font-size: 12px; }
        QFrame#separator { color: #30425b; }
        QLineEdit, QPlainTextEdit, QComboBox { background: #15263b; border: 1px solid #2e425f; border-radius: 6px; padding: 11px; selection-background-color: #315f87; }
        QPushButton, QToolButton { background: #1b2c43; border: 1px solid #354b69; border-radius: 6px; padding: 10px 14px; min-height: 22px; }
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
    connect(grid_, &QListView::doubleClicked, this, [this] { launch(); });
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
                launchPoll_.stop(); pendingLaunch_.clear(); refresh();
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
                launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false;
                refresh(); message(recovery.isEmpty() ? j["message"].toString(state) : recovery); scanDelay_.start();
                if (completedInstaller && recovery.isEmpty() && (state == "Ready" || state == "Installation pending") &&
                    library_->game(completedId).executable.isEmpty()) chooseExecutable(completedId);
                if (completedInstaller && recovery.isEmpty()) queueAutomaticMetadata(completedId);
            } else if (!active && QDateTime::currentMSecsSinceEpoch() - launchStarted_ > 10000) {
                launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false;
                message("The launch supervisor did not start. Check the command-line output."); selectGame();
            }
        } catch (const std::exception &e) { launchPoll_.stop(); pendingLaunch_.clear(); pendingInstaller_ = false; message(QString::fromUtf8(e.what())); }
    });
    syncTimer_.setInterval(30000);
    connect(&syncTimer_, &QTimer::timeout, this, [this] { syncPendingPeers(); selectGame(); });
    selectGame();
    const auto initial = root.isEmpty() ? QSettings().value("gamesRoot").toString() : root;
    if (!initial.isEmpty()) {
        rootPath_ = canonicalPath(initial); updateRootLink();
        const auto remembered = QSettings().value("gamesRoot").toString();
        rootRequiresDatabase_ = !remembered.isEmpty() && canonicalPath(remembered) == rootPath_;
        try { openRoot(initial, !rootRequiresDatabase_); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
    }
}
Window::~Window() {
    syncTimer_.stop();
    for (auto connection : autoConnections_) { connection->close(); delete connection; }
    autoConnections_.clear();
    activePeerIds_.clear();
    saveListener_.reset();
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
    auto saveRecovery = recoverSaveRestores(*next);
    saveRecovery.append(recoverSaveObservations(*next));
    saveRecovery.append(recoverPendingSaveSnapshots(*next));
    for (auto connection : autoConnections_) { connection->close(); delete connection; }
    autoConnections_.clear();
    activePeerIds_.clear();
    saveListener_.reset();
    library_ = std::move(next); rootPath_ = library_->root(); copyTracker_.clear(); scanDelay_.stop();
    lanListenIssue_.clear();
    saveListener_ = std::make_unique<LanListener>(*library_, false);
    saveListener_->onConnection = [this](LanConnection *connection) {
        connection->onReady = [this, connection](const QString &) {
            auto exchange = new LanSaveExchange(*library_, *connection, connection);
            new LanGameExchange(*library_, *connection, *exchange, connection);
            exchange->onOffer = [this, connection, exchange](const QJsonObject &offer) {
                const auto remoteId = offer["profile_game_id"].toString();
                const auto offerId = offer["request_id"].toString();
                const auto snapshotId = offer["snapshot_id"].toString();
                QString localId;
                try {
                    for (const auto &game : library_->games()) {
                        const auto config = loadSaveConfig(*library_, game.id);
                        if (config.gameIdentity != offer["game_identity"].toString() ||
                            config.releaseIdentity != offer["release_identity"].toString() ||
                            config.saveCompatibility != offer["save_compatibility"].toString() ||
                            config.mappings.isEmpty()) continue;
                        bool linked = config.profileGameId == remoteId;
                        for (const auto &link : config.peerLinks)
                            if (link.fingerprint == connection->peerFingerprint() &&
                                link.profileGameId == remoteId) linked = true;
                        if (linked) { if (!localId.isEmpty()) { localId.clear(); break; } localId = game.id; }
                    }
                    if (localId.isEmpty()) {
                        exchange->deferOffer(offerId, "Confirm the matching game in Manage → Saves first."); return;
                    }
                    if (library_->busy(localId)) {
                        exchange->deferOffer(offerId, "The matching game is currently busy."); return;
                    }
                    const auto gameId = localId;
                    exchange->onImported = [this, exchange, offerId, snapshotId, gameId](const SaveImportResult &result) {
                        try {
                            exchange->acknowledgeOffer(offerId, snapshotId);
                            selectGame();
                            if (result.relation == "conflict")
                                message("Peer saves for " + library_->game(gameId).name +
                                        " conflict with local progress. Both copies are in Manage → Saves.");
                            else if (result.relation == "ahead" || result.relation == "available")
                                message("New peer saves for " + library_->game(gameId).name +
                                        " are verified and available before the next Play.");
                        } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
                    };
                    exchange->fetch(gameId, remoteId, snapshotId);
                } catch (const std::exception &e) {
                    try { exchange->deferOffer(offerId, QString::fromUtf8(e.what())); }
                    catch (...) { connection->close(); }
                }
            };
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
    QTimer::singleShot(1000, this, [this] { syncPendingPeers(); });
    ++autoMetadataGeneration_; autoMetadataQueue_.clear(); autoMetadataQueued_.clear(); autoMetadataActive_ = false;
    rootRequiresDatabase_ = true; ++scanGeneration_;
    QSettings().setValue("gamesRoot", library_->root());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    watcher_.addPath(rootPath_);
    for (auto g : library_->games()) if (library_->supervisorActive(g.id)) {
        pendingLaunch_ = g.id; launchStarted_ = QDateTime::currentMSecsSinceEpoch(); launchPoll_.start(); break;
    }
    updateRootLink(); refresh();
    if (!saveRecovery.isEmpty()) message(saveRecovery.join("\n"));
    QTimer::singleShot(0, this, [this] { scan(); });
}
void Window::syncPendingPeers() {
    if (!rootAvailable() || !saveListener_ || autoConnections_.size() >= 2) return;
    try {
        const auto tasks = pendingPeerSync(*library_);
        const auto peers = trustedPeers(*library_);
        QSet<QString> attempted;
        syncQueueIssue_.clear();
        for (const auto &task : tasks) {
            if (autoConnections_.size() >= 2) break;
            if (activePeerIds_.contains(task.fingerprint) || attempted.contains(task.fingerprint)) continue;
            attempted.insert(task.fingerprint);
            TrustedPeer peer;
            for (const auto &candidate : peers)
                if (candidate.fingerprint == task.fingerprint) { peer = candidate; break; }
            if (peer.address.isEmpty() || peer.port == 0) continue;
            try {
                if (appliedSaveSnapshotId(*library_, task.gameId) != task.snapshotId) {
                    const auto current = appliedSaveSnapshotId(*library_, task.gameId);
                    if (current.isEmpty()) acknowledgePeerSync(*library_, task.fingerprint, task.gameId, task.snapshotId);
                    else queuePeerSync(*library_, task.gameId, current);
                    continue;
                }
            } catch (const std::exception &) {
                acknowledgePeerSync(*library_, task.fingerprint, task.gameId, task.snapshotId);
                continue;
            }
            auto connection = new LanConnection(*library_, false, this);
            autoConnections_.insert(connection); activePeerIds_.insert(task.fingerprint);
            const auto finish = [this, connection, fingerprint = task.fingerprint](bool nextNow = false) {
                if (!autoConnections_.contains(connection)) return;
                autoConnections_.remove(connection); activePeerIds_.remove(fingerprint);
                connection->close(); connection->deleteLater();
                if (nextNow) QTimer::singleShot(0, this, [this] { syncPendingPeers(); });
            };
            connection->onError = [finish](const QString &) { finish(false); };
            connection->onReady = [this, connection, task, finish](const QString &fingerprint) {
                if (fingerprint != task.fingerprint) { finish(); return; }
                try {
                    auto exchange = new LanSaveExchange(*library_, *connection, connection);
                    exchange->onAcknowledged = [this, task, finish](const QString &gameId, const QString &snapshotId) {
                        try { acknowledgePeerSync(*library_, task.fingerprint, gameId, snapshotId); }
                        catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
                        selectGame();
                        finish(true);
                    };
                    exchange->onDeferred = [finish](const QString &) { finish(false); };
                    exchange->onError = [finish](const QString &) { finish(false); };
                    exchange->offer(task.gameId, task.snapshotId);
                    QTimer::singleShot(60000, connection, [finish] { finish(false); });
                } catch (const std::exception &) { finish(false); }
            };
            connection->connectTo(peer.address, peer.port);
        }
    } catch (const std::exception &e) {
        const auto issue = QString::fromUtf8(e.what());
        if (issue != syncQueueIssue_) { syncQueueIssue_ = issue; message("Peer sync is pending: " + issue); }
    }
}
void Window::chooseRoot() {
    if (!pendingLaunch_.isEmpty()) { message("Wait until this launch finishes before switching libraries."); return; }
    const auto path = QFileDialog::getExistingDirectory(this, "Choose your main Games folder", library_ ? library_->root() : QDir::homePath());
    if (path.isEmpty()) return;
    try { openRoot(path); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
void Window::refresh() {
    const auto id = selected().id;
    if (rootAvailable()) model_->load(*library_);
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
    model_->actionsAllowed = accessible && pendingLaunch_.isEmpty() && !removing_; grid_->viewport()->update();
    const auto index = table_->currentIndex();
    listPlay_->setEnabled(index.data(LaunchableRole).toBool()); listManage_->setEnabled(index.data(ManageableRole).toBool());
    GameMetadata metadata;
    if (valid && !g.metadata.isEmpty()) {
        metadata.cover = cachedCover(metadataCoverPath(library_->dataRoot(), g.id));
        metadata.description = g.metadata["description"].toString();
        metadata.tags = metadataStrings(g.metadata["tags"]).mid(0, 12);
        metadata.playtime = g.metadata["expected_play_time"].toString();
    }
    cover_->setGame(valid ? g.name : QString(), metadata);
    description_->setText(metadata.description.isEmpty() ? "No description yet." :
        (metadata.description.size() > 500 ? metadata.description.left(500).trimmed() + "…" : metadata.description));
    playtimeValue_->setText(metadata.playtime.value_or("—")); tags_->setText(metadata.tags.isEmpty() ? "No tags yet" : metadata.tags.join(" · "));
    if (valid) {
        title_->setText(g.name); const auto state = accessible ? library_->status(g) : "Unavailable";
        originalTitle_->setText(g.metadata["original_title"].toString());
        originalTitle_->setVisible(!originalTitle_->text().isEmpty());
        status_->setText("●  " + state); status_->setStyleSheet("color: " + statusColor(state).name() + "; font-size: 13px;");
        syncState_->clear();
        if (accessible) try {
            int pending = 0; bool ahead = false, conflict = false;
            for (const auto &task : pendingPeerSync(*library_)) if (task.gameId == g.id) ++pending;
            for (const auto &candidate : importedSaveCandidates(*library_, g.id)) {
                if (candidate.relation == "conflict") conflict = true;
                if (candidate.relation == "ahead" || candidate.relation == "available") ahead = true;
            }
            QStringList parts;
            if (conflict) parts.append("Save conflict: choose a copy in Manage → Saves.");
            else if (ahead) parts.append("New peer save ready for the next Play.");
            if (pending) parts.append(QString::number(pending) + " peer save transfer" + (pending == 1 ? "" : "s") + " pending.");
            syncState_->setText(parts.join(" "));
        } catch (const std::exception &e) { syncState_->setText("Peer save status needs review: " + QString::fromUtf8(e.what())); }
        syncState_->setVisible(!syncState_->text().isEmpty());
        detail_->setText(!accessible ? "Reconnect your Games folder and press Rescan." : launchPathIssue(g));
        if (accessible && state == "Needs setup")
            detail_->setText("Play will verify setup in the existing prefix before starting the game. You can also use Prepare in Manage → Runtime.");
        if (accessible && state == "Recovery required") detail_->setText(library_->recoveryIssue(g.id));
        detail_->setVisible(!detail_->text().isEmpty());
    } else {
        title_->setText("Your next story"); status_->setText("No game selected"); status_->setStyleSheet("color: #9eafc5; font-size: 13px;");
        syncState_->clear(); syncState_->hide();
        originalTitle_->hide();
        detail_->setText(!search_->text().isEmpty() ? "No games match your search." : "Choose a Games folder in Settings, or use Add game to select a folder or run an installer."); detail_->show();
    }
}
void Window::scan() {
    if (removing_ || importing_ || pendingScan_.valid() || !pendingLaunch_.isEmpty()) return;
    if (!library_) {
        if (!rootPath_.isEmpty()) try { openRoot(rootPath_, !rootRequiresDatabase_); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
        return;
    }
    if (!rootAvailable()) { copyTracker_.clear(); refresh(); message("Games library unavailable. Reconnect it and press Rescan."); return; }
    scanRoot_ = library_->root(); pendingScanGeneration_ = scanGeneration_; auto root = scanRoot_;
    QStringList known; for (const auto &g : model_->games) known.append(g.folder);
    pendingScan_ = std::async(std::launch::async, [root, known] {
        Library worker(root); auto recovered = worker.recoverRemovals(); recovered.append(worker.recoverFolderImports());
        recovered.append(worker.recoverInstallations());
        recovered.append(recoverSaveRestores(worker)); recovered.append(recoverSaveObservations(worker));
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
    auto getGe = new QPushButton("Get GE-Proton…", &dialog); form->addRow(getGe);
    connect(getGe, &QPushButton::clicked, &dialog, [&, runner] {
        const auto installed = provisionGe(&dialog, library_->root());
        if (!installed.isEmpty()) { runner->addItem(installed); runner->setCurrentText(installed); }
    });
    auto destination = new QLabel(&dialog); destination->setObjectName("installerDestination"); destination->setTextFormat(Qt::PlainText);
    destination->setWordWrap(true); destination->setTextInteractionFlags(Qt::TextSelectableByMouse); form->addRow(destination);
    auto updateDestination = [=, this] { destination->setText("In the installer, choose N:\\ as the destination. It maps to:\n" +
        library_->root() + '/' + (name->text().trimmed().isEmpty() ? "<game name>" : name->text().trimmed()) +
        "\n\nThe installer runs with umu / Proton in this game's own prefix. After it closes, Novelty scans the folder. If it offers to launch the game, leave that unchecked."); };
    connect(name, &QLineEdit::textChanged, &dialog, updateDestination); updateDestination();
    tabs->addTab(installerTab, "Run installer");
    auto fromPeer = new QPushButton("Install from paired device…", &dialog);
    fromPeer->setObjectName("installFromPeer"); layout->addWidget(fromPeer, 0, Qt::AlignLeft);
    connect(fromPeer, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.reject(); QTimer::singleShot(0, this, [this] { installFromPeer(); });
    });
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
                auto transfer = std::async(std::launch::async, [root, chosen] { Library worker(root); return worker.addFolder(chosen); });
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
        const auto candidates = inspect(g.folder).executables;
        QDialog dialog(this); dialog.setObjectName("selectExecutableDialog");
        dialog.setWindowTitle("Choose game executable — " + g.name); dialog.resize(610, 380);
        auto layout = new QVBoxLayout(&dialog);
        auto note = new QLabel(candidates.isEmpty() ?
            "No game executable was detected. Browse for an .exe inside this game folder, or choose Later." :
            "Choose the game to launch. Setup and uninstall helpers are left out of this list. You can change this later in Manage.", &dialog);
        note->setWordWrap(true); layout->addWidget(note);
        auto choices = new QListWidget(&dialog); choices->setObjectName("executableCandidates");
        for (const auto &candidate : candidates) {
            auto item = new QListWidgetItem(QDir(g.folder).relativeFilePath(candidate), choices);
            item->setData(Qt::UserRole, candidate);
        }
        if (candidates.isEmpty()) choices->hide(); else layout->addWidget(choices);
        auto selected = new QLineEdit(&dialog); selected->setObjectName("selectedExecutable");
        selected->setPlaceholderText("Game .exe inside " + g.folder);
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
void Window::settings() {
    QDialog dialog(this); dialog.setObjectName("settingsDialog"); dialog.setWindowTitle("Settings"); dialog.resize(750, 420);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout;
    auto folderField = new QLineEdit(rootPath_, &dialog); folderField->setObjectName("settingsGamesFolder");
    form->addRow("Games folder", pathField(folderField, &dialog, true, "Choose your main Games folder"));
    auto s = library_ ? library_->settings() : QJsonObject(); auto backend = backendBox(&dialog, s["backend"].toString("umu"));
    auto runner = runnerBox(&dialog, s["runner"].toString(), library_ ? library_->root() : QString());
    auto umu = new QLineEdit(s["umu"].toString("umu-run"), &dialog);
    auto steam = new QLineEdit(s["steam"].toString("steam"), &dialog);
    form->addRow("Default backend", backend); form->addRow("Default Proton directory", runner);
    auto getGe = new QPushButton("Get GE-Proton…", &dialog); getGe->setObjectName("getGeSettings"); form->addRow(getGe);
    connect(getGe, &QPushButton::clicked, &dialog, [&] {
        const auto installed = provisionGe(&dialog, folderField->text());
        if (!installed.isEmpty()) { if (runner->findText(installed) < 0) runner->addItem(installed); runner->setCurrentText(installed); }
    });
    form->addRow("umu executable", pathField(umu, &dialog, false, "Select umu-run"));
    auto getUmu = new QPushButton("Get umu…", &dialog); getUmu->setObjectName("getUmuSettings"); form->addRow(getUmu);
    connect(getUmu, &QPushButton::clicked, &dialog, [&] {
        const auto installed = provisionUmu(&dialog, folderField->text());
        if (!installed.isEmpty()) umu->setText(installed);
    });
    form->addRow("Steam executable", pathField(steam, &dialog, false, "Select Steam")); layout->addLayout(form);
    if (library_ && !library_->managedRuntimeStorage()) {
        auto migrate = new QPushButton("Migrate legacy umu runtime into Games…", &dialog);
        migrate->setObjectName("migrateLegacyRuntime"); layout->addWidget(migrate);
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
    layout->addWidget(devicesButton);
    auto note = new QLabel("Default backend and Proton apply to newly discovered games. The umu executable applies to all umu games in this library.\n\nGet GE-Proton and Get umu install pinned versions for this library. Manage → Add to Steam creates a shortcut to Novelty's existing launch path.", &dialog); note->setWordWrap(true); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    try {
        const auto folder = folderField->text().trimmed();
        if (folder.isEmpty()) throw std::runtime_error("Choose a Games folder.");
        if (!library_ || canonicalPath(folder) != library_->root()) {
            if (!pendingLaunch_.isEmpty()) throw std::runtime_error("Wait until this launch finishes before switching libraries.");
            openRoot(folder);
        }
        s["backend"] = backend->currentData().toString(); s["runner"] = runner->currentText();
        s["umu"] = umu->text(); s["steam"] = steam->text(); library_->setSettings(s); refresh();
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
void Window::devices() {
    if (!library_ || !rootAvailable()) { message("Choose an available Games folder first."); return; }
    try { ensureDeviceIdentity(*library_); }
    catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    QDialog dialog(this); dialog.setObjectName("devicesDialog"); dialog.setWindowTitle("Devices"); dialog.resize(560, 420);
    auto layout = new QVBoxLayout(&dialog);
    auto own = new QLabel("This device: " + ensureDeviceIdentity(*library_).fingerprint, &dialog);
    own->setWordWrap(true); own->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(own);
    auto syncAddress = new QLabel(saveListener_ ?
        "For save sync, use this device's LAN IP with port " + QString::number(saveListener_->serverPort()) :
        lanListenIssue_, &dialog);
    syncAddress->setWordWrap(true); syncAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(syncAddress);
    auto list = new QListWidget(&dialog); list->setObjectName("trustedDevices"); layout->addWidget(list, 1);
    auto render = [&] {
        list->clear();
        for (const auto &peer : trustedPeers(*library_)) {
            auto item = new QListWidgetItem(peer.name + " — " + peer.fingerprint.left(12) + "…" +
                (peer.address.isEmpty() ? QString() : " · " + peer.address + ':' + QString::number(peer.port)), list);
            item->setData(Qt::UserRole, peer.fingerprint);
            item->setToolTip(peer.fingerprint);
        }
    };
    try { render(); } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    auto actions = new QHBoxLayout;
    auto pairButton = new QPushButton("Pair device…", &dialog);
    auto revokeButton = new QPushButton("Revoke selected", &dialog); revokeButton->setEnabled(false);
    actions->addWidget(pairButton); actions->addWidget(revokeButton); actions->addStretch(); layout->addLayout(actions);
    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [=] { revokeButton->setEnabled(list->currentItem() != nullptr); });
    connect(revokeButton, &QPushButton::clicked, &dialog, [&, this] {
        if (!list->currentItem()) return;
        try { revokePeer(*library_, list->currentItem()->data(Qt::UserRole).toString()); render(); }
        catch (const std::exception &e) { QMessageBox::warning(&dialog, "Cannot revoke device", QString::fromUtf8(e.what())); }
    });
    connect(pairButton, &QPushButton::clicked, &dialog, [&, this] {
        QDialog pair(&dialog); pair.setObjectName("pairDeviceDialog"); pair.setWindowTitle("Pair a LAN device"); pair.resize(550, 370);
        auto column = new QVBoxLayout(&pair);
        LanListener listener(*library_, true, &pair);
        if (!listener.listen(QHostAddress::AnyIPv4, 0)) {
            QMessageBox::warning(&pair, "Cannot pair", "Cannot listen for LAN peers: " + listener.errorString()); return;
        }
        auto addresses = localLanAddresses();
        auto instructions = new QLabel("Open this screen on both devices. On one device, enter the other device's LAN address and port below. Compare the verification phrase on both screens before approving.", &pair);
        instructions->setWordWrap(true); column->addWidget(instructions);
        auto local = new QLabel("This device: " + (addresses.isEmpty() ? QString("Find its IP in network settings") : addresses.join(", ")) +
                                "  ·  port " + QString::number(listener.serverPort()), &pair);
        local->setTextInteractionFlags(Qt::TextSelectableByMouse); local->setWordWrap(true); column->addWidget(local);
        auto row = new QHBoxLayout;
        auto host = new QLineEdit(&pair); host->setPlaceholderText("Other device's LAN IP address");
        auto port = new QLineEdit(QString::number(listener.serverPort()), &pair);
        port->setValidator(new QIntValidator(1, 65535, port)); port->setMaximumWidth(90);
        auto connectButton = new QPushButton("Connect", &pair);
        row->addWidget(host, 1); row->addWidget(port); row->addWidget(connectButton); column->addLayout(row);
        auto discovered = new QListWidget(&pair); discovered->setObjectName("discoveredPairingDevices");
        discovered->setMaximumHeight(90); column->addWidget(discovered);
        auto discoveryNote = new QLabel("Nearby pairing screens appear here when UDP discovery is available. You can always enter an IP address above.", &pair);
        discoveryNote->setWordWrap(true); column->addWidget(discoveryNote);
        QTimer beacon(&pair), discoveryPoll(&pair);
        const auto ownSession = newDiscoverySessionId();
        const int discoveryFd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int enabled = 1;
        sockaddr_in discoveryAddress{};
        discoveryAddress.sin_family = AF_INET; discoveryAddress.sin_port = htons(38374);
        discoveryAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool discoveryReady = discoveryFd >= 0 &&
            ::setsockopt(discoveryFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0 &&
            ::setsockopt(discoveryFd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            ::bind(discoveryFd, reinterpret_cast<sockaddr *>(&discoveryAddress), sizeof(discoveryAddress)) == 0;
        if (discoveryReady) {
            beacon.setInterval(2000);
            auto announce = [&] {
                const auto bytes = encodeDiscoveryAnnouncement({ownSession, listener.serverPort()});
                sockaddr_in broadcast{}; broadcast.sin_family = AF_INET; broadcast.sin_port = htons(38374);
                broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                ::sendto(discoveryFd, bytes.constData(), size_t(bytes.size()), 0,
                         reinterpret_cast<sockaddr *>(&broadcast), sizeof(broadcast));
            };
            connect(&beacon, &QTimer::timeout, &pair, announce);
            discoveryPoll.setInterval(250);
            connect(&discoveryPoll, &QTimer::timeout, &pair, [&] {
                for (;;) {
                    QByteArray bytes(64, Qt::Uninitialized); sockaddr_in sender{}; socklen_t senderSize = sizeof(sender);
                    const auto size = ::recvfrom(discoveryFd, bytes.data(), size_t(bytes.size()), 0,
                                                reinterpret_cast<sockaddr *>(&sender), &senderSize);
                    if (size < 0) { if (errno == EINTR) continue; break; }
                    if (size > bytes.size() || sender.sin_family != AF_INET ||
                        (ntohl(sender.sin_addr.s_addr) >> 24) == 127) continue;
                    char addressBuffer[INET_ADDRSTRLEN];
                    if (!::inet_ntop(AF_INET, &sender.sin_addr, addressBuffer, sizeof(addressBuffer))) continue;
                    bytes.resize(size);
                    DiscoveryAnnouncement item;
                    if (!decodeDiscoveryAnnouncement(bytes, item) || item.sessionId == ownSession) continue;
                    const auto address = QString::fromLatin1(addressBuffer);
                    bool found = false;
                    for (int i = 0; i < discovered->count(); ++i)
                        if (discovered->item(i)->data(Qt::UserRole).toByteArray() == item.sessionId) {
                            discovered->item(i)->setText("Nearby: " + address + ':' + QString::number(item.port));
                            discovered->item(i)->setData(Qt::UserRole + 1, address);
                            discovered->item(i)->setData(Qt::UserRole + 2, int(item.port));
                            found = true; break;
                        }
                    if (!found && discovered->count() < 16) {
                        auto row = new QListWidgetItem("Nearby: " + address + ':' + QString::number(item.port), discovered);
                        row->setData(Qt::UserRole, item.sessionId);
                        row->setData(Qt::UserRole + 1, address); row->setData(Qt::UserRole + 2, int(item.port));
                    }
                }
            });
            connect(discovered, &QListWidget::itemSelectionChanged, &pair, [&] {
                if (const auto selected = discovered->currentItem()) {
                    host->setText(selected->data(Qt::UserRole + 1).toString());
                    port->setText(QString::number(selected->data(Qt::UserRole + 2).toInt()));
                }
            });
            discoveryPoll.start(); beacon.start(); announce();
        } else discoveryNote->setText("UDP discovery is unavailable here. Enter the other device's LAN IP address above.");
        auto phrase = new QLabel("Waiting for a device…", &pair); phrase->setObjectName("pairingPhrase");
        phrase->setTextInteractionFlags(Qt::TextSelectableByMouse); phrase->setWordWrap(true); column->addWidget(phrase);
        auto name = new QLineEdit("Other device", &pair); name->setMaxLength(80); column->addWidget(name);
        auto approve = new QPushButton("Phrases match — approve", &pair); approve->setObjectName("approvePairing");
        approve->setEnabled(false); column->addWidget(approve);
        auto state = new QLabel(&pair); state->setWordWrap(true); column->addWidget(state);
        column->addStretch();
        LanConnection *active = nullptr;
        auto attach = [&](LanConnection *connection) {
            active = connection; approve->setEnabled(false); phrase->setText("Connecting…"); state->clear();
            connection->onPhrase = [&, connection](const QString &fingerprint, const QString &value) {
                if (active != connection) return;
                phrase->setText("Compare on both devices: " + value);
                state->setText("Device certificate: " + fingerprint.left(16) + "…");
                approve->setEnabled(true);
            };
            connection->onReady = [&, connection](const QString &) {
                if (active != connection) return;
                approve->setEnabled(false); state->setText("Paired. This device is now trusted for this Games library.");
                render();
            };
            connection->onError = [&, connection](const QString &error) {
                if (active != connection) return;
                approve->setEnabled(false); state->setText(error); active = nullptr;
                connection->deleteLater();
            };
        };
        listener.onConnection = [&](LanConnection *connection) {
            if (active) return false;
            attach(connection); return true;
        };
        connect(connectButton, &QPushButton::clicked, &pair, [&, this] {
            if (active) { state->setText("Finish or close the current pairing first."); return; }
            if (QHostAddress(host->text().trimmed()).isNull()) {
                state->setText("Enter the other device's LAN IP address."); return;
            }
            auto connection = new LanConnection(*library_, true, &pair);
            attach(connection);
            bool validPort = false;
            const auto number = port->text().toUShort(&validPort);
            if (!validPort || number == 0) { state->setText("Enter a valid port."); connection->deleteLater(); active = nullptr; return; }
            connection->connectTo(host->text().trimmed(), number);
        });
        connect(approve, &QPushButton::clicked, &pair, [&] {
            if (active) { approve->setEnabled(false); active->approvePairing(name->text()); }
        });
        connect(&pair, &QDialog::finished, &pair, [&] { listener.close(); if (active) active->close(); });
        pair.exec();
        if (discoveryFd >= 0) ::close(discoveryFd);
    });
    auto close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close); dialog.exec();
}
void Window::installFromPeer() {
    if (!library_ || !rootAvailable()) { message("Choose an available Games folder first."); return; }
    QList<TrustedPeer> peers;
    try { peers = trustedPeers(*library_); }
    catch (const std::exception &e) { message(QString::fromUtf8(e.what())); return; }
    if (peers.isEmpty()) { message("Pair the other device in Settings → Devices before installing a game."); return; }
    QDialog dialog(this); dialog.setObjectName("installFromPeerDialog");
    dialog.setWindowTitle("Install from paired device"); dialog.resize(600, 440);
    auto layout = new QVBoxLayout(&dialog);
    auto note = new QLabel("Keep Novelty open on the other device. Choose a trusted device, then select a game to transfer into this Games folder.", &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto peerBox = new QComboBox(&dialog); peerBox->setObjectName("gameTransferPeer");
    for (const auto &peer : peers) peerBox->addItem(peer.name + " — " + peer.fingerprint.left(12) + "…", peer.fingerprint);
    layout->addWidget(peerBox);
    auto row = new QHBoxLayout;
    auto host = new QLineEdit(&dialog); host->setPlaceholderText("Peer LAN IP address");
    auto port = new QLineEdit(&dialog); port->setValidator(new QIntValidator(1, 65535, port)); port->setMaximumWidth(85);
    auto connectButton = new QPushButton("Show games", &dialog);
    row->addWidget(host, 1); row->addWidget(port); row->addWidget(connectButton); layout->addLayout(row);
    const auto fillEndpoint = [&] {
        const auto fingerprint = peerBox->currentData().toString();
        for (const auto &peer : peers) if (peer.fingerprint == fingerprint) {
            host->setText(peer.address); port->setText(QString::number(peer.port ? peer.port : 38373)); break;
        }
    };
    connect(peerBox, &QComboBox::currentIndexChanged, &dialog, fillEndpoint); fillEndpoint();
    auto games = new QListWidget(&dialog); games->setObjectName("peerGames"); layout->addWidget(games, 1);
    auto status = new QLabel("Connect to list the device's available games.", &dialog);
    status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto installButton = buttons->addButton("Install selected game", QDialogButtonBox::ActionRole);
    installButton->setObjectName("startPeerGameInstall"); installButton->setEnabled(false);
    layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(games, &QListWidget::itemSelectionChanged, &dialog,
            [=] { installButton->setEnabled(games->currentItem() != nullptr); });
    LanConnection *active = nullptr;
    LanGameExchange *exchange = nullptr;
    QString installedId;
    connect(connectButton, &QPushButton::clicked, &dialog, [&, this] {
        if (active) return;
        bool validPort = false; const auto number = port->text().toUShort(&validPort);
        if (QHostAddress(host->text().trimmed()).isNull() || !validPort || number == 0) {
            status->setText("Enter the paired device's LAN IP address and port."); return;
        }
        const auto expected = peerBox->currentData().toString();
        auto connection = new LanConnection(*library_, false, &dialog);
        active = connection; connectButton->setEnabled(false); peerBox->setEnabled(false);
        games->clear(); installButton->setEnabled(false); status->setText("Connecting to trusted device…");
        connection->onError = [&, connection](const QString &reason) {
            if (active != connection) return;
            status->setText(reason); active = nullptr; exchange = nullptr;
            connectButton->setEnabled(true); peerBox->setEnabled(true); connection->deleteLater();
        };
        connection->onReady = [&, connection, expected, number](const QString &fingerprint) {
            if (active != connection) return;
            if (fingerprint != expected) {
                status->setText("The connected device is different from the one selected.");
                connection->close(); active = nullptr; exchange = nullptr;
                connectButton->setEnabled(true); peerBox->setEnabled(true); connection->deleteLater(); return;
            }
            try {
                rememberPeerEndpoint(*library_, fingerprint, host->text().trimmed(), number);
                auto router = new LanSaveExchange(*library_, *connection, connection);
                exchange = new LanGameExchange(*library_, *connection, *router, connection);
                exchange->onError = [&](const QString &reason) { status->setText(reason); };
                exchange->onCatalogue = [&, connection](const QJsonArray &catalogue) {
                    if (active != connection) return;
                    games->clear();
                    for (const auto &value : catalogue) {
                        const auto item = value.toObject();
                        auto row = new QListWidgetItem(item["name"].toString() +
                            (item["release_identity"].toString().isEmpty() ? QString() :
                                " · " + item["release_identity"].toString()), games);
                        row->setData(Qt::UserRole, item);
                    }
                    status->setText(catalogue.isEmpty() ? "No transferable games are available on this device." :
                                     "Select a game, then install it into Games.");
                };
                exchange->onProgress = [&, connection](qint64 done, qint64 total) {
                    if (active != connection) return;
                    status->setText("Transferring verified game files: " +
                        QString::number(double(done) / 1048576.0, 'f', 1) + " / " +
                        QString::number(double(total) / 1048576.0, 'f', 1) + " MiB");
                };
                exchange->onInstalled = [&, connection](const QString &gameId) {
                    if (active != connection) return;
                    installedId = gameId; dialog.accept();
                };
                exchange->catalogue();
            } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); connection->close(); }
        };
        connection->connectTo(host->text().trimmed(), number);
    });
    connect(installButton, &QPushButton::clicked, &dialog, [&] {
        if (!exchange || !games->currentItem()) return;
        try {
            exchange->install(games->currentItem()->data(Qt::UserRole).toJsonObject());
            installButton->setEnabled(false); games->setEnabled(false); connectButton->setEnabled(false);
            status->setText("Preparing the game file manifest on the other device…");
        } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
    });
    connect(&dialog, &QDialog::finished, &dialog, [&] { if (active) active->close(); });
    importing_ = true;
    dialog.exec();
    importing_ = false;
    if (installedId.isEmpty()) return;
    refresh();
    for (int row = 0; row < model_->games.size(); ++row) if (model_->games[row].id == installedId) {
        table_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, 0))); break;
    }
    try {
        const auto result = refreshLinkedPeerSaves(*library_, installedId);
        message(!result.issues.isEmpty() ?
                "Game installed. Peer saves can be checked again before Play: " + result.issues.join(" ") :
                result.peersChecked ? "Game installed from paired device. Save history checked." :
                "Game installed from paired device. Confirm its save identity and paths to sync progress.");
        if (library_->game(installedId).executable.isEmpty()) chooseExecutable(installedId);
        else launch(true);
    } catch (const std::exception &e) { message("Game installed. " + QString::fromUtf8(e.what())); }
}
void Window::syncSavesFromPeer(const QString &gameId, QWidget *parent) {
    if (!library_ || !rootAvailable()) throw std::runtime_error("Reconnect the Games folder before syncing saves.");
    const auto config = loadSaveConfig(*library_, gameId);
    if (config.gameIdentity.isEmpty() || config.releaseIdentity.isEmpty() ||
        config.saveCompatibility.isEmpty() || config.mappings.isEmpty())
        throw std::runtime_error("Confirm this game's identity, edition, save format and save paths first.");
    QDialog dialog(parent); dialog.setObjectName("syncSavesDialog"); dialog.setWindowTitle("Sync saves from peer");
    dialog.resize(530, 250);
    auto layout = new QVBoxLayout(&dialog);
    auto instructions = new QLabel("Enter a paired device's LAN IP and save-sync port from its Devices screen. Both devices must have Novelty open.", &dialog);
    instructions->setWordWrap(true); layout->addWidget(instructions);
    auto row = new QHBoxLayout;
    auto host = new QLineEdit(&dialog); host->setPlaceholderText("Peer LAN IP address");
    auto port = new QLineEdit("38373", &dialog); port->setValidator(new QIntValidator(1, 65535, port));
    port->setMaximumWidth(90);
    auto connectButton = new QPushButton("Check saves", &dialog);
    row->addWidget(host, 1); row->addWidget(port); row->addWidget(connectButton); layout->addLayout(row);
    auto status = new QLabel("Ready to check the paired device.", &dialog); status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText); layout->addWidget(status); layout->addStretch();
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    LanConnection *active = nullptr;
    connect(connectButton, &QPushButton::clicked, &dialog, [&, this] {
        if (active) return;
        bool validPort = false; const auto number = port->text().toUShort(&validPort);
        if (QHostAddress(host->text().trimmed()).isNull() || !validPort || number == 0) {
            status->setText("Enter the paired device's LAN IP address and port."); return;
        }
        auto connection = new LanConnection(*library_, false, &dialog);
        active = connection; connectButton->setEnabled(false); status->setText("Connecting to the trusted device…");
        connection->onError = [&, connection](const QString &reason) {
            if (active != connection) return;
            status->setText(reason); connectButton->setEnabled(true); active = nullptr;
            connection->deleteLater();
        };
        connection->onReady = [&, connection, config, gameId](const QString &peerFingerprint) {
            if (active != connection) return;
            status->setText("Checking compatible saves…");
            try { rememberPeerEndpoint(*library_, peerFingerprint, host->text().trimmed(), quint16(port->text().toUShort())); }
            catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); connection->close(); return; }
            auto exchange = new LanSaveExchange(*library_, *connection, connection);
            exchange->onError = [status](const QString &reason) { status->setText(reason); };
            exchange->onProgress = [status](qint64 bytes, qint64 total) {
                status->setText("Receiving verified saves: " + QString::number(bytes) + " / " +
                                QString::number(total) + " bytes");
            };
            exchange->onImported = [&, status, gameId](const SaveImportResult &result) {
                if (result.relation == "current" || result.relation == "behind") {
                    status->setText("Your current saves are already at least as recent as this peer snapshot."); return;
                }
                if (result.relation == "conflict") {
                    status->setText("Both devices changed saves. The peer copy is preserved in snapshot history. Choose which snapshot to restore in Manage → Saves.");
                    return;
                }
                status->setText("Verified peer snapshot received. Choose whether to restore it now.");
                QTimer::singleShot(0, &dialog, [&, gameId, snapshotId = result.snapshotId] {
                    if (QMessageBox::question(&dialog, "Restore peer saves?",
                        "Restore the verified peer snapshot into this installation? Novelty keeps a backup of the current saves.",
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
                    try {
                        const auto root = library_->root();
                        auto work = std::async(std::launch::async, [root, gameId, snapshotId] {
                            Library worker(root); restoreSaveSnapshot(worker, gameId, snapshotId);
                        });
                        RemovalProgress progress(&dialog); progress.setWindowTitle("Restoring peer saves");
                        auto progressLayout = new QVBoxLayout(&progress);
                        progressLayout->addWidget(new QLabel("Verifying and restoring mapped saves…", &progress));
                        QTimer poll; poll.setInterval(60);
                        connect(&poll, &QTimer::timeout, &progress, [&] {
                            if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
                        });
                        poll.start(); progress.exec(); poll.stop(); work.get();
                        status->setText("Peer saves restored. Local replaced progress was kept in snapshot history.");
                    } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
                });
            };
            exchange->onMatches = [&, exchange, peerFingerprint, gameId](const QJsonArray &matches) {
                if (matches.isEmpty()) { status->setText("No game with the same confirmed edition and save format is available on that device."); return; }
                QJsonObject selected;
                if (matches.size() == 1) selected = matches.first().toObject();
                else {
                    QDialog choose(&dialog); choose.setWindowTitle("Choose peer game");
                    auto column = new QVBoxLayout(&choose);
                    column->addWidget(new QLabel("Several installations match. Choose the one whose saves you want.", &choose));
                    auto options = new QComboBox(&choose);
                    for (const auto &value : matches) {
                        const auto item = value.toObject();
                        options->addItem(item["name"].toString() + " · " + item["profile_game_id"].toString().left(8), item);
                    }
                    column->addWidget(options);
                    auto actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &choose);
                    column->addWidget(actions);
                    connect(actions, &QDialogButtonBox::accepted, &choose, &QDialog::accept);
                    connect(actions, &QDialogButtonBox::rejected, &choose, &QDialog::reject);
                    if (choose.exec() != QDialog::Accepted) return;
                    selected = options->currentData().toJsonObject();
                }
                const auto remoteId = selected["profile_game_id"].toString();
                const auto head = selected["snapshot_id"].toString();
                if (QUuid(remoteId).isNull() || (!head.isEmpty() && QUuid(head).isNull())) {
                    status->setText("The peer returned an invalid game or snapshot ID."); return;
                }
                if (head.isEmpty()) { status->setText("This peer has no captured save data for that game yet."); return; }
                try {
                    if (remoteId != config.profileGameId) {
                        if (QMessageBox::question(&dialog, "Link matching games?",
                            "Link this installation with the peer's “" + selected["name"].toString() +
                            "” save history? The confirmed game, edition and save-format labels match.",
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
                        linkPeerSaveGame(*library_, gameId, peerFingerprint, remoteId);
                    }
                    status->setText("Checking the peer's save history…");
                    const auto reportImported = exchange->onImported;
                    connection->close(); active = nullptr; connection->deleteLater();
                    connectButton->setEnabled(true);
                    QTimer::singleShot(0, &dialog, [&, gameId, reportImported] {
                        try {
                            const auto result = refreshLinkedPeerSaves(*library_, gameId);
                            if (!result.issues.isEmpty()) { status->setText(result.issues.join(" ")); return; }
                            const auto candidates = importedSaveCandidates(*library_, gameId);
                            if (candidates.isEmpty()) {
                                status->setText("Your local saves are up to date with the paired device."); return;
                            }
                            if (candidates.size() != 1 || candidates.first().relation == "conflict") {
                                status->setText("Both devices changed saves. Both copies are preserved in Manage → Saves."); return;
                            }
                            reportImported({result.snapshotsImported > 0,
                                            candidates.first().relation, candidates.first().snapshotId});
                        } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
                    });
                } catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
            };
            try { exchange->locate(config); }
            catch (const std::exception &e) { status->setText(QString::fromUtf8(e.what())); }
        };
        connection->connectTo(host->text().trimmed(), number);
    });
    connect(&dialog, &QDialog::finished, &dialog, [&] { if (active) active->close(); });
    dialog.exec();
}
void Window::manage() {
    auto g = selected(); if (!library_ || g.id.isEmpty()) return;
    if (library_->busy(g.id)) {
        message(library_->supervisorActive(g.id) ? "This game is running or being prepared." : library_->recoveryIssue(g.id)); return;
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
    overviewRow("Backend", g.backend == "steam" ? "Steam shortcut" : "umu / Proton");
    const auto savedProfile = findSetupProfile(g.setupProfile);
    overviewRow("Setup profile", g.backend == "steam" ? "Managed by Steam" :
        (savedProfile ? savedProfile->title : "Unavailable: " + g.setupProfile));
    overviewRow("Proton directory", g.backend == "steam" ? "Managed by Steam" : g.runner);
    overviewRow("Prefix", g.backend == "steam" ? "Managed by Steam" : library_->prefix(g.id));
    overviewRow("Accepted runner", g.runnerRevision); overviewRow("Locale", g.locale.isEmpty() ? "System default" : g.locale);
    overviewRow("Steam launch ID", g.steamId); overviewRow("Arguments", g.arguments.join('\n'));
    QStringList overviewEnvironment; for (auto it = g.environment.begin(); it != g.environment.end(); ++it) overviewEnvironment.append(it.key() + '=' + it.value());
    overviewRow("Environment", overviewEnvironment.join('\n')); overviewRow("Engine evidence", g.evidence);
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
    relink->setEnabled(g.backend == "umu");
    relink->setToolTip(g.backend == "umu" ? "Keep this game's settings and prefix when its folder moves." : "Steam shortcut paths must currently be updated in Steam.");
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
    auto profile = new QComboBox(&d); profile->setObjectName("setupProfile");
    for (const auto &entry : setupProfiles()) profile->addItem(entry.title, entry.id);
    if (profile->findData(g.setupProfile) < 0) profile->addItem("Unavailable: " + g.setupProfile, g.setupProfile);
    profile->setCurrentIndex(profile->findData(g.setupProfile));
    auto profileNote = new QLabel(&d); profileNote->setObjectName("setupProfileNote");
    profileNote->setWordWrap(true); profileNote->setTextFormat(Qt::PlainText);
    auto resetProfile = new QPushButton("Use detected default", &d); resetProfile->setObjectName("resetSetupProfile");
    resetProfile->setToolTip("Change only the setup profile. Save to apply; other settings and existing prefix files are kept.");
    runtimeForm->addRow("Setup profile", profile); runtimeForm->addRow(profileNote); runtimeForm->addRow(resetProfile);
    auto updateProfile = [=] {
        const bool umu = backend->currentData() == "umu";
        profile->setEnabled(umu); resetProfile->setEnabled(umu);
        const auto selected = findSetupProfile(profile->currentData().toString());
        profileNote->setText(!umu ? "Steam manages this shortcut's setup." : selected ? selected->description :
            "This profile version is unavailable. Choose a supported profile before launching.");
    };
    connect(profile, &QComboBox::currentIndexChanged, &d, updateProfile);
    connect(backend, &QComboBox::currentIndexChanged, &d, updateProfile); updateProfile();
    connect(resetProfile, &QPushButton::clicked, &d, [&, profile] {
        profile->setCurrentIndex(profile->findData(recommendSetupProfile(g.engine).profileId));
    });
    auto runner = runnerBox(&d, g.runner, library_->root()); auto locale = new QComboBox(&d); locale->setEditable(true);
    locale->addItems({"", "ja_JP.UTF-8", "en_US.UTF-8"}); locale->setCurrentText(g.locale);
    auto effective = new QLabel(&d); effective->setObjectName("effectiveSetup");
    effective->setWordWrap(true); effective->setTextFormat(Qt::PlainText);
    auto updateEffective = [=, this] {
        if (backend->currentData() != "umu") {
            effective->setText("Steam manages this shortcut's runner and prefix."); return;
        }
        const auto selected = findSetupProfile(profile->currentData().toString());
        const auto recommendation = recommendSetupProfile(g.engine);
        const auto profileName = selected ? selected->title : "Unavailable profile";
        const auto profileSource = profile->currentData().toString() == recommendation.profileId ?
            recommendation.source : "selected for this game";
        QString runnerSource, runnerValue = runner->currentText().trimmed();
        if (!runnerValue.isEmpty()) runnerSource = "Per-game Proton";
        else {
            const auto libraryRunner = library_->settings()["runner"].toString();
            if (!libraryRunner.isEmpty() && QFileInfo(QDir(libraryRunner).filePath("proton")).isFile()) {
                runnerSource = "Library default Proton"; runnerValue = libraryRunner;
            } else {
                const auto pin = readJson(library_->dataRoot() + "/ge-proton.json")["version"].toString();
                runnerValue = pin.isEmpty() ? preferredProtonVersion(profile->currentData().toString()) : pin;
                runnerSource = pin.isEmpty() ? "Profile Proton default" : "Library GE pin";
            }
        }
        if (runnerValue.isEmpty()) { runnerSource = "Proton"; runnerValue = "choose at first Play"; }
        effective->setText("Setup: " + profileName + " (" + profileSource + ")\n" + runnerSource + ": " + runnerValue +
                           "\nLocale: " + (locale->currentText().isEmpty() ? "inherit system" : locale->currentText()) +
                           "\nArguments and environment overrides are editable in Launch and Advanced.");
    };
    auto prefix = new QLabel(library_->prefix(g.id), &d); prefix->setWordWrap(true); prefix->setTextInteractionFlags(Qt::TextSelectableByMouse);
    prefix->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto evidence = new QLabel(g.evidence, &d); evidence->setWordWrap(true); evidence->setTextFormat(Qt::PlainText);
    runtimeForm->addRow("Proton directory", runner); runtimeForm->addRow("Windows language / locale (blank = inherit)", locale);
    runtimeForm->addRow("Effective setup", effective);
    connect(profile, &QComboBox::currentIndexChanged, &d, updateEffective);
    connect(runner, &QComboBox::currentTextChanged, &d, updateEffective);
    connect(locale, &QComboBox::currentTextChanged, &d, updateEffective);
    connect(backend, &QComboBox::currentIndexChanged, &d, updateEffective);
    updateEffective();
    locale->setToolTip("Choose ja_JP.UTF-8 for older Japanese installers that display garbled text. Save before retrying an installer; this also sets the game's launch locale.");
    if (g.source == "installer") {
        auto localeNote = new QLabel("For garbled Japanese installer text, choose ja_JP.UTF-8 and Save. To rerun a completed installer, use Run another executable below.", &d);
        localeNote->setWordWrap(true); runtimeForm->addRow(localeNote);
    }
    auto getGe = new QPushButton("Get GE-Proton…", &d); getGe->setObjectName("getGeManage"); runtimeForm->addRow(getGe);
    getGe->setEnabled(g.backend == "umu");
    getGe->setToolTip("Install a GE-Proton build for umu. Steam compatibility tools are currently managed in Steam.");
    connect(getGe, &QPushButton::clicked, &d, [&, runner] {
        const auto installed = provisionGe(&d, library_->root());
        if (!installed.isEmpty()) { if (runner->findText(installed) < 0) runner->addItem(installed); runner->setCurrentText(installed); }
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
    auto savesIntro = new QLabel("Confirm where this game stores progress. Novelty snapshots only these locations. Identity and compatibility labels prepare matching between your devices; enter them only when you know they refer to the same game and save format.", savesTab);
    savesIntro->setWordWrap(true); savesLayout->addWidget(savesIntro);
    auto saveIdentity = new QLineEdit(savesTab), saveRelease = new QLineEdit(savesTab), saveCompatibility = new QLineEdit(savesTab);
    saveIdentity->setObjectName("saveGameIdentity"); saveRelease->setObjectName("saveReleaseIdentity");
    saveCompatibility->setObjectName("saveCompatibility");
    auto saveForm = new QFormLayout; saveForm->addRow("Game identity", saveIdentity);
    saveForm->addRow("Release / build", saveRelease); saveForm->addRow("Compatible saves", saveCompatibility);
    savesLayout->addLayout(saveForm);
    auto discoveryStatus = new QLabel(savesTab); discoveryStatus->setObjectName("saveDiscoveryStatus");
    discoveryStatus->setWordWrap(true); discoveryStatus->setTextFormat(Qt::PlainText);
    savesLayout->addWidget(discoveryStatus);
    auto saveCandidates = new QListWidget(savesTab); saveCandidates->setObjectName("saveCandidates");
    saveCandidates->setMaximumHeight(80); savesLayout->addWidget(saveCandidates);
    auto mappingList = new QListWidget(savesTab); mappingList->setObjectName("saveMappings");
    mappingList->setMaximumHeight(120); savesLayout->addWidget(mappingList);
    auto mappingRow = new QHBoxLayout;
    auto mappingPath = new QLineEdit(savesTab); mappingPath->setPlaceholderText("Save path, for example UserData");
    mappingPath->setToolTip("Paths you type are relative to the game folder. Browse to choose a file or folder elsewhere.");
    auto browseSaveFile = new QPushButton("File…", savesTab), browseSaveFolder = new QPushButton("Folder…", savesTab);
    mappingRow->addWidget(mappingPath, 1);
    mappingRow->addWidget(browseSaveFile); mappingRow->addWidget(browseSaveFolder); savesLayout->addLayout(mappingRow);
    auto mappingActions = new QHBoxLayout;
    auto addMapping = new QPushButton("Add path", savesTab), removeMapping = new QPushButton("Remove path", savesTab);
    auto useCandidate = new QPushButton("Use selected suggestion", savesTab);
    auto findMappings = new QPushButton("Find likely Ren'Py saves", savesTab); findMappings->setObjectName("findSaveMappings");
    auto persistSaves = new QPushButton("Save mappings", savesTab); persistSaves->setObjectName("saveMappingsButton");
    mappingActions->addWidget(addMapping); mappingActions->addWidget(removeMapping); mappingActions->addWidget(useCandidate);
    mappingActions->addWidget(findMappings); mappingActions->addStretch();
    mappingActions->addWidget(persistSaves); savesLayout->addLayout(mappingActions);
    auto discoveryActions = new QHBoxLayout;
    auto stopSearching = new QPushButton("Stop searching", savesTab), rescanSaves = new QPushButton("Search again", savesTab);
    stopSearching->setObjectName("stopSaveDiscovery"); rescanSaves->setObjectName("rescanSaveDiscovery");
    discoveryActions->addWidget(stopSearching); discoveryActions->addWidget(rescanSaves); discoveryActions->addStretch();
    savesLayout->addLayout(discoveryActions);
    auto snapshots = new QListWidget(savesTab); snapshots->setObjectName("saveSnapshots"); savesLayout->addWidget(snapshots, 1);
    auto snapshotActions = new QHBoxLayout;
    auto makeSnapshot = new QPushButton("Create snapshot", savesTab), restoreSnapshot = new QPushButton("Restore selected…", savesTab);
    auto syncFromPeer = new QPushButton("Sync from peer…", savesTab); syncFromPeer->setObjectName("syncSavesFromPeer");
    makeSnapshot->setObjectName("createSaveSnapshot"); restoreSnapshot->setObjectName("restoreSaveSnapshot");
    snapshotActions->addWidget(makeSnapshot); snapshotActions->addWidget(restoreSnapshot);
    snapshotActions->addWidget(syncFromPeer); snapshotActions->addStretch();
    savesLayout->addLayout(snapshotActions);
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
    auto renderSaveStatus = [this, discoveryStatus, saveCandidates, g, displaySaveLocation] {
        const auto config = loadSaveConfig(*library_, g.id);
        saveCandidates->clear();
        for (const auto &candidate : config.candidates) {
            const auto item = candidate.toObject();
            const SaveMapping mapping{item["scope"].toString(), item["path"].toString()};
            const auto location = displaySaveLocation(mapping);
            auto row = new QListWidgetItem(location, saveCandidates);
            row->setToolTip(location);
            row->setData(Qt::UserRole, mapping.scope); row->setData(Qt::UserRole + 1, mapping.path);
        }
        saveCandidates->setVisible(!config.candidates.isEmpty());
        if (config.discoveryState == "unidentified") {
            discoveryStatus->setText("Unidentified · You stopped save discovery. Local snapshots require a confirmed path."); return;
        }
        if (config.discoveryState == "searching") {
            discoveryStatus->setText("Searching · Novelty will check likely save locations on future launches. " +
                (config.candidates.isEmpty() ? "No save sync is configured." :
                 QString::number(config.candidates.size()) + " possible location(s) need confirmation. No save sync is configured.")); return;
        }
        const auto snapshots = listSaveSnapshots(*library_, g.id);
        const bool hasData = std::any_of(snapshots.begin(), snapshots.end(), [](const SaveSnapshot &item) { return item.files > 0; });
        discoveryStatus->setText(hasData ? "Identified · Local save snapshots are available." :
                                           "Identified · Save location is known, but no saved data has been captured yet.");
    };
    auto addRow = [mappingList, displaySaveLocation](const SaveMapping &mapping) {
        auto item = new QListWidgetItem(displaySaveLocation(mapping), mappingList);
        item->setData(Qt::UserRole, mapping.scope); item->setData(Qt::UserRole + 1, mapping.path);
    };
    try {
        const auto config = loadSaveConfig(*library_, g.id);
        saveIdentity->setText(config.gameIdentity); saveRelease->setText(config.releaseIdentity);
        saveCompatibility->setText(config.saveCompatibility);
        if (config.gameIdentity.isEmpty() && g.metadata.value("id").isString())
            saveIdentity->setPlaceholderText("Suggested: vndb:" + g.metadata.value("id").toString());
        for (const auto &mapping : config.mappings) addRow(mapping);
        renderSnapshots(); renderSaveStatus();
    } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    auto browseMapping = [&, mappingPath, displaySaveLocation](bool directory) {
        const auto chosen = directory ? QFileDialog::getExistingDirectory(&d, "Choose save folder", g.folder) :
                                        QFileDialog::getOpenFileName(&d, "Choose save file", g.folder);
        if (chosen.isEmpty()) return;
        try { mappingPath->setText(displaySaveLocation(saveMappingFromPath(*library_, g, chosen))); saveMessage->clear(); }
        catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    };
    connect(browseSaveFile, &QPushButton::clicked, &d, [browseMapping] { browseMapping(false); });
    connect(browseSaveFolder, &QPushButton::clicked, &d, [browseMapping] { browseMapping(true); });
    connect(addMapping, &QPushButton::clicked, &d, [=, this] {
        try {
            const auto mapping = saveMappingFromPath(*library_, g, mappingPath->text());
            addRow(mapping); mappingPath->clear(); saveMessage->clear();
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(useCandidate, &QPushButton::clicked, &d, [=] {
        auto item = saveCandidates->currentItem();
        if (!item) { saveMessage->setText("Select a suggested location first."); return; }
        addRow({item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString()});
        saveMessage->setText("Review the location, choose Save mappings, then Create snapshot to capture this play.");
    });
    connect(removeMapping, &QPushButton::clicked, &d, [mappingList] { delete mappingList->takeItem(mappingList->currentRow()); });
    connect(findMappings, &QPushButton::clicked, &d, [&, addRow] {
        try {
            int added = 0;
            for (const auto &mapping : suggestedSaveMappings(*library_, g.id)) {
                bool present = false;
                for (int i = 0; i < mappingList->count(); ++i)
                    if (mappingList->item(i)->data(Qt::UserRole).toString() == mapping.scope &&
                        mappingList->item(i)->data(Qt::UserRole + 1).toString() == mapping.path) { present = true; break; }
                if (!present) { addRow(mapping); ++added; }
            }
            saveMessage->setText(added ? "Found likely save folders. Review them, then choose Save mappings." :
                                         "No populated Ren'Py save folders were found. Add a path manually after you save in the game.");
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    auto saveMappingsNow = [&, saveIdentity, saveRelease, saveCompatibility, mappingList] {
        SaveConfig config = loadSaveConfig(*library_, g.id);
        config.gameIdentity = saveIdentity->text().trimmed();
        config.releaseIdentity = saveRelease->text().trimmed();
        config.saveCompatibility = saveCompatibility->text().trimmed();
        config.mappings.clear();
        for (int i = 0; i < mappingList->count(); ++i)
            config.mappings.append({mappingList->item(i)->data(Qt::UserRole).toString(),
                                    mappingList->item(i)->data(Qt::UserRole + 1).toString()});
        if (!config.mappings.isEmpty()) { config.discoveryState = "identified"; config.discoverySource = "manual"; }
        else if (config.discoveryState == "identified") { config.discoveryState = "searching"; config.discoverySource.clear(); }
        saveSaveConfig(*library_, g.id, config);
    };
    connect(persistSaves, &QPushButton::clicked, &d, [&, saveMappingsNow, renderSaveStatus] {
        try { saveMappingsNow(); renderSaveStatus(); saveMessage->setText("Save mappings and identity saved."); }
        catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(stopSearching, &QPushButton::clicked, &d, [&, renderSaveStatus] {
        try {
            if (!loadSaveConfig(*library_, g.id).mappings.isEmpty()) {
                saveMessage->setText("Remove confirmed paths and save before stopping discovery."); return;
            }
            setSaveDiscoveryState(*library_, g.id, "unidentified");
            mappingList->clear(); renderSaveStatus(); saveMessage->setText("Save discovery stopped for this game.");
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(rescanSaves, &QPushButton::clicked, &d, [&, renderSaveStatus] {
        try {
            setSaveDiscoveryState(*library_, g.id, "searching"); renderSaveStatus();
            saveMessage->setText("Save discovery will continue on the next Play.");
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(makeSnapshot, &QPushButton::clicked, &d, [&, saveMappingsNow, renderSnapshots, renderSaveStatus] {
        try {
            saveMappingsNow();
            const auto root = library_->root(), id = g.id;
            auto work = std::async(std::launch::async, [root, id] { Library worker(root); return createSaveSnapshot(worker, id); });
            RemovalProgress progress(&d); progress.setWindowTitle("Creating save snapshot");
            auto progressLayout = new QVBoxLayout(&progress);
            progressLayout->addWidget(new QLabel("Copying and verifying mapped saves…", &progress)); progress.resize(420, 100);
            QTimer poll; poll.setInterval(60);
            connect(&poll, &QTimer::timeout, &progress, [&] {
                if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
            });
            poll.start(); progress.exec(); poll.stop();
            const auto result = work.get();
            renderSnapshots(); renderSaveStatus(); saveMessage->setText("Saves checked. Latest verified snapshot: " + result.created);
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(syncFromPeer, &QPushButton::clicked, &d, [&, saveMappingsNow, renderSnapshots, renderSaveStatus] {
        try {
            saveMappingsNow();
            syncSavesFromPeer(g.id, &d);
            renderSnapshots(); renderSaveStatus();
        } catch (const std::exception &e) { saveMessage->setText(QString::fromUtf8(e.what())); }
    });
    connect(restoreSnapshot, &QPushButton::clicked, &d, [&, renderSnapshots, renderSaveStatus] {
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
    tabs->addTab(savesTab, "Saves");
    auto advanceTab = new QWidget(&d); auto al = new QVBoxLayout(advanceTab);
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
            g.runner = runner->currentText(); g.locale = locale->currentText(); g.backend = backend->currentData().toString(); g.steamId = steamId->text().trimmed();
            g.setupProfile = profile->currentData().toString();
            g.arguments = args->toPlainText().split('\n', Qt::SkipEmptyParts); g.environment.clear();
            for (auto line : env->toPlainText().split('\n', Qt::SkipEmptyParts)) {
                const int pos = line.indexOf('='); if (pos <= 0) throw std::runtime_error("Environment entries must use KEY=value.");
                g.environment.insert(line.left(pos).trimmed(), line.mid(pos + 1));
            }
            library_->save(g); d.accept();
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
    const auto g = selected(); if (!library_ || g.id.isEmpty()) return;
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
void Window::runExecutable(QDialog *manageDialog) {
    const auto g = selected(); if (!library_ || g.id.isEmpty()) return;
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
    if (!findSetupProfile(g.setupProfile)) {
        message("Choose a supported setup profile in Manage → Runtime before playing."); return false;
    }
    const auto originalRunner = g.runner;
    QString geVersionToInstall;
    if (g.runner.isEmpty()) {
        const auto preferred = library_->settings()["runner"].toString();
        const auto profileVersion = preferredProtonVersion(g.setupProfile);
        const auto pin = readJson(library_->dataRoot() + "/ge-proton.json")["version"].toString();
        const auto version = pin.isEmpty() ? profileVersion : pin;
        const auto available = installedRunners(library_->root());
        QString chosen = !preferred.isEmpty() && QFileInfo(QDir(preferred).filePath("proton")).isFile() ? preferred : QString();
        if (chosen.isEmpty() && !version.isEmpty()) {
            const auto managed = library_->dataRoot() + "/runners/" + version;
            chosen = managed;
            if (!QFileInfo(managed + "/proton").isFile()) {
                message("Preparing " + version + " for first Play…");
                geVersionToInstall = version;
            }
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
            if (choice.exec() != QDialog::Accepted) return false;
            chosen = runner->currentText();
        } else if (chosen.isEmpty()) {
            message("Preparing Proton for first Play…");
            chosen = provisionGe(this, library_->root(), {}, true);
        }
        if (chosen.isEmpty()) return false;
        g.runner = chosen;
    }
    const auto settingsBefore = library_->settings();
    if (!provisionFirstPlay(this, *library_, g, geVersionToInstall)) return false;
    if (g.runner != originalRunner) {
        library_->save(g); // Accept the selected runner only after the full first-Play setup succeeds.
        g = library_->game(g.id);
        refresh();
    } else if (settingsBefore != library_->settings()) refresh();
    return true;
}
bool Window::applyIncomingSavesBeforePlay(const QString &gameId) {
    const auto candidates = importedSaveCandidates(*library_, gameId);
    if (candidates.isEmpty()) return true;
    QList<ImportedSaveCandidate> newer;
    bool conflict = false;
    for (const auto &candidate : candidates) {
        if (candidate.relation == "conflict") conflict = true;
        else if (candidate.relation == "ahead" || candidate.relation == "available") newer.append(candidate);
    }
    if (conflict || newer.size() > 1) {
        const auto choice = QMessageBox::question(this, "Different saves on paired devices",
            "Paired-device save histories differ. Both copies are preserved in Manage → Saves. Play using the current local saves?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return choice == QMessageBox::Yes;
    }
    if (newer.isEmpty()) return true;
    const auto root = library_->root(), snapshotId = newer.first().snapshotId;
    try {
        auto work = std::async(std::launch::async, [root, gameId, snapshotId] {
            Library worker(root); restoreSaveSnapshot(worker, gameId, snapshotId);
        });
        RemovalProgress progress(this); progress.setWindowTitle("Applying paired-device saves");
        auto layout = new QVBoxLayout(&progress);
        layout->addWidget(new QLabel("Verifying and restoring the newer save before Play…", &progress));
        progress.resize(440, 110);
        QTimer poll; poll.setInterval(60);
        connect(&poll, &QTimer::timeout, &progress, [&] {
            if (work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) progress.accept();
        });
        poll.start(); progress.exec(); poll.stop(); work.get();
        refresh();
        return true;
    } catch (const std::exception &e) {
        message("Newer peer saves could not be applied: " + QString::fromUtf8(e.what()));
        return false;
    }
}
void Window::launch(bool prepareOnly, const QString &tool, const std::optional<ExtraLaunch> &extra, bool installing, bool debugWine) {
    auto g = selected(); if (!library_ || g.id.isEmpty() || !pendingLaunch_.isEmpty()) return;
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
        if (!installing && !prepareOnly && tool.isEmpty() && !extra) {
            const auto peers = trustedPeers(*library_);
            if (!peers.isEmpty()) {
                message("Checking paired-device saves before Play…");
                const auto result = refreshLinkedPeerSaves(*library_, g.id);
                if (!result.issues.isEmpty())
                    message("Some peers are unavailable; local saves remain usable. " + result.issues.join(" "));
            }
            if (!applyIncomingSavesBeforePlay(g.id)) return;
        }
        if (!prepareLaunchRuntime(g)) { message("Play preparation was cancelled. Your game and existing settings were kept."); return; }
        if (installing) extraLaunchCommand(*library_, g, {g.installer, {}, {}});
        else if (extra) extraLaunchCommand(*library_, g, *extra);
        else if (tool.isEmpty()) launchCommand(*library_, g, prepareOnly); else prefixToolCommand(*library_, g, tool);
        library_->recordStatus(g.id, "Starting");
        QProcess p; p.setProgram(launcherExecutable_);
        QStringList arguments{"--root", library_->root(), installing ? "--install" : prepareOnly ? "--prepare" : "--run", g.id};
        if (!installing && !prepareOnly && tool.isEmpty() && !extra) arguments.append("--peer-checked");
        if (!tool.isEmpty()) arguments.append({"--prefix-tool", tool});
        if (debugWine) arguments.append("--debug-wine");
        if (extra) {
            arguments.append("--extra-exe=" + extra->executable); arguments.append("--extra-cwd=" + extra->workingDirectory);
            for (const auto &argument : extra->arguments) arguments.append("--extra-arg=" + argument);
        }
        p.setArguments(arguments);
        p.setWorkingDirectory(g.workingDirectory);
        if (!p.startDetached()) throw std::runtime_error("Could not start the launch supervisor.");
        pendingLaunch_ = g.id; pendingInstaller_ = installing; launchStarted_ = QDateTime::currentMSecsSinceEpoch();
        launchPoll_.start(); selectGame(); message(debugWine ? "Launching once with Wine debug…" : installing ? "Opening installer… Choose N:\\ as the destination." : extra ? "Starting executable…" : !tool.isEmpty() ? "Opening " + tool + "…" : prepareOnly ? "Preparing game…" : "Starting game…");
    } catch (const std::exception &e) { message(QString::fromUtf8(e.what())); }
}
} // namespace novelty
