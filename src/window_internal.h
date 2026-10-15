#pragma once
#include "window.h"
#include "app_settings.h"
#include "provisioning.h"
#include "recipes.h"
#include "runner_manager.h"
#include "setup.h"
#include "metadata.h"
#include "saves.h"
#include "save_server_config.h"
#include "save_server_client.h"
#include "save_server_sync.h"
#include "steam_shortcut.h"
#include "devices.h"
#include "lan_connection.h"
#include "lan_saves.h"
#include "lan_sync.h"
#include "lan_games.h"
#include "lan_discovery.h"
#include "runtime_migration.h"
#include "pairing.h"
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qbuffer.h>
#include <QtCore/qprocess.h>
#include <QtCore/qsavefile.h>
#include <QtCore/quuid.h>
#include <QtCore/qsysinfo.h>
#include <QtGui/qdesktopservices.h>
#include <QtGui/qfontmetrics.h>
#include <QtGui/qaction.h>
#include <QtGui/qimagereader.h>
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
class RemovalProgress : public QDialog {
public:
    using QDialog::QDialog;
    void reject() override {} // Permanent cleanup cannot be cancelled after confirmation.
};
QWidget *pathField(QLineEdit *edit, QWidget *parent, bool directory, const QString &caption, QLineEdit *initialFolder = nullptr);
QComboBox *backendBox(QWidget *parent, const QString &current, bool allowNative = true);
QComboBox *runnerBox(QWidget *parent, const QString &current, const QString &libraryRoot);
QString askPresetId(QWidget *parent, const QString &title, const QString &suggested);
QStringList metadataStrings(const QJsonValue &value);
} // namespace novelty
