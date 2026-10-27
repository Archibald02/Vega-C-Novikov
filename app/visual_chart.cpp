#include "visual_chart.hpp"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimeZone>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace live {
VisualChart::VisualChart(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(220);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}
void VisualChart::set_lines(const QVector<PlotLine> &lines, const QString &title, const QString &subtitle) {
    histogram_ = false;
    lines_ = lines;
    if (lines_.size() > 6)
        lines_.resize(6);
    for (auto &line : lines_)
        if (line.points.size() > 500)
            line.points.resize(500);
    bars_.clear();
    markers_.clear();
    title_ = title;
    subtitle_ = subtitle;
    hover_.clear();
    valid_ = false;
    update();
}
void VisualChart::set_histogram(const QVector<QPointF> &bins, const QString &title, const QString &subtitle) {
    set_lines({{QStringLiteral("Частота"), QColor("#39d2b0"), bins, false}}, title, subtitle);
    histogram_ = true;
    valid_ = false;
    update();
}
void VisualChart::set_candles(const QVector<Candle> &bars, const QString &title, const QString &subtitle) {
    bars_ = bars.mid(0, 5000);
    lines_.clear();
    title_ = title;
    subtitle_ = subtitle;
    hover_.clear();
    view_ = 100;
    valid_ = false;
    update();
}
void VisualChart::clear(const QString &message) {
    if (bars_.isEmpty() && lines_.isEmpty() && title_ == message && subtitle_.isEmpty())
        return;
    bars_.clear();
    lines_.clear();
    title_ = message;
    subtitle_.clear();
    hover_.clear();
    valid_ = false;
    update();
}
void VisualChart::resizeEvent(QResizeEvent *event) {
    valid_ = false;
    QWidget::resizeEvent(event);
}
void VisualChart::wheelEvent(QWheelEvent *event) {
    if (bars_.isEmpty()) {
        if (histogram_ || lines_.isEmpty()) {
            event->ignore();
            return;
        }
        const double width =
            std::clamp((upper_ - lower_) * (event->angleDelta().y() > 0 ? .8 : 1.25), .01, 1.);
        const double anchor =
            std::clamp((event->position().x() - price_area_.left()) / price_area_.width(), 0., 1.);
        const double start = std::clamp(lower_ + (upper_ - lower_) * anchor - width * anchor, 0., 1. - width);
        set_range(start, start + width);
        emit range_changed(lower_, upper_);
        event->accept();
        return;
    }
    view_ = std::clamp(static_cast<int>(view_ * (event->angleDelta().y() > 0 ? 0.8 : 1.25)), 20, 500);
    hover_.clear();
    valid_ = false;
    update();
    event->accept();
}
void VisualChart::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_ && bars_.isEmpty() && !histogram_) {
        const double delta =
            (drag_x_ - event->position().x()) / std::max(1., price_area_.width()) * (upper_ - lower_);
        const double width = upper_ - lower_, start = std::clamp(lower_ + delta, 0., 1. - width);
        drag_x_ = event->position().x();
        set_range(start, start + width);
        emit range_changed(lower_, upper_);
    }
    if (!price_area_.contains(event->position())) {
        leaveEvent(nullptr);
        return;
    }
    cursor_ = std::clamp((event->position().x() - price_area_.left()) / price_area_.width(), 0., 1.);
    emit cursor_changed(cursor_);
    if (bars_.isEmpty()) {
        const double x = xmin_ + cursor_ * (xmax_ - xmin_);
        hover_ = QStringLiteral("X %1").arg(x, 0, 'f', 2);
        for (const auto &line : lines_) {
            const QPointF *nearest = nullptr;
            for (const auto &pt : line.points)
                if (std::isfinite(pt.y()) && (!nearest || std::abs(pt.x() - x) < std::abs(nearest->x() - x)))
                    nearest = &pt;
            if (nearest)
                hover_ += QStringLiteral(" • %1 %2").arg(line.name).arg(nearest->y(), 0, 'f', 3);
            if (hover_.size() > 180)
                break;
        }
        update();
        return;
    }
    if (!shown_)
        return;
    const int i = std::clamp(static_cast<int>((event->position().x() - price_area_.left()) /
                                              price_area_.width() * shown_),
                             0, shown_ - 1) +
                  begin_;
    const auto &b = bars_[i];
    const QString text = QDateTime::fromMSecsSinceEpoch(b.time_ms)
                             .toTimeZone(QTimeZone("Europe/Moscow"))
                             .toString("dd.MM.yyyy HH:mm") +
                         QStringLiteral("  O %1  H %2  L %3  C %4  V %5")
                             .arg(b.open, 0, 'f', 2)
                             .arg(b.high, 0, 'f', 2)
                             .arg(b.low, 0, 'f', 2)
                             .arg(b.close, 0, 'f', 2)
                             .arg(b.volume, 0, 'f', 0);
    if (text != hover_) {
        hover_ = text;
        update();
    }
}
void VisualChart::leaveEvent(QEvent *) {
    if (!hover_.isEmpty()) {
        hover_.clear();
        cursor_ = -1;
        emit cursor_changed(-1);
        update();
    }
}
QString VisualChart::interaction_help() {
    return QStringLiteral(
        "Колесо: масштаб • перетаскивание: диапазон • двойной щелчок: весь ряд • курсор: значения");
}
void VisualChart::set_range(double lo, double hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo < 0 || hi > 1 || hi - lo < .009)
        return;
    lower_ = lo;
    upper_ = hi;
    valid_ = false;
    update();
}
void VisualChart::reset_view() {
    set_range(0, 1);
    emit range_changed(0, 1);
}
void VisualChart::focus_x(double x) {
    if (full_max_ <= full_min_)
        return;
    const double middle = std::clamp((x - full_min_) / (full_max_ - full_min_), 0., 1.);
    const double width = .15, start = std::clamp(middle - width / 2, 0., 1. - width);
    set_range(start, start + width);
    emit range_changed(lower_, upper_);
}
void VisualChart::set_markers(const QVector<QPointF> &markers) {
    markers_ = markers.mid(0, 1000);
    valid_ = false;
    update();
}
void VisualChart::set_cursor(double fraction) {
    cursor_ = fraction;
    update();
}
void VisualChart::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_x_ = e->position().x();
        e->accept();
    }
}
void VisualChart::mouseReleaseEvent(QMouseEvent *e) {
    dragging_ = false;
    e->accept();
}
void VisualChart::mouseDoubleClickEvent(QMouseEvent *e) {
    reset_view();
    e->accept();
}
void VisualChart::rebuild() {
    valid_ = true;
    cache_ = QPixmap((QSizeF(size()) * devicePixelRatioF()).toSize());
    cache_.setDevicePixelRatio(devicePixelRatioF());
    cache_.fill(QColor("#101c2b"));
    QPainter p(&cache_);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor("#e7f0fa"));
    auto title_font = font();
    title_font.setBold(true);
    p.setFont(title_font);
    p.drawText(QRect(16, 8, width() - 32, 24), Qt::AlignVCenter, title_);
    p.setFont(font());
    p.setPen(QColor("#91a7be"));
    p.drawText(QRect(16, 34, width() - 32, 22), Qt::AlignVCenter, subtitle_);
    price_area_ = QRectF(82, 76, std::max(10, width() - 108), std::max(20, height() - 118));
    const bool candle_mode = !bars_.isEmpty();
    if (!candle_mode && lines_.isEmpty())
        return;
    const int columns = std::clamp((width() - 100) / 170, 1, 6);
    const int legend_rows = (static_cast<int>(lines_.size()) + columns - 1) / columns;
    if (!candle_mode && legend_rows > 1) {
        price_area_.setTop(price_area_.top() + (legend_rows - 1) * 18);
    }
    double lo = INFINITY, hi = -INFINITY, xmin = INFINITY, xmax = -INFINITY;
    auto bound = [&](double v) {
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    };
    if (candle_mode) {
        shown_ = std::min(view_, static_cast<int>(bars_.size()));
        begin_ = static_cast<int>(bars_.size()) - shown_;
        for (int i = begin_; i < bars_.size(); ++i) {
            bound(bars_[i].low);
            bound(bars_[i].high);
        }
        price_area_.setHeight(std::max(20.0, price_area_.height() * 0.70));
    } else {
        for (const auto &line : lines_)
            for (const auto &pt : line.points)
                if (std::isfinite(pt.x()) && std::isfinite(pt.y())) {
                    bound(pt.y());
                    xmin = std::min(xmin, pt.x());
                    xmax = std::max(xmax, pt.x());
                }
    }
    if (!candle_mode && !histogram_ && std::isfinite(xmin) && std::isfinite(xmax)) {
        full_min_ = xmin;
        full_max_ = xmax;
        const double full_span = std::max(1., xmax - xmin);
        xmin = full_min_ + lower_ * full_span;
        xmax = full_min_ + upper_ * full_span;
        lo = INFINITY;
        hi = -INFINITY;
        for (const auto &line : lines_)
            for (const auto &pt : line.points)
                if (pt.x() >= xmin && pt.x() <= xmax)
                    bound(pt.y());
    }
    xmin_ = xmin;
    xmax_ = xmax;
    if (histogram_) {
        lo = 0;
        if (!lines_.isEmpty() && lines_[0].points.size() > 1) {
            const double half = (xmax - xmin) / (lines_[0].points.size() - 1) / 2;
            xmin -= half;
            xmax += half;
        }
    }
    if (!std::isfinite(lo) || !std::isfinite(hi))
        return;
    const double pad = std::max((hi - lo) * 0.12, std::max(std::abs(lo) * 0.0001, 0.01));
    if (!histogram_)
        lo -= pad;
    hi += pad;
    ymin_ = lo;
    ymax_ = hi;
    auto y = [&](double v) { return price_area_.bottom() - (v - lo) / (hi - lo) * price_area_.height(); };
    for (int i = 0; i <= 4; ++i) {
        const double v = lo + (hi - lo) * i / 4, yy = y(v);
        p.setPen(QColor("#26384b"));
        p.drawLine(QPointF(price_area_.left(), yy), QPointF(price_area_.right(), yy));
        p.setPen(QColor("#91a7be"));
        p.drawText(QRectF(0, yy - 10, 73, 20), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 2));
    }
    if (candle_mode) {
        const double step = price_area_.width() / shown_, body = std::clamp(step * 0.65, 1.0, 14.0);
        double max_volume = 1;
        for (int i = begin_; i < bars_.size(); ++i)
            max_volume = std::max(max_volume, bars_[i].volume);
        const QRectF volume(price_area_.left(), price_area_.bottom() + 22, price_area_.width(),
                            std::max(20.0, height() - price_area_.bottom() - 58));
        p.setPen(QColor("#91a7be"));
        p.drawText(QRectF(4, volume.top() - 5, 72, 20), Qt::AlignLeft, "Объём");
        p.save();
        p.setClipRect(price_area_.united(volume).adjusted(-2, -2, 2, 2));
        QPainterPath sma;
        bool seeded = false;
        double sum = 0;
        for (int i = 0; i < bars_.size(); ++i) {
            sum += bars_[i].close;
            if (i >= 20)
                sum -= bars_[i - 20].close;
            if (i < begin_)
                continue;
            const auto &b = bars_[i];
            const double xx = price_area_.left() + (i - begin_ + 0.5) * step;
            const QColor color(b.close >= b.open ? "#39d2b0" : "#f08c98");
            p.setPen(QPen(color, 1));
            p.drawLine(QPointF(xx, y(b.high)), QPointF(xx, y(b.low)));
            p.setBrush(color);
            p.drawRect(QRectF(xx - body / 2, std::min(y(b.open), y(b.close)), body,
                              std::max(1.0, std::abs(y(b.open) - y(b.close)))));
            auto tint = color;
            tint.setAlpha(100);
            p.fillRect(QRectF(xx - body / 2, volume.bottom() - b.volume / max_volume * volume.height(), body,
                              b.volume / max_volume * volume.height()),
                       tint);
            if (i >= 19) {
                const QPointF pt(xx, y(sum / 20));
                if (seeded)
                    sma.lineTo(pt);
                else {
                    sma.moveTo(pt);
                    seeded = true;
                }
            }
        }
        p.setPen(QPen(QColor("#efc571"), 1.7));
        p.setBrush(Qt::NoBrush);
        p.drawPath(sma);
        p.restore();
        p.setPen(QColor("#91a7be"));
        p.drawText(QRectF(price_area_.left(), height() - 28, 180, 20), Qt::AlignLeft,
                   QDateTime::fromMSecsSinceEpoch(bars_[begin_].time_ms)
                       .toTimeZone(QTimeZone("Europe/Moscow"))
                       .toString("dd.MM.yyyy HH:mm"));
        p.drawText(QRectF(price_area_.right() - 180, height() - 28, 180, 20), Qt::AlignRight,
                   QDateTime::fromMSecsSinceEpoch(bars_.back().time_ms)
                       .toTimeZone(QTimeZone("Europe/Moscow"))
                       .toString("dd.MM.yyyy HH:mm"));
    } else {
        const double span = std::max(1.0, xmax - xmin);
        int legend_index = 0;
        for (const auto &line : lines_) {
            p.setPen(line.color);
            p.drawText(
                QRect(84 + (legend_index % columns) * 170, 56 + (legend_index / columns) * 18, 170, 20),
                Qt::AlignLeft, line.name);
            ++legend_index;
            QPainterPath path;
            bool seeded = false;
            for (const auto &pt : line.points) {
                if (!std::isfinite(pt.x()) || !std::isfinite(pt.y())) {
                    seeded = false;
                    continue;
                }
                const QPointF q(price_area_.left() + (pt.x() - xmin) / span * price_area_.width(), y(pt.y()));
                if (seeded)
                    path.lineTo(q);
                else {
                    path.moveTo(q);
                    seeded = true;
                }
            }
            p.save();
            p.setClipRect(price_area_);
            p.setPen(QPen(line.color, 1.8, line.dashed ? Qt::DashLine : Qt::SolidLine));
            if (histogram_) {
                const double bar_width =
                    price_area_.width() / std::max<qsizetype>(1, line.points.size()) * .85;
                for (const auto &pt : line.points) {
                    const double xx = price_area_.left() + (pt.x() - xmin) / span * price_area_.width();
                    p.fillRect(
                        QRectF(xx - bar_width / 2, y(pt.y()), bar_width, std::max(1.0, y(0) - y(pt.y()))),
                        line.color);
                }
            } else
                p.drawPath(path);
            p.restore();
        }
        p.save();
        p.setClipRect(price_area_);
        for (const auto &marker : markers_)
            if (marker.x() >= xmin && marker.x() <= xmax && std::isfinite(marker.y())) {
                const QPointF q(price_area_.left() + (marker.x() - xmin) / span * price_area_.width(),
                                y(marker.y()));
                p.setPen(QColor("#ffb05b"));
                p.setBrush(QColor("#ffb05b"));
                p.drawEllipse(q, 4, 4);
            }
        p.restore();
        p.setPen(QColor("#91a7be"));
        p.drawText(QRectF(price_area_.left(), height() - 28, 80, 20), Qt::AlignLeft,
                   QString::number(xmin, 'f', 0));
        p.drawText(QRectF(price_area_.right() - 80, height() - 28, 80, 20), Qt::AlignRight,
                   QString::number(xmax, 'f', 0));
        p.drawText(QRectF(price_area_.left(), height() - 28, price_area_.width(), 20), Qt::AlignCenter,
                   histogram_ ? QStringLiteral("Доходность, % →")
                              : QStringLiteral("Наблюдения / время сессии →"));
    }
}
void VisualChart::paintEvent(QPaintEvent *) {
    if (!valid_ || cache_.deviceIndependentSize().toSize() != size() ||
        cache_.devicePixelRatio() != devicePixelRatioF())
        rebuild();
    QPainter p(this);
    p.drawPixmap(0, 0, cache_);
    if (cursor_ >= 0) {
        p.setPen(QPen(QColor("#91a7be"), 1, Qt::DashLine));
        const double x = price_area_.left() + cursor_ * price_area_.width();
        p.drawLine(QPointF(x, price_area_.top()), QPointF(x, price_area_.bottom()));
    }
    if (!hover_.isEmpty()) {
        p.fillRect(QRect(82, 77, width() - 110, 26), QColor("#193047"));
        p.setPen(QColor("#e7f0fa"));
        p.drawText(QRect(88, 77, width() - 122, 26), Qt::AlignVCenter, hover_);
    }
}
} // namespace live
