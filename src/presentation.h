#pragma once
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtGui/qimage.h>
#include <QtGui/qicon.h>
#include <QtGui/qevent.h>
#include <QtWidgets/qlistview.h>
#include <QtWidgets/qstyleditemdelegate.h>
#include <functional>
#include <optional>

namespace novelty {
// Cached metadata shown by the library and detail views.
struct GameMetadata {
    QImage cover;
    QString description;
    QStringList tags;
    std::optional<QString> language;
    std::optional<QString> playtime;
    std::optional<QString> lastPlayed;
};
enum GameRole { EngineRole = Qt::UserRole + 1, StatusRole, LaunchableRole, ManageableRole, CoverRole, SetupPendingRole };
QImage cachedCover(const QString &path);
void invalidateCover(const QString &path);
QIcon uiIcon(const QString &name);
QColor statusColor(const QString &status, bool setupPending = false);
class Cover : public QWidget {
public:
    explicit Cover(QWidget *parent = nullptr);
    void setGame(const QString &name, const GameMetadata &metadata = {});
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    void updateCoverHeight();
    QString name_;
    GameMetadata metadata_;
};
class CardDelegate : public QStyledItemDelegate {
public:
    explicit CardDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;
};
class GameGrid : public QListView {
public:
    explicit GameGrid(QWidget *parent = nullptr);
    std::function<void(const QModelIndex &, const QPoint &)> contextAction;
protected:
    void resizeEvent(QResizeEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
};
} // namespace novelty
