#include "portfolio_live_feed.hpp"
#include <QTimeZone>
namespace live {
namespace {
const QTimeZone moscow("Europe/Moscow");
constexpr qint64 day = 86400000;
} // namespace
PortfolioLiveFeed::PortfolioLiveFeed(QObject *parent, QUrl base, std::function<qint64()> clock)
    : QObject(parent), loader_(nullptr, base), clock_(std::move(clock)) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
    connect(&loader_, &MarketHistory::failed, this, [this](const QString &) { failure(false); });
    connect(&loader_, &MarketHistory::truncated, this, [this](const QString &) { failure(true); });
    connect(&loader_, &MarketHistory::quality_report, this, [this](const QString &, int, int rejected) {
        if (rejected)
            failure(true);
    });
    connect(&loader_, &MarketHistory::loaded, this,
            [this](const QString &symbol, int, const QVector<Candle> &bars, bool) {
                if (!active_ || paused_ || !loading_ || !buffer_ || index_ >= cycle_.size() ||
                    symbol != QString::fromStdString(buffer_->symbols()[index_]))
                    return;
                for (const auto &c : bars) {
                    const auto t = QDateTime::fromMSecsSinceEpoch(c.time_ms, moscow);
                    const auto end = interval_ == 60 ? c.time_ms + 3600000 : t.addDays(1).toMSecsSinceEpoch();
                    if (end <= cutoff_)
                        cycle_[index_].push_back(
                            {c.time_ms, end, c.open, c.high, c.low, c.close, c.volume, t.offsetFromUtc()});
                }
                ++index_;
                if (index_ < cycle_.size()) {
                    const auto generation = generation_;
                    QTimer::singleShot(0, this, [this, generation] {
                        if (generation == generation_)
                            next();
                    });
                    return;
                }
                try {
                    const auto frames = buffer_->merge(cycle_, begin_, cutoff_);
                    cycle_.clear();
                    loading_ = false;
                    if (!frames.empty() && on_frames)
                        on_frames(frames);
                    if (active_ && on_snapshot)
                        on_snapshot(buffer_->snapshot());
                    if (active_ && !paused_ && on_status)
                        on_status(QStringLiteral(
                                      "MOEX • задержанные закрытые свечи. Последний общий конец UTC: %1. "
                                      "Следующий опрос не ранее чем через 60 с; ожидание не означает "
                                      "доступность новых интервалов.")
                                      .arg(QDateTime::fromMSecsSinceEpoch(buffer_->last_end())
                                               .toUTC()
                                               .toString(Qt::ISODate)));
                    schedule();
                } catch (const std::exception &) {
                    failure(true);
                }
            });
}
void PortfolioLiveFeed::start(std::unique_ptr<quant::PortfolioStreamBuffer> buffer, int interval) {
    stop();
    buffer_ = std::move(buffer);
    interval_ = interval;
    active_ = buffer_ && (interval == 60 || interval == 24);
    cooldown_.invalidate();
    refresh();
}
void PortfolioLiveFeed::stop() {
    active_ = paused_ = loading_ = false;
    ++generation_;
    timer_.stop();
    loader_.clear();
    cycle_.clear();
    buffer_.reset();
}
void PortfolioLiveFeed::pause(bool paused) {
    if (!active_)
        return;
    paused_ = paused;
    ++generation_;
    timer_.stop();
    loader_.cancel();
    cycle_.clear();
    loading_ = false;
    if (!paused_)
        refresh();
}
void PortfolioLiveFeed::schedule() {
    if (active_ && !paused_)
        timer_.start(60000);
}
void PortfolioLiveFeed::refresh() {
    if (!active_ || paused_ || loading_ || !buffer_)
        return;
    if (cooldown_.isValid() && cooldown_.elapsed() < 60000) {
        timer_.start(static_cast<int>(qMax<qint64>(1, 60000 - cooldown_.elapsed())));
        return;
    }
    const auto now = clock_();
    cutoff_ = now - 20 * 60000;
    if (buffer_->last_end() > cutoff_ || now - buffer_->last_end() > 7 * day) {
        failure(true);
        return;
    }
    begin_ = QDateTime::fromMSecsSinceEpoch(buffer_->last_time(), moscow)
                 .date()
                 .addDays(-1)
                 .startOfDay(moscow)
                 .toMSecsSinceEpoch();
    cycle_.assign(buffer_->symbols().size(), {});
    index_ = 0;
    loading_ = true;
    cooldown_.start();
    next();
}
void PortfolioLiveFeed::next() {
    if (!active_ || paused_ || !loading_ || !buffer_)
        return;
    loader_.load_range(QString::fromStdString(buffer_->symbols()[index_]),
                       QDateTime::fromMSecsSinceEpoch(begin_, moscow).date(),
                       QDateTime::fromMSecsSinceEpoch(clock_(), moscow).date(), interval_, true);
}
void PortfolioLiveFeed::failure(bool fatal) {
    if (!active_ || paused_)
        return;
    ++generation_;
    loader_.cancel();
    loading_ = false;
    cycle_.clear();
    if (fatal) {
        stop();
        if (on_fatal)
            on_fatal(QStringLiteral("Поток остановлен: закрытая история изменилась/неполна, данные "
                                    "отклонены, превышен лимит либо общая свеча старше 7 дней. Счёт сохранён "
                                    "в RAM, позиции не проданы. "
                                    "Проверьте источник и запустите новый сеанс после завершения текущего."));
    } else {
        if (on_status)
            on_status(QStringLiteral("MOEX временно недоступен. Цикл не применён; счёт не изменён. Повтор не "
                                     "ранее чем через 60 с."));
        schedule();
    }
}
} // namespace live
