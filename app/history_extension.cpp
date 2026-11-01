#include "market_history.hpp"
#include <QTimeZone>
#include <algorithm>
namespace live {
namespace {
QDate date_of(const Candle &b) {
    return QDateTime::fromMSecsSinceEpoch(b.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).date();
}
bool identical(const Candle &a, const Candle &b) {
    return a.time_ms == b.time_ms && a.open == b.open && a.high == b.high && a.low == b.low &&
           a.close == b.close && a.volume == b.volume;
}
} // namespace
bool MarketHistory::extend(const QString &key, const QDate &till) {
    if (active_)
        return false;
    const auto found = cache_.constFind(key);
    if (found == cache_.cend() || found->bars.isEmpty() || found->info.limited || !till.isValid() ||
        till <= found->info.till || till.year() > 3000 || found->info.from.daysTo(till) > 7305) {
        emit failed(QStringLiteral(
            "Для догрузки нужен полный RAM-набор и более поздняя конечная дата (общий период до 20 лет)."));
        return false;
    }
    const auto base = *found;
    const auto overlap = std::max(base.info.from, date_of(base.bars.back()).addDays(-1));
    const QString target_key = base.info.symbol + ":" + base.info.from.toString(Qt::ISODate) + ":" +
                               till.toString(Qt::ISODate) + ":" + QString::number(base.info.interval);
    load_range(base.info.symbol, overlap, till, base.info.interval, true, base.info.preset);
    if (!active_)
        return false;
    extension_ = std::make_shared<const Extension>(Extension{base, till, overlap, target_key});
    if (date_of(base.bars.front()) < overlap)
        request_.started_ms = std::min(request_.started_ms, base.info.started_ms);
    const auto generation = generation_;
    emit state_message(QStringLiteral("Догружается хвост с %1; ранняя часть берётся из RAM.")
                           .arg(overlap.toString(Qt::ISODate)));
    if (generation != generation_ || !active_)
        return false;
    return true;
}
void MarketHistory::restart_extension(const QString &reason) {
    if (!extension_)
        return;
    const auto extension = extension_;
    const auto generation = generation_;
    emit state_message(reason + QStringLiteral(" Весь расширенный период загружается заново."));
    if (generation != generation_ || !active_)
        return;
    checkpoint_ = {};
    extension_.reset();
    load_range(extension->base.info.symbol, extension->base.info.from, extension->target_till,
               extension->base.info.interval, true, extension->base.info.preset);
}
void MarketHistory::complete_extension() {
    const auto extension = extension_;
    if (!extension)
        return;
    const auto &base = extension->base;
    auto first = std::lower_bound(base.bars.cbegin(), base.bars.cend(), extension->checked_from,
                                  [](const Candle &b, const QDate &date) { return date_of(b) < date; });
    const auto prefix = static_cast<qsizetype>(first - base.bars.cbegin());
    auto old = first;
    qsizetype received = 0;
    while (received < accumulated_.size() && date_of(accumulated_[received]) <= base.info.till) {
        if (old == base.bars.cend() || !identical(*old, accumulated_[received])) {
            restart_extension(QStringLiteral("Пересечение с RAM-набором изменилось."));
            return;
        }
        ++old;
        ++received;
    }
    if (old != base.bars.cend()) {
        restart_extension(QStringLiteral("В пересечении не хватает старых свечей."));
        return;
    }
    if (request_.limited || prefix + accumulated_.size() > 5000) {
        restart_extension(QStringLiteral("Объединённый набор превышает предел догрузки."));
        return;
    }
    QVector<Candle> merged;
    merged.reserve(prefix + accumulated_.size());
    for (auto i = base.bars.cbegin(); i != first; ++i)
        merged.push_back(*i);
    for (const auto &b : accumulated_)
        merged.push_back(b);
    accumulated_ = std::move(merged);
    checkpoint_ = {};
    extension_.reset();
    request_.key = extension->target_key;
    request_.from = base.info.from;
    request_.extended = true;
    request_.checked_from = extension->checked_from;
    request_.base_revision = base.info.revision;
    request_.reused_bars = static_cast<int>(prefix);
    request_.limited = accumulated_.size() >= 5000;
    complete(base.info.symbol, base.info.preset, extension->target_key, &base);
}
} // namespace live
