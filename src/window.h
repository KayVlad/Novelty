#pragma once
#include "core.h"
#include "presentation.h"
#include "devices.h"
#include <QtWidgets/qmainwindow.h>
#include <QtCore/qabstractitemmodel.h>
#include <QtCore/qsortfilterproxymodel.h>
#include <QtCore/qfilesystemwatcher.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qtimer.h>
#include <QtCore/qurl.h>
#include <QtCore/qlist.h>
#include <QtCore/qset.h>
#include <future>

class QTableView;
class QLabel;
class QPushButton;
class QLineEdit;
class QStackedWidget;
class QDialog;
class QAction;
class QProcess;

namespace novelty {
class VndbClient;
class LanListener;
class LanConnection;
class LanDiscovery;
struct PeerGame {
    TrustedPeer peer;
    QJsonObject descriptor;
    QImage cover;
    bool online = true;
    QString id() const { return "peer:" + peer.fingerprint + ':' + descriptor["profile_game_id"].toString(); }
};
class GameModel : public QAbstractTableModel {
public:
    explicit GameModel(QObject *parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : games.size(); }
    int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex &, int role) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    void load(const Library &, const QList<PeerGame> &peers = {});
    bool updateState(const Library &, const QString &id);
    void unavailable();
    QList<Game> games;
    QStringList states;
    QList<bool> launchable, manageable, setupPending;
    QString coverDataRoot;
    bool actionsAllowed = true;
    QHash<QString, PeerGame> remoteGames;
};

class Window : public QMainWindow {
public:
    explicit Window(const QString &root = {}, const QString &launcherExecutable = {},
                    QUrl metadataApiBase = QUrl("https://api.vndb.org/kana/"));
    ~Window() override;
    void scan();
    void chooseRoot();
    void manage();
    void relinkGame();
    void settings();
    void recipes();
    void devices();
    void syncSavesFromPeer(const QString &gameId, QWidget *parent);
    void installFromPeer();
    void refreshPeerGames();
    void syncPendingPeers();
    void addGame();
    void launch(bool prepareOnly = false, const QString &tool = {}, const std::optional<ExtraLaunch> &extra = {}, bool installing = false, bool debugWine = false);
private:
    void openRoot(const QString &, bool allowCreation = true);
    bool rootAvailable() const;
    void refresh();
    void selectGame();
    void message(const QString &);
    void updateRootLink();
    void removeGame(const QString &id, QDialog *manageDialog);
    void runExecutable(QDialog *manageDialog);
    bool chooseExecutable(const QString &id);
    bool prepareLaunchRuntime(Game &game);
    void queueAutomaticMetadata(const QString &id);
    void startNextAutomaticMetadata();
    void retryPendingServerSaves();
    Game selected() const;
    void queryPeerGames(const TrustedPeer &);
    std::unique_ptr<Library> library_;
    std::unique_ptr<LanListener> saveListener_;
    std::unique_ptr<LanDiscovery> peerDiscovery_;
    QSet<LanConnection *> catalogueConnections_;
    QSet<QString> cataloguePeers_;
    QHash<QString, QList<PeerGame>> peerGames_;
    QHash<QString, qint64> peerLastAttempt_;
    QSet<LanConnection *> autoConnections_;
    QSet<QString> activePeerIds_;
    QString lanListenIssue_;
    QString syncQueueIssue_;
    GameModel *model_;
    QSortFilterProxyModel *proxy_;
    QTableView *table_;
    GameGrid *grid_;
    QStackedWidget *views_;
    Cover *cover_;
    QLabel *status_, *setupState_, *syncState_, *description_, *playtimeValue_, *tags_;
    QLabel *rootLabel_, *title_, *originalTitle_, *detail_, *notice_;
    QPushButton *scanButton_;
    QAction *listPlay_, *listManage_, *listInstall_;
    QLineEdit *search_;
    QFileSystemWatcher watcher_;
    QTimer scanDelay_, scanPoll_, launchPoll_, syncTimer_;
    QProcess *serverRetryProcess_ = nullptr;
    QHash<QString, qint64> serverNextRetry_;
    QHash<QString, int> serverRetryFailures_;
    std::future<DiscoverySnapshot> pendingScan_;
    CopyTracker copyTracker_;
    QElapsedTimer discoveryClock_;
    QString rootPath_, scanRoot_, pendingLaunch_;
    bool pendingInstaller_ = false;
    bool pendingPrepare_ = false;
    bool rootRequiresDatabase_ = false;
    bool removing_ = false;
    bool importing_ = false;
    int scanGeneration_ = 0, pendingScanGeneration_ = 0;
    qint64 launchStarted_ = 0;
    QString launcherExecutable_;
    QUrl metadataApiBase_;
    VndbClient *autoMetadataClient_ = nullptr;
    QList<QString> autoMetadataQueue_;
    QSet<QString> autoMetadataQueued_;
    bool autoMetadataActive_ = false;
    int autoMetadataGeneration_ = 0;
};
} // namespace novelty
