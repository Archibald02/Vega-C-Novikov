#include "market_history.hpp"
#include "quote_parser.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <cmath>
namespace live {
QVector<Candle> parse_candles(const QJsonDocument &document) {
    QVector<Candle> bars;
    const auto block = document.object().value("candles").toObject();
    const auto cols = block.value("columns").toArray();
    const QStringList names{"begin", "open", "high", "low", "close", "volume"};
    QVector<int> positions;
    for (const auto &name : names) {
        int found = -1;
        for (int i = 0; i < cols.size(); ++i)
            if (cols[i].toString() == name) {
                found = i;
                break;
            }
        if (found < 0)
            return {};
        positions.push_back(found);
    }
    for (const auto &value : block.value("data").toArray()) {
        const auto row = value.toArray();
        auto at = [&](int index) {
            return positions[index] < row.size() ? row[positions[index]] : QJsonValue();
        };
        Candle bar;
        const auto date = QDateTime::fromString(at(0).toString(), "yyyy-MM-dd HH:mm:ss");
        if (!date.isValid() || date.date().year() < 1990 || date.date().year() > 3000)
            continue;
        bar.time_ms = QDateTime(date.date(), date.time(), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
        double *fields[]{&bar.open, &bar.high, &bar.low, &bar.close, &bar.volume};
        bool valid = true;
        for (int i = 0; i < 5; ++i) {
            const auto v = at(i + 1);
            const double n = v.toDouble(NAN);
            if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > (i == 4 ? 1e18 : 1e12) ||
                (i != 4 && n == 0))
                valid = false;
            *fields[i] = n;
        }
        if (!valid || bar.high < std::max(bar.open, bar.close) || bar.low > std::min(bar.open, bar.close) ||
            bar.low > bar.high)
            continue;
        if (!bars.isEmpty() && bar.time_ms <= bars.back().time_ms)
            continue;
        bars.push_back(bar);
        if (bars.size() >= 500)
            break;
    }
    return bars;
}
void MarketHistory::cancel() {
    ++generation_;
    active_ = false;
    accumulated_.clear();
    if (reply_) {
        auto old = reply_;
        reply_.clear();
        old->abort();
    }
}
void MarketHistory::clear() {
    cancel();
    cache_.clear();
}
void MarketHistory::load(const QString &symbol, int preset, bool force) {
    if (preset < 0 || preset > 2) {
        cancel();
        emit failed(QStringLiteral("Некорректный период."));
        return;
    }
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    const int days = preset == 0 ? 90 : preset == 1 ? 365 : 7;
    load_range(symbol, today.addDays(-days), today, preset == 2 ? 60 : 24, force, preset);
}
void MarketHistory::load_range(const QString &symbol, const QDate &from, const QDate &till, int interval,
                               bool force, int result_preset) {
    cancel();
    try {
        if (parse_symbols(symbol) != QStringList{symbol} || !from.isValid() || !till.isValid() ||
            from.year() < 1990 || till.year() > 3000 || from > till || from.daysTo(till) > 7305 ||
            (interval != 24 && interval != 60))
            throw std::invalid_argument("range");
    } catch (...) {
        emit failed(QStringLiteral("Проверьте тикер, даты (не более 20 лет) и интервал."));
        return;
    }
    const int preset = result_preset >= 0 ? result_preset : (interval == 60 ? 2 : 1);
    const QString key = symbol + ":" + from.toString(Qt::ISODate) + ":" + till.toString(Qt::ISODate) + ":" +
                        QString::number(interval);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto cached = cache_.constFind(key);
    if (!force && cached != cache_.cend() && now - cached->fetched_ms < 300000) {
        emit quality_report(symbol, cached->received, cached->rejected);
        emit loaded(symbol, preset, cached->bars, true);
        return;
    }
    if (now < resume_ms_) {
        emit failed(QStringLiteral("Лимит API; повторите через %1 с.").arg((resume_ms_ - now + 999) / 1000));
        return;
    }
    active_ = true;
    received_ = rejected_ = 0;
    page(symbol, preset, key, from, till, interval, generation_, 0);
}
void MarketHistory::complete(const QString &symbol, int preset, const QString &key) {
    active_ = false;
    if (accumulated_.isEmpty()) {
        emit failed(QStringLiteral("Нет доступной публичной истории за выбранный период."));
        return;
    }
    if (cache_.size() >= 8 && !cache_.contains(key)) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->fetched_ms < oldest->fetched_ms)
                oldest = it;
        cache_.erase(oldest);
    }
    cache_[key] = {accumulated_, QDateTime::currentMSecsSinceEpoch(), received_, rejected_};
    const auto bars = accumulated_;
    accumulated_.clear();
    emit quality_report(symbol, received_, rejected_);
    emit loaded(symbol, preset, bars, false);
}
void MarketHistory::page(const QString &symbol, int preset, const QString &key, QDate from, QDate till,
                         int interval, quint64 generation, int offset) {
    if (generation != generation_)
        return;
    QUrl url = public_base_.resolved(QUrl(symbol + "/candles.json"));
    QUrlQuery query;
    query.addQueryItem("iss.meta", "off");
    query.addQueryItem("iss.only", "candles");
    query.addQueryItem("from", from.toString(Qt::ISODate));
    query.addQueryItem("till", till.toString(Qt::ISODate));
    query.addQueryItem("interval", QString::number(interval));
    query.addQueryItem("start", QString::number(offset));
    query.addQueryItem("limit", "500");
    query.addQueryItem("candles.columns", "begin,open,high,low,close,volume");
    url.setQuery(query);
    reply_ = request_json(
        network_, QNetworkRequest(url), this,
        [this, symbol, preset, key, from, till, interval, generation, offset](const HttpResult &result) {
            if (generation != generation_)
                return;
            reply_.clear();
            if (!result.ok()) {
                active_ = false;
                accumulated_.clear();
                if (result.status == 429)
                    resume_ms_ =
                        QDateTime::currentMSecsSinceEpoch() + std::max(5, result.retry_after_seconds) * 1000;
                emit failed(http_error_text(result));
                return;
            }
            const auto block = result.document.object().value("candles").toObject();
            if (!block.value("data").isArray() || !block.value("columns").isArray()) {
                active_ = false;
                accumulated_.clear();
                emit failed(QStringLiteral("Некорректная схема свечей API."));
                return;
            }
            const int raw = block.value("data").toArray().size();
            auto bars = parse_candles(result.document);
            received_ += raw;
            rejected_ += raw - static_cast<int>(bars.size());
            const auto before = accumulated_.size();
            for (const auto &bar : bars) {
                const auto date =
                    QDateTime::fromMSecsSinceEpoch(bar.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).date();
                if (date < from || date > till ||
                    (!accumulated_.isEmpty() && bar.time_ms <= accumulated_.back().time_ms)) {
                    ++rejected_;
                    continue;
                }
                if (accumulated_.size() < 5000)
                    accumulated_.push_back(bar);
            }
            emit progress(symbol, accumulated_.size(), 5000);
            if (accumulated_.size() >= 5000) {
                emit truncated(symbol);
                complete(symbol, preset, key);
                return;
            }
            if (raw == 0 || raw < 500) {
                complete(symbol, preset, key);
                return;
            }
            if (accumulated_.size() == before || offset >= 5000) {
                active_ = false;
                accumulated_.clear();
                emit failed(
                    QStringLiteral("API не продвигает страницу; загрузка остановлена без зацикливания."));
                return;
            }
            QTimer::singleShot(150, this, [=] {
                page(symbol, preset, key, from, till, interval, generation, offset + raw);
            });
        },
        {}, {512 * 1024, 15000});
}
} // namespace live
