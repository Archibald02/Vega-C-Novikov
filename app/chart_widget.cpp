#include "chart_widget.hpp"
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
namespace live {
void ChartWidget::set_series(const std::deque<QPointF>& points, const QString& title, const QString& detail) {
    points_ = points; title_ = title; detail_ = detail; update();
}
void ChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#101c2b"));
    p.setPen(QColor("#eef5fc"));
    QFont title_font = font(); title_font.setPointSize(13); title_font.setBold(true); p.setFont(title_font);
    p.drawText(QRect(24,16,width()-48,28), Qt::AlignLeft|Qt::AlignVCenter, title_.isEmpty() ? QStringLiteral("График цены") : title_);
    p.setFont(font()); p.setPen(QColor("#91a7be"));
    p.drawText(QRect(24,48,width()-48,24), Qt::AlignLeft|Qt::AlignVCenter, detail_);
    if (points_.empty()) {
        p.drawText(rect().adjusted(30,70,-30,-30),Qt::AlignCenter,QStringLiteral("Ожидаем котировки\nГрафик появится при поступлении данных")); return;
    }
    QRectF plot(86,92,std::max(10,width()-116),std::max(10,height()-138));
    double lo = points_.front().y(), hi = lo;
    for (const auto& point : points_) { lo = std::min(lo,point.y()); hi = std::max(hi,point.y()); }
    const double pad = std::max((hi-lo)*0.1,std::max(std::abs(lo)*0.0001,0.01)); lo-=pad; hi+=pad;
    const double x0 = points_.front().x(), span = std::max(1.0,points_.back().x()-x0);
    for (int i=0;i<=4;++i) {
        const double y = plot.top()+i*plot.height()/4;
        p.setPen(QColor("#243449")); p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
        p.setPen(QColor("#91a7be"));
        p.drawText(QRectF(4,y-10,72,20),Qt::AlignRight|Qt::AlignVCenter,QString::number(hi-(hi-lo)*i/4,'f',2));
    }
    QPainterPath path;
    for (std::size_t i=0;i<points_.size();++i) {
        const QPointF q(plot.left()+(points_[i].x()-x0)/span*plot.width(),plot.bottom()-(points_[i].y()-lo)/(hi-lo)*plot.height());
        if (i==0) path.moveTo(q); else path.lineTo(q);
    }
    p.setClipRect(plot.adjusted(-2,-2,2,2));
    p.setPen(QPen(QColor("#38cfb0"),2.3)); p.drawPath(path);
    if (points_.size()==1) p.drawEllipse(QPointF(plot.left(),plot.center().y()),3,3);
    p.setClipping(false); p.setPen(QColor("#91a7be"));
    const auto start = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(points_.front().x())).toLocalTime().toString("HH:mm:ss");
    const auto end = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(points_.back().x())).toLocalTime().toString("HH:mm:ss");
    p.drawText(QRectF(plot.left(),plot.bottom()+12,180,22),Qt::AlignLeft,start);
    p.drawText(QRectF(plot.right()-180,plot.bottom()+12,180,22),Qt::AlignRight,end);
}
}
