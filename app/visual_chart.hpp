#pragma once
#include "market_history.hpp"
#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QWidget>
namespace live {
struct PlotLine { QString name; QColor color; QVector<QPointF> points; bool dashed = false; };
/** Cached plots for true candles/volume and bounded analytical line series. */
class VisualChart : public QWidget {
public:
    explicit VisualChart(QWidget* parent = nullptr);
    void set_lines(const QVector<PlotLine>& lines, const QString& title, const QString& subtitle);
    void set_histogram(const QVector<QPointF>& bins, const QString& title, const QString& subtitle);
    void set_candles(const QVector<Candle>& bars, const QString& title, const QString& subtitle);
    void clear(const QString& message = QStringLiteral("Данных пока нет"));
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    void rebuild();
    QVector<PlotLine> lines_;
    QVector<Candle> bars_;
    QString title_, subtitle_, hover_;
    QPixmap cache_;
    QRectF price_area_;
    bool valid_ = false, histogram_ = false;
    int view_ = 100, begin_ = 0, shown_ = 0;
};
}
