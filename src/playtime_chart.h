#pragma once
#include "playtime.h"
#include <QtWidgets/qwidget.h>
namespace novelty {
class PlaytimeChart final : public QWidget {
public:
    explicit PlaytimeChart(QWidget *parent = nullptr);
    void setDays(const QList<PlaytimeDay> &days);
    double dailyAxisHours() const;
    double cumulativeAxisHours() const;
protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *) override;
private:
    QList<PlaytimeDay> days_;
};
class PlaytimeHeatmap final : public QWidget {
public:
    explicit PlaytimeHeatmap(QWidget *parent = nullptr);
    void setDays(const QList<PlaytimeDay> &days);
protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *) override;
private:
    QList<PlaytimeDay> days_;
    QRectF cellRect(QDate day) const;
};
}
