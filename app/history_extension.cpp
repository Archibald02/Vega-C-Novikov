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
auto first_on_or_after(const QVector<Candle> &bars, const QDate &date) {
    return std::lower_bound(bars.cbegin(), bars.cend(), date,
                            [](const Candle &b, const QDate &d) { return date_of(b) < d; });
}
auto first_after(const QVector<Candle> &bars, const QDate &date) {
    return std::upper_bound(bars.cbegin(), bars.cend(), date,
                            [](const QDate &d, const Candle &b) { return d < date_of(b); });
}
} // namespace
bool MarketHistory::extend(const QString &key, const QDate &till) {
    return begin_extension(key, till, false);
}
bool MarketHistory::prepend(const QString &key, const QDate &from) {
    return begin_extension(key, from, true);
}
bool MarketHistory::begin_extension(const QString &key, const QDate &date, bool before) {
    if (active_)
        return false;
    const auto found = cache_.constFind(key);
    if (found == cache_.cend() || found->bars.isEmpty() || found->info.limited || !date.isValid() ||
        date.year() < 1990 || date.year() > 3000 ||
        (before ? date >= found->info.from : date <= found->info.till) ||
        (before ? date.daysTo(found->info.till) : found->info.from.daysTo(date)) > 7305) {
        emit failed(QStringLiteral("Для догрузки нужен полный RAM-набор и дата за выбранной границей "
                                   "(1990–3000, общий период до 20 лет)."));
        return false;
    }
    const auto base = *found;
    const auto target_from = before ? date : base.info.from;
    const auto target_till = before ? base.info.till : date;
    const auto checked_from = before ? date : std::max(base.info.from, date_of(base.bars.back()).addDays(-1));
    const auto checked_till = before ? std::min(base.info.till, date_of(base.bars.front()).addDays(1)) : date;
    const QString target_key = base.info.symbol + ":" + target_from.toString(Qt::ISODate) + ":" +
                               target_till.toString(Qt::ISODate) + ":" + QString::number(base.info.interval);
    load_range(base.info.symbol, checked_from, checked_till, base.info.interval, true, base.info.preset);
    if (!active_)
        return false;
    extension_ = std::make_shared<const Extension>(
        Extension{base, target_from, target_till, checked_from, checked_till, before, false, target_key});
    const bool reused =
        before ? date_of(base.bars.back()) > checked_till : date_of(base.bars.front()) < checked_from;
    if (reused)
        request_.started_ms = std::min(request_.started_ms, base.info.started_ms);
    const auto generation = generation_;
    emit state_message(QStringLiteral("Догружается %1: %2—%3; остальная часть берётся из RAM.")
                           .arg(before ? QStringLiteral("начало") : QStringLiteral("хвост"),
                                checked_from.toString(Qt::ISODate), checked_till.toString(Qt::ISODate)));
    if (generation != generation_ || !active_)
        return false;
    return true;
}
bool MarketHistory::recheck(const QString &key, const QDate &from, const QDate &till) {
    if (active_)
        return false;
    const auto found = cache_.constFind(key);
    if (found == cache_.cend() || found->info.limited || !from.isValid() || !till.isValid() || from > till ||
        from <= found->info.from || till >= found->info.till) {
        emit failed(QStringLiteral("Выберите полный RAM-набор и участок строго внутри его периода."));
        return false;
    }
    const auto base = *found;
    const auto first = first_on_or_after(base.bars, from), last = first_after(base.bars, till);
    if (first == base.bars.cbegin() || last == base.bars.cend()) {
        emit failed(QStringLiteral("Для проверки участка нужны принятые свечи до и после него. "
                                   "Используйте полную перепроверку периода."));
        return false;
    }
    const auto checked_from = date_of(*(first - 1)), checked_till = date_of(*last);
    load_range(base.info.symbol, checked_from, checked_till, base.info.interval, true, base.info.preset);
    if (!active_)
        return false;
    extension_ = std::make_shared<const Extension>(Extension{
        base, base.info.from, base.info.till, checked_from, checked_till, false, true, key, from, till});
    if (date_of(base.bars.front()) < checked_from || date_of(base.bars.back()) > checked_till)
        request_.started_ms = std::min(request_.started_ms, base.info.started_ms);
    const auto generation = generation_;
    emit state_message(QStringLiteral("Перепроверяется участок %1—%2 с опорными свечами по обе стороны.")
                           .arg(from.toString(Qt::ISODate), till.toString(Qt::ISODate)));
    return generation == generation_ && active_;
}
void MarketHistory::restart_extension(const QString &reason) {
    if (!extension_)
        return;
    const auto extension = extension_;
    const auto generation = generation_;
    emit state_message(reason + QStringLiteral(" Весь целевой период загружается заново."));
    if (generation != generation_ || !active_)
        return;
    checkpoint_ = {};
    extension_.reset();
    load_range(extension->base.info.symbol, extension->target_from, extension->target_till,
               extension->base.info.interval, true, extension->base.info.preset);
}
void MarketHistory::complete_extension() {
    const auto extension = extension_;
    if (!extension)
        return;
    const auto &base = extension->base;
    const auto first = first_on_or_after(base.bars, extension->checked_from);
    const auto last = first_after(base.bars, extension->checked_till);
    const auto fresh_first = first_on_or_after(accumulated_, base.info.from);
    const auto fresh_last = first_after(accumulated_, base.info.till);
    if (extension->rechecked && rejected_ > 0) {
        fail(QStringLiteral("Ответ участка содержит отклонённые строки. Прежний RAM-набор сохранён; "
                            "исправления не применены."),
             false);
        return;
    }
    bool overlap_matches = false;
    if (extension->rechecked) {
        overlap_matches =
            std::equal(first, first_on_or_after(base.bars, extension->replace_from), fresh_first,
                       first_on_or_after(accumulated_, extension->replace_from), identical) &&
            std::equal(first_after(base.bars, extension->replace_till), last,
                       first_after(accumulated_, extension->replace_till), fresh_last, identical);
    } else
        overlap_matches = std::equal(first, last, fresh_first, fresh_last, identical);
    if (!overlap_matches) {
        restart_extension(QStringLiteral("Пересечение с RAM-набором изменилось или неполно."));
        return;
    }
    const auto reused = extension->rechecked
                            ? (first - base.bars.cbegin()) + (base.bars.cend() - last)
                            : (extension->prepended ? base.bars.cend() - last : first - base.bars.cbegin());
    if (request_.limited || reused + accumulated_.size() > 5000) {
        restart_extension(QStringLiteral("Объединённый набор превышает предел догрузки."));
        return;
    }
    QVector<Candle> merged;
    merged.reserve(reused + accumulated_.size());
    if (!extension->prepended)
        for (auto i = base.bars.cbegin(); i != first; ++i)
            merged.push_back(*i);
    for (const auto &b : accumulated_)
        merged.push_back(b);
    if (extension->prepended || extension->rechecked)
        for (auto i = last; i != base.bars.cend(); ++i)
            merged.push_back(*i);
    accumulated_ = std::move(merged);
    checkpoint_ = {};
    extension_.reset();
    request_.key = extension->target_key;
    request_.from = extension->target_from;
    request_.till = extension->target_till;
    request_.extended = !extension->rechecked;
    request_.rechecked = extension->rechecked;
    request_.prepended = extension->prepended;
    request_.checked_from = extension->checked_from;
    request_.checked_till = extension->checked_till;
    request_.base_revision = base.info.revision;
    request_.reused_bars = static_cast<int>(reused);
    request_.limited = accumulated_.size() >= 5000;
    complete(base.info.symbol, base.info.preset, extension->target_key, &base);
}
} // namespace live
