#include "playtime_chart.h"
#include <QtGui/qevent.h>
#include <QtGui/qpainter.h>
#include <QtGui/qpainterpath.h>
#include <QtWidgets/qtooltip.h>
#include <algorithm>
#include <cmath>
namespace novelty {
namespace {
constexpr double hourMs = 3600000.0;
const QColor barColor("#3b82d0"), lineColor("#e69337"), green("#36a269");
QRectF plotRect(const QWidget *widget) { return QRectF(76, 40, std::max(1, widget->width()-172), std::max(1, widget->height()-92)); }
struct Axis {
    double maximum, unitMs;
    QString unit;
    double hours() const { return maximum * unitMs / hourMs; }
};
Axis axisFor(const QList<PlaytimeDay> &days, bool cumulative) {
    qint64 maximum = 0;
    for (const auto &day : days) maximum = std::max(maximum, cumulative ? day.cumulativeMs : day.foregroundMs);
    const bool minutes = maximum < hourMs;
    const double unitMs = minutes ? 60000.0 : hourMs;
    const double value = std::max(1.0, maximum / unitMs);
    const auto magnitude = std::pow(10.0, std::floor(std::log10(value)));
    return {std::ceil(value / magnitude * 2) * magnitude / 2, unitMs, minutes ? "minutes" : "hours"};
}
QString duration(qint64 ms) { return formatTrackedTime(ms, true); }
}
PlaytimeChart::PlaytimeChart(QWidget *parent) : QWidget(parent) {
    setObjectName("playtimeChart"); setMinimumSize(360, 260);
    setAccessibleName("Daily and cumulative foreground playtime over the last 30 local calendar days");
}
void PlaytimeChart::setDays(const QList<PlaytimeDay> &days) { days_ = days; update(); }
double PlaytimeChart::dailyAxisHours() const { return axisFor(days_, false).hours(); }
double PlaytimeChart::cumulativeAxisHours() const { return axisFor(days_, true).hours(); }
void PlaytimeChart::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const auto plot = plotRect(this); const auto text = palette().color(QPalette::Text);
    p.setPen(barColor); p.setBrush(barColor); p.drawRect(QRectF(plot.left(), 12, 12, 10));
    p.drawText(QRectF(plot.left()+18, 5, 120, 24), "Daily playtime");
    p.setPen(QPen(lineColor, 2)); p.drawLine(QPointF(plot.left()+150, 17), QPointF(plot.left()+170, 17));
    p.drawText(QRectF(plot.left()+178, 5, 180, 24), "Cumulative playtime");
    const auto left = axisFor(days_, false), right = axisFor(days_, true);
    for (int tick = 0; tick <= 4; ++tick) {
        const auto y = plot.bottom() - plot.height()*tick/4;
        auto grid = text; grid.setAlpha(45); p.setPen(grid); p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(text);
        p.drawText(QRectF(29, y-10, 40, 20), Qt::AlignRight|Qt::AlignVCenter, QString::number(left.maximum*tick/4, 'g', 3));
        p.drawText(QRectF(plot.right()+7, y-10, 43, 20), Qt::AlignLeft|Qt::AlignVCenter, QString::number(right.maximum*tick/4, 'g', 3));
    }
    p.save(); p.translate(18, plot.center().y()); p.rotate(-90);
    p.drawText(QRectF(-plot.height()/2, -20, plot.height(), 40), Qt::AlignCenter, "Daily playtime\n(" + left.unit + ")"); p.restore();
    p.save(); p.translate(width()-20, plot.center().y()); p.rotate(90);
    p.drawText(QRectF(-plot.height()/2, -20, plot.height(), 40), Qt::AlignCenter, "Cumulative playtime\n(" + right.unit + ")"); p.restore();
    if (days_.isEmpty()) return;
    const auto step = plot.width()/days_.size(); QPainterPath line;
    for (int i = 0; i < days_.size(); ++i) {
        const auto &day = days_[i]; const auto x = plot.left() + step*(i+.5);
        const auto barHeight = plot.height()*day.foregroundMs/left.unitMs/left.maximum;
        p.fillRect(QRectF(x-step*.34, plot.bottom()-barHeight, step*.68, barHeight), barColor);
        const QPointF point(x, plot.bottom()-plot.height()*day.cumulativeMs/right.unitMs/right.maximum);
        if (i == 0) line.moveTo(point); else line.lineTo(point);
        if (i == 0 || i == days_.size()-1 || (i % 7 == 0 && i < days_.size()-3)) {
            p.setPen(text); p.drawText(QRectF(x-30, plot.bottom()+7, 60, 20), Qt::AlignCenter, day.day.toString("dd MMM"));
        }
    }
    p.setPen(QPen(lineColor, 2.5)); p.setBrush(Qt::NoBrush); p.drawPath(line);
    p.setPen(text); p.drawText(QRectF(plot.left(), height()-24, plot.width(), 20), Qt::AlignCenter, "Last 30 days");
}
bool PlaytimeChart::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip && !days_.isEmpty()) {
        auto help = static_cast<QHelpEvent *>(event); const auto plot = plotRect(this);
        if (plot.contains(help->pos())) {
            const auto i = std::clamp(int((help->pos().x()-plot.left())/plot.width()*days_.size()), 0, int(days_.size()-1));
            const auto &day = days_[i];
            QToolTip::showText(help->globalPos(), day.day.toString("dd-MM-yyyy") + "\nPlaytime: " + duration(day.foregroundMs) + "\nCumulative: " + duration(day.cumulativeMs), this);
        }
        return true;
    }
    return QWidget::event(event);
}
PlaytimeHeatmap::PlaytimeHeatmap(QWidget *parent) : QWidget(parent) {
    setObjectName("playtimeHeatmap"); setMinimumSize(360, 158);
    setAccessibleName("Foreground playtime activity heatmap over the last year");
}
void PlaytimeHeatmap::setDays(const QList<PlaytimeDay> &days) { days_ = days; update(); }
QRectF PlaytimeHeatmap::cellRect(QDate day) const {
    if (days_.isEmpty()) return {};
    const auto first = days_.first().day.addDays(1-days_.first().day.dayOfWeek());
    const auto columns = first.daysTo(days_.last().day)/7+1;
    const double step = std::min(15.0, std::max(1.0, (width()-53.0)/columns));
    const auto index = first.daysTo(day);
    return QRectF(43 + index/7*step, 32 + index%7*14, std::max(1.0, step-2), 12);
}
void PlaytimeHeatmap::paintEvent(QPaintEvent *) {
    QPainter p(this); const auto text = palette().color(QPalette::Text);
    p.setPen(text); p.drawText(QRect(4, 0, width()-8, 24), "Playtime activity · last year");
    for (int row : {0, 2, 4, 6}) p.drawText(QRect(0, 30+row*14, 36, 16), Qt::AlignRight|Qt::AlignVCenter, QStringList{"Mon","Tue","Wed","Thu","Fri","Sat","Sun"}[row]);
    qint64 maximum = 1; for (const auto &day : days_) maximum = std::max(maximum, day.foregroundMs);
    for (const auto &day : days_) {
        const auto rect = cellRect(day.day);
        QColor color = palette().color(QPalette::Mid); color.setAlpha(45);
        if (day.foregroundMs > 0) { color = green; color.setAlphaF(.3 + .7*std::min(1.0, double(day.foregroundMs)/maximum)); }
        p.fillRect(rect, color);
        if (day.day.day() == 1 && rect.left() < width()-28) { p.setPen(text); p.drawText(QRectF(rect.left(), 16, 40, 16), day.day.toString("MMM")); }
    }
    p.setPen(text); p.drawText(QRect(43,133,35,20), "Less");
    for (int level = 0; level < 5; ++level) {
        QColor color = level ? green : palette().color(QPalette::Mid);
        color.setAlphaF(level ? .25+.75*level/4 : .18);
        p.fillRect(QRect(80+level*15,137,12,12),color);
    }
    p.drawText(QRect(160,133,width()-170,20), "More · measured foreground time");
}
bool PlaytimeHeatmap::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        auto help = static_cast<QHelpEvent *>(event);
        for (const auto &day : days_) if (cellRect(day.day).contains(help->pos())) {
            QToolTip::showText(help->globalPos(), day.day.toString("dd-MM-yyyy") + "\nMeasured playtime: " + duration(day.foregroundMs), this); return true;
        }
    }
    return QWidget::event(event);
}
}
