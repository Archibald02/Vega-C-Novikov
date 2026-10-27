#pragma once
#include "market_history.hpp"
#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QWidget>
namespace live {
struct PlotLine {
    QString name;
    QColor color;
    QVector<QPointF> points;
    bool dashed = false;
};
/** Cached plots for true candles/volume and bounded analytical line series. */
class VisualChart : public QWidget {
    Q_OBJECT
  public:
    explicit VisualChart(QWidget *parent = nullptr);
    void set_lines(const QVector<PlotLine> &lines, const QString &title, const QString &subtitle);
    void set_histogram(const QVector<QPointF> &bins, const QString &title, const QString &subtitle);
    void set_candles(const QVector<Candle> &bars, const QString &title, const QString &subtitle);
    void set_range(double lower, double upper);
    void focus_x(double x);
    void set_markers(const QVector<QPointF> &markers);
    void set_cursor(double fraction);
    void reset_view();
    static QString interaction_help();
    void clear(const QString &message = QStringLiteral("Данных пока нет"));
  signals:
    void range_changed(double lower, double upper);
    void cursor_changed(double fraction);

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

  private:
    void rebuild();
    double lower_ = 0, upper_ = 1, xmin_ = 0, xmax_ = 1, ymin_ = 0, ymax_ = 1;
    double full_min_ = 0, full_max_ = 1, cursor_ = -1, drag_x_ = 0;
    bool dragging_ = false;
    QVector<QPointF> markers_;
    QVector<PlotLine> lines_;
    QVector<Candle> bars_;
    QString title_, subtitle_, hover_;
    QPixmap cache_;
    QRectF price_area_;
    bool valid_ = false, histogram_ = false;
    int view_ = 100, begin_ = 0, shown_ = 0;
};
} // namespace live
