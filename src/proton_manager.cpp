#include "proton_manager.h"
#include "app_settings.h"
#include "recipes.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qset.h>
#include <QtCore/qhash.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qnetworkrequest.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qdialog.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qmessagebox.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qscrollbar.h>
#include <QtWidgets/qtabwidget.h>
#include <algorithm>

namespace novelty {
GeUpdateManager::GeUpdateManager(const QString &root, QObject *parent, QUrl endpoint, QString helper)
    : QObject(parent), root_(canonicalPath(root)), helper_(std::move(helper)), endpoint_(std::move(endpoint)),
      network_(this), timeout_(this), process_(this) {
    try { catalog_ = cachedGeCatalog(root_); } catch (...) { /* A fresh request can replace a damaged cache. */ }
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { if (reply_) reply_->abort(); });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { output_ = (output_ + process_.readAllStandardOutput()).right(65536); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { errors_ = (errors_ + process_.readAllStandardError()).right(65536); });
    const auto finished = [this](bool success) {
        output_ += process_.readAllStandardOutput(); errors_ += process_.readAllStandardError();
        const auto path = root_ + "/.novelty/runners/" + installing_;
        success = success && QString::fromUtf8(output_).trimmed() == path && runnerInstallationReady(root_, path);
        status_ = success ? "Installed " + installing_ + ". Existing versions were kept." :
            "Could not install " + installing_ + ": " + (errors_.isEmpty() ? process_.errorString() : QString::fromUtf8(errors_).trimmed());
        QFile::remove(request_); request_.clear(); installing_.clear(); operation_.reset();
    };
    connect(&process_, &QProcess::errorOccurred, this, [finished](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) finished(false); });
    connect(&process_, &QProcess::finished, this, [finished](int code, QProcess::ExitStatus exit) { finished(code == 0 && exit == QProcess::NormalExit); });
}
GeUpdateManager::~GeUpdateManager() {
    if (reply_) { disconnect(reply_, nullptr, this, nullptr); reply_->abort(); }
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(1000); }
    QFile::remove(request_);
}
bool GeUpdateManager::automaticUpdates() const { return loadAppSettings()["autoGeUpdates"].toBool(); }
bool GeUpdateManager::acquire() {
    if (reply_ || !installing_.isEmpty()) return false;
    operation_ = std::make_unique<QLockFile>(root_ + "/.novelty/ge-update.lock");
    operation_->setStaleLockTime(0);
    if (operation_->tryLock()) return true;
    operation_.reset(); status_ = "Another Novelty window is managing GE-Proton. Installed runners remain available.";
    return false;
}
void GeUpdateManager::recordCheck() {
    writeJson(root_ + "/.novelty/ge-update-state.json", {{"schema", 1},
        {"last_check", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}
void GeUpdateManager::checkAtStartup() {
    try {
        if (!automaticUpdates()) { status_ = "Automatic GE-Proton updates are disabled."; return; }
        const auto state = readJson(root_ + "/.novelty/ge-update-state.json");
        const auto checked = QDateTime::fromString(state["last_check"].toString(), Qt::ISODate);
        const auto age = checked.secsTo(QDateTime::currentDateTimeUtc());
        if (checked.isValid() && age >= 0 && age < 86400) { status_ = "GE-Proton was checked in the last day."; return; }
        refreshCatalog(true);
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); }
}
void GeUpdateManager::refreshCatalog(bool automatic, bool older) {
    try {
        if (!acquire()) return;
        if (endpoint_.scheme() != "https" && endpoint_.scheme() != "file") throw std::runtime_error("GE catalog requires HTTPS.");
        automatic_ = automatic;
        if (!older) { page_ = 1; recordCheck(); }
        auto url = endpoint_;
        if (older) {
            auto query = url.query().split('&', Qt::SkipEmptyParts);
            query.removeIf([](const QString &item) { return item.startsWith("page="); });
            query.append("page=" + QString::number(page_)); url.setQuery(query.join('&'));
        }
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "Novelty/2"); request.setRawHeader("Accept", "application/vnd.github+json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        response_.clear(); reply_ = network_.get(request); reply_->setReadBufferSize(2 * 1024 * 1024 + 1); timeout_.start(10000);
        status_ = "Checking official GE-Proton releases…";
        connect(reply_, &QNetworkReply::readyRead, this, [this] {
            response_ += reply_->readAll();
            if (response_.size() > 2 * 1024 * 1024) reply_->abort();
        });
        connect(reply_, &QNetworkReply::finished, this, [this] {
            auto *reply = reply_; reply_ = nullptr; timeout_.stop();
            response_ += reply->readAll();
            const bool automatic = automatic_;
            bool okay = false;
            try {
                if (reply->error() != QNetworkReply::NoError) throw std::runtime_error(reply->errorString().toStdString());
                const auto entries = parseGeCatalog(response_);
                more_ = reply->rawHeader("Link").contains("rel=\"next\""); ++page_;
                for (const auto &entry : entries) {
                    catalog_.removeIf([&](const auto &old) { return old.version == entry.version; });
                    catalog_.append(entry);
                }
                std::sort(catalog_.begin(), catalog_.end(), [](const auto &a, const auto &b) { return newerGeVersion(a.version, b.version); });
                cacheGeCatalog(root_, catalog_);
                status_ = entries.isEmpty() ? "No more supported releases." : "Official GE-Proton releases loaded.";
                okay = !entries.isEmpty();
            } catch (const std::exception &error) { status_ = "Release check failed; cached releases remain available. " + QString::fromUtf8(error.what()); }
            reply->deleteLater(); operation_.reset();
            try {
                if (okay && automatic && automaticUpdates() && !catalog_.isEmpty()) {
                    const auto latest = resolveRunnerSelector(root_, "ge-proton-latest");
                    if (newerGeVersion(catalog_.first().version, QFileInfo(latest).fileName())) install(catalog_.first().version, true);
                    else status_ = "GE-Proton is up to date. Installed runners are ready to use.";
                }
            } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); }
        });
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); operation_.reset(); }
}
void GeUpdateManager::install(const QString &version, bool automatic) {
    if (reply_ || !installing_.isEmpty()) return;
    try {
        if (!std::any_of(catalog_.cbegin(), catalog_.cend(), [&](const auto &entry) { return entry.version == version; }))
            throw std::runtime_error("Select a supported official release from the list.");
        if (!resolveRunnerSelector(root_, version).isEmpty()) { status_ = version + " is already installed; no duplicate download is needed."; return; }
        if (geInstallInProgress(root_, version)) { status_ = version + " is already being installed. Other runners remain available."; return; }
        if (!acquire()) return;
        auto helper = helper_;
        if (helper.isEmpty()) for (const auto &directory : {"/../share/novelty/", "/../scripts/"}) {
            const auto candidate = QCoreApplication::applicationDirPath() + directory + "install-ge.py";
            if (QFileInfo(candidate).isFile()) { helper = canonicalPath(candidate); break; }
        }
        if (!QFileInfo(helper).isFile()) throw std::runtime_error("The bundled GE installer helper is missing.");
        auto python = resolveProgram(QCoreApplication::applicationDirPath() + "/../python/bin/python3");
        if (python.isEmpty()) python = resolveProgram("python3");
        if (python.isEmpty()) throw std::runtime_error("The GE installer requires Python 3.");
        request_ = root_ + "/.novelty/ge-request-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
        writeJson(request_, {{"schema", 1}, {"version", version}, {"architecture", "x86_64"}, {"auto_download", true}});
        output_.clear(); errors_.clear(); automatic_ = automatic; installing_ = version;
        status_ = "Installing " + version + " in the background — downloading and verifying. Games can use installed runners.";
        process_.setProcessEnvironment(externalToolEnvironment());
        process_.start(python, {helper, root_, "--config", request_});
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what()); QFile::remove(request_); request_.clear(); installing_.clear(); operation_.reset();
    }
}
void GeUpdateManager::setAutomaticUpdates(bool enabled) {
    setAppSetting("autoGeUpdates", enabled);
    if (!enabled && automatic_) {
        if (reply_) reply_->abort();
        if (!installing_.isEmpty()) {
            const auto pid = process_.processId(); process_.terminate();
            QTimer::singleShot(2000, this, [this, pid] { if (process_.state() != QProcess::NotRunning && process_.processId() == pid) process_.kill(); });
        }
    }
    if (enabled) checkAtStartup();
}
QString manageProton(QWidget *parent, Library &library, GeUpdateManager &updates, const QString &unsavedRunner) {
    QDialog dialog(parent); dialog.setObjectName("protonManagerDialog"); dialog.setWindowTitle("Proton Manager"); dialog.resize(760, 580);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget(&dialog); tabs->setObjectName("protonManagerTabs"); layout->addWidget(tabs, 1);
    auto ge = new QWidget(tabs); auto body = new QVBoxLayout(ge); tabs->addTab(ge, "GE-Proton");
    auto automatic = new QCheckBox("Automatic check", ge);
    automatic->setToolTip("Check for new GE-Proton releases at startup, at most once per day, and install updates in the background.");
    automatic->setObjectName("automaticGeUpdates"); automatic->setChecked(updates.automaticUpdates()); body->addWidget(automatic);
    auto installed = new QListWidget(ge); installed->setObjectName("managedGeList");
    installed->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    body->addWidget(new QLabel("Official releases and Novelty-managed installations", ge)); body->addWidget(installed, 1);
    auto catalogActions = new QHBoxLayout;
    auto refresh = new QPushButton("Refresh catalog", ge); refresh->setObjectName("refreshGeCatalog"); catalogActions->addWidget(refresh);
    auto older = new QPushButton("Load older releases", ge); older->setObjectName("olderGeReleases"); catalogActions->addWidget(older);
    body->addLayout(catalogActions);
    auto external = new QWidget(tabs); auto externalLayout = new QVBoxLayout(external); tabs->addTab(external, "External Proton");
    auto note = new QLabel("Steam Proton and compatibility tools are discovered automatically. Register another Proton installation or a directory containing several versions below. Novelty only reads these locations.", external);
    note->setWordWrap(true); externalLayout->addWidget(note);
    auto row = new QHBoxLayout;
    auto path = new QLineEdit(external); path->setObjectName("externalProtonPath"); path->setPlaceholderText("/path/to/Proton or compatibility tools directory"); row->addWidget(path, 1);
    auto add = new QPushButton("Add", external); add->setObjectName("addExternalProtonPath"); row->addWidget(add); externalLayout->addLayout(row);
    auto locations = new QListWidget(external); locations->setObjectName("externalProtonLocations"); externalLayout->addWidget(locations, 1);
    auto unregister = new QPushButton("Remove registration", external); unregister->setObjectName("removeExternalProtonPath"); externalLayout->addWidget(unregister);
    auto externalDefault = new QPushButton("Use selected installation as default", external); externalDefault->setObjectName("defaultExternalProton"); externalLayout->addWidget(externalDefault);
    auto versions = new QListWidget(external); versions->setObjectName("externalProtonVersions"); externalLayout->addWidget(new QLabel("Available external installations (including Steam)", external)); externalLayout->addWidget(versions, 1);
    auto status = new QLabel(&dialog); status->setObjectName("protonManagerStatus"); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    auto close = new QPushButton("Close", &dialog); layout->addWidget(close, 0, Qt::AlignRight);
    QString selected, signature, localStatus;
    const auto removeVersion = [&](QString version) {
        try {
            const auto candidate = library.dataRoot() + "/runners/" + version;
            if (unsavedRunner == candidate || resolveRunnerSelector(library.root(), unsavedRunner) == candidate)
                throw std::runtime_error("This build is selected in Settings. Choose another default and save Settings before removing it.");
            const auto issue = managedRunnerRemovalIssue(library, version);
            if (!issue.isEmpty()) throw std::runtime_error(issue.toStdString());
            if (QMessageBox::question(&dialog, "Remove GE-Proton?", "Remove managed " + version + "?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            removeManagedRunner(library, version); localStatus = version + " removed."; signature.clear();
        } catch (const std::exception &error) { localStatus = QString::fromUtf8(error.what()); }
        status->setText(localStatus);
    };
    const auto render = [&] {
        try {
            const auto inventory = installedRunners(library.root());
            QStringList releaseNames; for (const auto &release : updates.catalog()) releaseNames.append(release.version);
            const auto roots = externalProtonLocations();
            const auto entries = managedGeRunners(library);
            QStringList managedState;
            for (const auto &entry : entries) managedState.append(entry.version + QString::number(entry.verified));
            const bool installing = geInstallInProgress(library.root());
            const auto next = inventory.join('\n') + roots.join('\n') + releaseNames.join('\n') + managedState.join('\n') +
                updates.status() + updates.installingVersion() + QString::number(installing);
            status->setText(localStatus.isEmpty() ? updates.status() : localStatus);
            older->setEnabled(updates.hasOlderReleases() && !updates.checking());
            refresh->setEnabled(!updates.checking() && updates.installingVersion().isEmpty());
            if (next == signature) return;
            signature = next;
            const auto scroll = installed->verticalScrollBar()->value();
            const auto current = installed->currentItem() ? installed->currentItem()->data(Qt::UserRole).toString() : QString();
            installed->clear();
            QSet<QString> managed;
            QHash<QString, ManagedRunner> managedVersions;
            for (const auto &entry : entries) {
                managed.insert(entry.path); managedVersions.insert(entry.version, entry);
                if (!releaseNames.contains(entry.version)) releaseNames.append(entry.version);
            }
            QHash<QString, QString> byVersion;
            versions->clear();
            for (const auto &runner : inventory) {
                // Prefer the managed copy when the same version exists in several locations.
                const auto name = QFileInfo(runner).fileName();
                if (!byVersion.contains(name) || managed.contains(runner)) byVersion.insert(name, runner);
                if (managed.contains(runner)) continue;
                auto item = new QListWidgetItem(name + " · " + runner, versions);
                item->setData(Qt::UserRole, runner);
            }
            std::sort(releaseNames.begin(), releaseNames.end(), newerGeVersion);
            for (const auto &version : releaseNames) {
                const auto found = byVersion.value(version);
                const bool active = version == updates.installingVersion() || geInstallInProgress(library.root(), version);
                const bool needsReview = managedVersions.contains(version) && !managedVersions.value(version).verified;
                const auto state = active ? "Installing…" : !found.isEmpty() ? (managed.contains(found) ? "Installed, verified" : "Installed externally") :
                    needsReview ? "Unavailable, receipt needs review" : "Available";
                auto item = new QListWidgetItem(installed); item->setData(Qt::UserRole, version);
                item->setToolTip(found.isEmpty() ? managedVersions.value(version).path : found);
                auto widget = new QWidget(installed); auto line = new QHBoxLayout(widget); line->setContentsMargins(5, 2, 5, 2);
                auto label = new QLabel(version + " · " + state, widget); label->setTextFormat(Qt::PlainText); line->addWidget(label, 1);
                // Only catalog releases can be installed, and existing directories are never overwritten.
                if (managedVersions.contains(version)) {
                    auto button = new QPushButton("Remove", widget); button->setObjectName("removeGe_" + version);
                    button->setEnabled(managedVersions.value(version).verified && !active); line->addWidget(button);
                    QObject::connect(button, &QPushButton::clicked, &dialog, [&, version] { removeVersion(version); });
                } else if (std::any_of(updates.catalog().cbegin(), updates.catalog().cend(), [&](const auto &release) { return release.version == version; })) {
                    auto button = new QPushButton("Install", widget); button->setObjectName("installGe_" + version);
                    button->setEnabled(!active && found.isEmpty() && !needsReview && !installing && updates.installingVersion().isEmpty() && !updates.checking()); line->addWidget(button);
                    QObject::connect(button, &QPushButton::clicked, &dialog, [&, version] { localStatus.clear(); updates.install(version); signature.clear(); });
                }
                item->setSizeHint(widget->sizeHint()); installed->setItemWidget(item, widget);
                if (version == current) installed->setCurrentItem(item);
            }
            installed->doItemsLayout();
            installed->verticalScrollBar()->setValue(scroll);
            locations->clear();
            for (const auto &root : roots) {
                const auto state = QFileInfo(root).isDir() ? "registered" : "missing — reconnect or remove registration";
                auto item = new QListWidgetItem(root + " · " + state, locations); item->setData(Qt::UserRole, root);
            }
        } catch (const std::exception &error) { status->setText(QString::fromUtf8(error.what())); }
    };
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(automatic, &QCheckBox::toggled, &dialog, [&](bool enabled) {
        try { localStatus.clear(); updates.setAutomaticUpdates(enabled); } catch (const std::exception &error) { localStatus = QString::fromUtf8(error.what()); }
        render();
    });
    QObject::connect(refresh, &QPushButton::clicked, &dialog, [&] { localStatus.clear(); updates.refreshCatalog(); render(); });
    QObject::connect(older, &QPushButton::clicked, &dialog, [&] { localStatus.clear(); updates.refreshCatalog(false, true); render(); });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        try { addExternalProtonLocation(path->text()); path->clear(); localStatus = "External location registered."; signature.clear(); }
        catch (const std::exception &error) { localStatus = QString::fromUtf8(error.what()); }
        render();
    });
    QObject::connect(unregister, &QPushButton::clicked, &dialog, [&] {
        if (!locations->currentItem()) return;
        try {
            removeExternalProtonLocation(locations->currentItem()->data(Qt::UserRole).toString());
            localStatus = "Registration removed. The external installation remains on disk."; signature.clear();
        } catch (const std::exception &error) { localStatus = QString::fromUtf8(error.what()); }
        render();
    });
    QObject::connect(externalDefault, &QPushButton::clicked, &dialog, [&] {
        if (!versions->currentItem()) return;
        selected = versions->currentItem()->data(Qt::UserRole).toString(); dialog.accept();
    });
    QTimer poll; poll.setInterval(300); QObject::connect(&poll, &QTimer::timeout, &dialog, render); poll.start();
    render();
    if (updates.catalog().isEmpty() && !updates.checking()) updates.refreshCatalog();
    dialog.exec();
    return selected;
}
}
