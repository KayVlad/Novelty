#pragma once
#include "runner_manager.h"
#include <QtCore/qobject.h>
#include <QtCore/qprocess.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qtimer.h>
#include <QtNetwork/qnetworkaccessmanager.h>
#include <QtNetwork/qnetworkreply.h>
#include <memory>

class QWidget;
namespace novelty {
// One asynchronous operation per library. No recurring update timer and no launch lock.
class GeUpdateManager : public QObject {
public:
    explicit GeUpdateManager(const QString &root, QObject *parent = nullptr,
        QUrl endpoint = QUrl("https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=100"),
        QString helper = {});
    ~GeUpdateManager() override;
    void checkAtStartup();
    void refreshCatalog(bool automatic = false, bool older = false);
    void install(const QString &version, bool automatic = false);
    void setAutomaticUpdates(bool enabled);
    const QList<GeRelease> &catalog() const { return catalog_; }
    QString status() const { return status_; }
    QString installingVersion() const { return installing_; }
    bool checking() const { return reply_ != nullptr; }
    bool hasOlderReleases() const { return more_; }
    bool automaticUpdates() const;
private:
    bool acquire();
    void recordCheck();
    QString root_, helper_, status_, installing_, request_;
    QUrl endpoint_;
    QList<GeRelease> catalog_;
    QNetworkAccessManager network_;
    QNetworkReply *reply_ = nullptr;
    QTimer timeout_;
    QProcess process_;
    std::unique_ptr<QLockFile> operation_;
    QByteArray output_, errors_, response_;
    bool automatic_ = false, more_ = true;
    int page_ = 1;
};
// Returned selection is committed by Settings/Manage, preserving their Cancel behavior.
QString manageProton(QWidget *parent, Library &, GeUpdateManager &, const QString &unsavedRunner = {});
}
