#include "chart_widget.hpp"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace live {
ChartWidget::ChartWidget(QWidget* parent):QWidget(parent) {setMinimumHeight(290);setMouseTracking(true);setAttribute(Qt::WA_OpaquePaintEvent);}
void ChartWidget::set_series(const ChartSeries* series,const QString& title,const QString& detail,std::uint64_t revision) {
    if(series_==series && revision_==revision && title_==title && detail_==detail) return;
    series_=series;title_=title;detail_=detail;revision_=revision;hover_index_=-1;valid_=false;update();
}
void ChartWidget::set_options(bool sma,bool ema,bool bands,int oscillator) {
    if(sma_==sma && ema_==ema && bands_==bands && oscillator_==oscillator) return;
    sma_=sma;ema_=ema;bands_=bands;oscillator_=oscillator;valid_=false;update();
}
void ChartWidget::reset_view() {view_limit_=500;valid_=false;update();}
void ChartWidget::resizeEvent(QResizeEvent* event) {valid_=false;QWidget::resizeEvent(event);}
void ChartWidget::wheelEvent(QWheelEvent* event) {
    view_limit_=std::clamp(static_cast<int>(view_limit_*(event->angleDelta().y()>0?0.8:1.25)),30,5000);
    hover_index_=-1;valid_=false;update();event->accept();
}
void ChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if(!series_ || !count_ || !price_plot_.contains(event->position())) {if(hover_index_!=-1){hover_index_=-1;update();}return;}
    const double target=x0_+(event->position().x()-price_plot_.left())/price_plot_.width()*span_;
    std::size_t left=begin_,right=begin_+count_;
    while(left<right){const auto mid=(left+right)/2;if((*series_)[mid].time_ms<target)left=mid+1;else right=mid;}
    const auto idx=std::min(left,begin_+count_-1);
    if(hover_index_!=static_cast<int>(idx)){hover_index_=static_cast<int>(idx);update();}
}
void ChartWidget::leaveEvent(QEvent*) {hover_index_=-1;update();}
void ChartWidget::rebuild() {
    ++rebuild_count_;valid_=true;cache_=QPixmap((QSizeF(size())*devicePixelRatioF()).toSize());cache_.setDevicePixelRatio(devicePixelRatioF());cache_.fill(QColor("#101c2b"));
    QPainter p(&cache_);p.setRenderHint(QPainter::Antialiasing);
    QFont title=font();title.setPointSize(12);title.setBold(true);p.setFont(title);p.setPen(QColor("#eef5fc"));
    p.drawText(QRect(18,10,width()-36,26),Qt::AlignVCenter,title_.isEmpty()?QStringLiteral("График цены"):title_);
    p.setFont(font());p.setPen(QColor("#91a7be"));p.drawText(QRect(18,38,width()-36,24),Qt::AlignVCenter,detail_);
    count_=series_?std::min<std::size_t>(series_->size(),view_limit_):0;
    if(!count_) {p.drawText(rect().adjusted(20,60,-20,-20),Qt::AlignCenter,QStringLiteral("Ожидаем котировки • колесо мыши меняет масштаб"));return;}
    begin_=series_->size()-count_;low_=high_=(*series_)[begin_].price;
    for(std::size_t i=begin_;i<series_->size();++i) {
        const auto& s=(*series_)[i];
        auto bounds=[this](double v){if(std::isfinite(v)){low_=std::min(low_,v);high_=std::max(high_,v);}};
        bounds(s.price);if(sma_)bounds(s.indicators.sma);if(ema_)bounds(s.indicators.ema);
        if(bands_){bounds(s.indicators.lower);bounds(s.indicators.upper);}
    }
    const auto pad=std::max((high_-low_)*0.1,std::max(std::abs(low_)*0.0001,0.01));low_-=pad;high_+=pad;
    const int bottom=height()-34;const int osc_height=oscillator_?std::max(58,(height()-110)/4):0;
    price_plot_=QRectF(82,80,std::max(10,width()-106),std::max(30,bottom-80-osc_height-(oscillator_?24:0)));
    osc_plot_=QRectF(price_plot_.left(),price_plot_.bottom()+24,price_plot_.width(),osc_height);
    x0_=(*series_)[begin_].time_ms;span_=std::max(1.0,static_cast<double>(series_->back().time_ms)-x0_);
    auto x=[this](const ChartSample& s){return price_plot_.left()+(s.time_ms-x0_)/span_*price_plot_.width();};
    for(int i=0;i<=4;++i) {
        const double y=price_plot_.top()+i*price_plot_.height()/4;
        p.setPen(QColor("#243449"));p.drawLine(QPointF(price_plot_.left(),y),QPointF(price_plot_.right(),y));
        p.setPen(QColor("#91a7be"));p.drawText(QRectF(0,y-10,72,20),Qt::AlignRight|Qt::AlignVCenter,QString::number(high_-(high_-low_)*i/4,'f',2));
    }
    auto line=[&](auto value,const QColor& color,const QRectF& plot,double lo,double hi) {
        QPainterPath path;bool started=false;
        for(std::size_t i=begin_;i<series_->size();++i) {
            const auto& s=(*series_)[i];const double v=value(s);
            if(!std::isfinite(v)){started=false;continue;}
            const QPointF q(x(s),plot.bottom()-(v-lo)/(hi-lo)*plot.height());
            if(started)path.lineTo(q);else{path.moveTo(q);started=true;}
        }
        p.save();p.setClipRect(plot.adjusted(-1,-1,1,1));p.setPen(QPen(color,1.5));p.drawPath(path);p.restore();
    };
    if(bands_) {
        line([](const ChartSample&s){return s.indicators.upper;},QColor("#626d94"),price_plot_,low_,high_);
        line([](const ChartSample&s){return s.indicators.lower;},QColor("#626d94"),price_plot_,low_,high_);
    }
    if(sma_)line([](const ChartSample&s){return s.indicators.sma;},QColor("#ecbd69"),price_plot_,low_,high_);
    if(ema_)line([](const ChartSample&s){return s.indicators.ema;},QColor("#829eff"),price_plot_,low_,high_);
    line([](const ChartSample&s){return s.price;},QColor("#38cfb0"),price_plot_,low_,high_);
    if(count_==1){p.setPen(QColor("#38cfb0"));p.drawEllipse(QPointF(price_plot_.left(),price_plot_.center().y()),3,3);}
    p.setPen(QColor("#91a7be"));
    if(oscillator_) {
        p.drawText(QRectF(4,osc_plot_.top()-16,76,20),Qt::AlignLeft,oscillator_==1?"RSI 14":"MACD");
        if(oscillator_==1) {
            p.setPen(QPen(QColor("#4b4c58"),1,Qt::DashLine));
            for(int level:{30,70}){const double y=osc_plot_.bottom()-level*osc_plot_.height()/100;p.drawLine(QPointF(osc_plot_.left(),y),QPointF(osc_plot_.right(),y));}
            line([](const ChartSample&s){return s.indicators.rsi;},QColor("#bca2f2"),osc_plot_,0,100);
        } else {
            double lo=0,hi=0;
            for(std::size_t i=begin_;i<series_->size();++i)for(double v:{(*series_)[i].indicators.macd,(*series_)[i].indicators.signal})if(std::isfinite(v)){lo=std::min(lo,v);hi=std::max(hi,v);}
            const double d=std::max(hi-lo,0.001)*0.15;lo-=d;hi+=d;
            line([](const ChartSample&s){return s.indicators.macd;},QColor("#829eff"),osc_plot_,lo,hi);
            line([](const ChartSample&s){return s.indicators.signal;},QColor("#ecbd69"),osc_plot_,lo,hi);
        }
    }
    p.setPen(QColor("#91a7be"));
    p.drawText(QRectF(price_plot_.left(),bottom+8,160,22),Qt::AlignLeft,QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(x0_)).toLocalTime().toString("HH:mm:ss"));
    p.drawText(QRectF(price_plot_.right()-180,bottom+8,180,22),Qt::AlignRight,QDateTime::fromMSecsSinceEpoch(series_->back().time_ms).toLocalTime().toString("HH:mm:ss"));
}
void ChartWidget::paintEvent(QPaintEvent*) {
    if(!valid_ || cache_.deviceIndependentSize().toSize()!=size() || cache_.devicePixelRatio()!=devicePixelRatioF())rebuild();
    QPainter p(this);p.drawPixmap(0,0,cache_);
    if(!series_ || hover_index_<0 || static_cast<std::size_t>(hover_index_)>=series_->size())return;
    const auto& s=(*series_)[static_cast<std::size_t>(hover_index_)];
    const double x=price_plot_.left()+(s.time_ms-x0_)/span_*price_plot_.width();
    p.setPen(QPen(QColor("#8aa0b6"),1,Qt::DashLine));p.drawLine(QPointF(x,price_plot_.top()),QPointF(x,price_plot_.bottom()));
    p.fillRect(QRectF(price_plot_.left()+4,price_plot_.top()+4,260,24),QColor("#182c42"));p.setPen(QColor("#e9f0fa"));
    p.drawText(QRectF(price_plot_.left()+10,price_plot_.top()+4,245,24),Qt::AlignVCenter,QDateTime::fromMSecsSinceEpoch(s.time_ms).toLocalTime().toString("HH:mm:ss")+" • "+QString::number(s.price,'f',2)+QStringLiteral(" ₽"));
}
}
