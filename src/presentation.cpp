#include "presentation.h"
#include <QtGui/qpainter.h>
#include <QtGui/qevent.h>
#include <QtGui/qpainterpath.h>
#include <QtGui/qimagereader.h>
#include <QtCore/qcache.h>
#include <QtWidgets/qstyle.h>
#include <QtWidgets/qscrollbar.h>
#include <algorithm>

namespace novelty {
namespace {
constexpr int cardWidth = 220;
constexpr int cardInset = 10;
int cardHeight(int width) { return qRound((width - cardInset) * 4.0 / 3.0) + cardInset; }
QCache<QString, QImage> &coverCache() { static QCache<QString, QImage> cache(32 * 1024); return cache; }
void drawCover(QPainter &p, const QRectF &r, const QString &name, const QImage &image = {}, bool remote = false) {
    p.save(); p.setClipRect(r, Qt::IntersectClip);
    if (!image.isNull()) {
        const double targetRatio = r.width() / r.height();
        const double imageRatio = double(image.width()) / image.height();
        QRectF source(0, 0, image.width(), image.height());
        if (imageRatio > targetRatio) {
            source.setWidth(image.height() * targetRatio);
            source.moveLeft((image.width() - source.width()) / 2);
        } else {
            source.setHeight(image.width() / targetRatio);
            source.moveTop((image.height() - source.height()) / 2);
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, image, source);
    } else {
        QLinearGradient gradient(r.topLeft(), r.bottomRight());
        const auto hue = 195 + qHash(name) % 45;
        gradient.setColorAt(0, QColor::fromHsl(hue, remote ? 0 : 100, 48));
        gradient.setColorAt(1, QColor::fromHsl(hue, remote ? 0 : 80, 25));
        p.fillRect(r, gradient);
        p.setPen(QPen(QColor(150, 205, 230, 24), 1));
        for (int i = -2; i < 7; ++i) p.drawLine(QPointF(r.left() + i * 65, r.bottom()), QPointF(r.left() + i * 65 + r.height(), r.top()));
        const QPointF center = r.center();
        p.setPen(QPen(QColor(160, 209, 235, 60), 1.5));
        p.setBrush(QColor(15, 33, 53, 70));
        p.drawRoundedRect(QRectF(center.x() - 30, center.y() - 34, 60, 68), 5, 5);
        p.setFont(QFont("", 25, QFont::Light)); p.setPen(QColor(190, 220, 238, 160));
        p.drawText(QRectF(center.x() - 30, center.y() - 34, 60, 68), Qt::AlignCenter, name.isEmpty() ? "N" : name.left(1).toUpper());
    }
    p.restore();
}
void textLine(QPainter &p, const QRect &r, const QString &text, const QColor &color, bool bold = false, int pixels = 14) {
    QFont font = p.font(); font.setPixelSize(pixels); font.setBold(bold); p.setFont(font); p.setPen(color);
    p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(text, Qt::ElideRight, r.width()));
}
}
QImage cachedCover(const QString &path) {
    if (path.isEmpty()) return {};
    if (auto image = coverCache().object(path)) return *image;
    QImageReader reader(path); reader.setAutoTransform(true);
    auto image = reader.read();
    if (!image.isNull()) coverCache().insert(path, new QImage(image), std::max(1, int(image.sizeInBytes() / 1024)));
    return image;
}
void invalidateCover(const QString &path) { coverCache().remove(path); }
QColor statusColor(const QString &status, bool setupPending) {
    if (setupPending) return QColor("#8dcfff");
    if (status == "Ready" || status == "Running") return QColor("#83ce9c");
    if (status.contains("attention", Qt::CaseInsensitive) || status.contains("rename", Qt::CaseInsensitive)) return QColor("#dfb676");
    return QColor("#91a3b8");
}
QIcon uiIcon(const QString &name) {
    QPixmap pix(24, 24); pix.fill(Qt::transparent); QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#d2ddec"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "plus") { p.drawLine(12, 5, 12, 19); p.drawLine(5, 12, 19, 12); }
    else if (name == "grid") for (int y : {5, 14}) for (int x : {5, 14}) p.drawRoundedRect(QRectF(x, y, 5, 5), 1, 1);
    else if (name == "list") for (int y : {6, 12, 18}) { p.drawPoint(5, y); p.drawLine(10, y, 20, y); }
    else if (name == "search") { p.drawEllipse(QRectF(4, 4, 11, 11)); p.drawLine(14, 14, 20, 20); }
    else if (name == "folder") { QPainterPath path; path.moveTo(3, 7); path.lineTo(3, 19); path.lineTo(21, 19); path.lineTo(21, 8); path.lineTo(11, 8); path.lineTo(9, 5); path.lineTo(3, 5); path.closeSubpath(); p.drawPath(path); }
    else if (name == "play") { p.setBrush(QColor("#d2ddec")); p.setPen(Qt::NoPen); QPainterPath path; path.moveTo(8, 5); path.lineTo(19, 12); path.lineTo(8, 19); path.closeSubpath(); p.drawPath(path); }
    else if (name == "trash") { p.setPen(QPen(QColor("#e99494"), 1.7)); p.drawLine(5, 6, 19, 6); p.drawLine(9, 3, 15, 3); p.drawRect(QRectF(7, 7, 10, 14)); p.drawLine(10, 10, 10, 17); p.drawLine(14, 10, 14, 17); }
    else if (name == "rescan") { p.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 280 * 16); p.drawLine(19, 4, 20, 10); p.drawLine(20, 10, 14, 9); }
    else if (name == "gear") { p.drawEllipse(QRectF(6, 6, 12, 12)); p.drawEllipse(QRectF(10, 10, 4, 4)); for (int a = 0; a < 360; a += 45) { p.save(); p.translate(12, 12); p.rotate(a); p.drawLine(0, -7, 0, -10); p.restore(); } }
    else if (name == "manage") { for (int x : {6, 12, 18}) p.drawLine(x, 4, x, 20); p.setBrush(QColor("#14263b")); p.drawEllipse(QPointF(6, 9), 2, 2); p.drawEllipse(QPointF(12, 15), 2, 2); p.drawEllipse(QPointF(18, 8), 2, 2); }
    else if (name == "gamepad") { p.drawRoundedRect(QRectF(2, 6, 20, 13), 4, 4); p.drawLine(6, 12, 12, 12); p.drawLine(9, 9, 9, 15); p.drawPoint(16, 10); p.drawPoint(19, 14); }
    return QIcon(pix);
}
Cover::Cover(QWidget *parent) : QWidget(parent) { setFixedHeight(200); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); }
void Cover::updateCoverHeight() {
    const auto &image = metadata_.cover;
    const int height = image.isNull() || width() < 1 ? 200 : qMax(1, qRound(double(width()) * image.height() / image.width()));
    if (this->height() != height) setFixedHeight(height);
}
void Cover::setGame(const QString &name, const GameMetadata &metadata) {
    name_ = name; metadata_ = metadata; updateCoverHeight(); update();
}
void Cover::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); updateCoverHeight(); }
void Cover::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip; clip.addRoundedRect(rect(), 8, 8); p.setClipPath(clip);
    if (metadata_.cover.isNull()) drawCover(p, rect(), name_, {}, metadata_.remote);
    else p.drawImage(rect(), metadata_.cover);
}
QSize CardDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const {
    if (const auto view = qobject_cast<const QListView *>(option.widget)) return view->gridSize();
    return QSize(cardWidth, cardHeight(cardWidth));
}
void CardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &o, const QModelIndex &i) const {
    auto &p = *painter; p.save(); p.setRenderHint(QPainter::Antialiasing);
    const auto r = o.rect.adjusted(5, 5, -5, -5);
    const bool selected = o.state & QStyle::State_Selected;
    p.setBrush(QColor("#15263b")); p.setPen(QPen(selected ? QColor("#62a4dc") : QColor("#2a3d55"), selected ? 2 : 1));
    p.drawRoundedRect(r, 8, 8);
    p.save(); QPainterPath clip; clip.addRoundedRect(r.adjusted(1, 1, -1, -1), 7, 7); p.setClipPath(clip);
    const auto cover = i.data(CoverRole);
    const bool remote = i.data(RemoteRole).toBool();
    auto image = cover.canConvert<QImage>() ? cover.value<QImage>() : cachedCover(cover.toString());
    if (remote && !image.isNull()) image = image.convertToFormat(QImage::Format_Grayscale8);
    drawCover(p, r, i.data().toString(), image, remote);
    QLinearGradient fade(r.topLeft(), r.bottomLeft());
    fade.setColorAt(0, QColor(5, 15, 29, 0)); fade.setColorAt(0.4, QColor(5, 15, 29, 0));
    fade.setColorAt(0.7, QColor(5, 15, 29, 205)); fade.setColorAt(1, QColor(5, 15, 29, 250)); p.fillRect(r, fade); p.restore();
    textLine(p, QRect(r.left() + 12, r.bottom() - 65, r.width() - 24, 28), i.data().toString(), QColor("#f0f4fa"), true, 17);
    textLine(p, QRect(r.left() + 12, r.bottom() - 38, r.width() - 48, 22), remote ? i.data(StatusRole).toString() : i.data(EngineRole).toString(), QColor("#a7b9cf"), false, 12);
    p.setPen(Qt::NoPen); p.setBrush(statusColor(i.data(StatusRole).toString(), i.data(SetupPendingRole).toBool()));
    p.drawEllipse(QPointF(r.right() - 18, r.bottom() - 27), 4, 4);
    p.restore();
}
GameGrid::GameGrid(QWidget *parent) : QListView(parent) {
    setObjectName("gameGrid"); setViewMode(QListView::IconMode); setResizeMode(QListView::Adjust);
    setMovement(QListView::Static); setWrapping(true); setUniformItemSizes(true); setSpacing(0);
    setSelectionMode(QAbstractItemView::SingleSelection); setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setItemDelegate(new CardDelegate(this)); setGridSize(QSize(cardWidth, cardHeight(cardWidth)));
}
void GameGrid::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    // QListView reserves scrollbar space for icon layout even before the bar appears.
    // Use that stable budget, leaving a pixel for its inclusive wrapping boundary.
    const int scrollbar = std::max(verticalScrollBar()->sizeHint().width(),
                                  style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar()));
    const int width = std::max(1, maximumViewportSize().width() - scrollbar - 2 * frameWidth() - 1);
    const int columns = std::max(1, width / 210);
    const int cellWidth = std::max(1, width / columns);
    const QSize cell(cellWidth, cardHeight(cellWidth));
    if (gridSize() != cell) setGridSize(cell);
}
void GameGrid::contextMenuEvent(QContextMenuEvent *event) {
    const auto index = indexAt(event->pos());
    if (!index.isValid() || (!index.data(ManageableRole).toBool() && !index.data(RemoteRole).toBool()) || !contextAction) return;
    contextAction(index, event->globalPos());
    event->accept();
}
} // namespace novelty
