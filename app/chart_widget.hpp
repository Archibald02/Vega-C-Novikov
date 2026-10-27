#pragma once
#include <QWidget>
#include <QPointF>
#include <deque>
namespace live {
/** Lightweight bounded line chart. X coordinates are provider timestamps. */
class ChartWidget : public QWidget {
public:
    explicit ChartWidget(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(270); }
    void set_series(const std::deque<QPointF>& points, const QString& title, const QString& detail);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    std::deque<QPointF> points_;
    QString title_, detail_;
};
}
