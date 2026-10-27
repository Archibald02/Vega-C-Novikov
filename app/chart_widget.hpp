#pragma once
#include "quant/indicators.hpp"
#include <QPixmap>
#include <QWidget>
#include <cstdint>
namespace live {
/** One bounded-history observation and precomputed indicators. */
struct ChartSample { std::int64_t time_ms=0; double price=0; quant::IndicatorValues indicators; };
using ChartSeries=quant::RingBuffer<ChartSample>;
/** Cached raster chart: no history copies, repaint only on revision/viewport changes. */
class ChartWidget : public QWidget {
public:
    explicit ChartWidget(QWidget* parent=nullptr);
    void set_series(const ChartSeries* series,const QString& title,const QString& detail,std::uint64_t revision);
    void set_options(bool sma,bool ema,bool bands,int oscillator);
    void reset_view();
    std::uint64_t rebuild_count() const {return rebuild_count_;}
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    void rebuild();
    const ChartSeries* series_=nullptr;
    QString title_,detail_;
    std::uint64_t revision_=0,rebuild_count_=0;
    bool sma_=true,ema_=false,bands_=false,valid_=false;
    int oscillator_=1,view_limit_=500,hover_index_=-1;
    std::size_t begin_=0,count_=0;
    QRectF price_plot_,osc_plot_;
    double low_=0,high_=1,x0_=0,span_=1;
    QPixmap cache_;
};
}
