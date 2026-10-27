#include "market_history.hpp"
#include "quote_parser.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>
#include <cmath>
namespace live {
QVector<Candle> parse_candles(const QJsonDocument& document) {
    QVector<Candle> bars;
    const auto block = document.object().value("candles").toObject();
    const auto cols = block.value("columns").toArray();
    const QStringList names{"begin", "open", "high", "low", "close", "volume"};
    QVector<int> positions;
    for (const auto& name : names) {
        int found = -1;
        for (int i = 0; i < cols.size(); ++i) if (cols[i].toString() == name) { found = i; break; }
        if (found < 0) return {};
        positions.push_back(found);
    }
    for (const auto& value : block.value("data").toArray()) {
        const auto row = value.toArray();
        auto at = [&](int index) { return positions[index] < row.size() ? row[positions[index]] : QJsonValue(); };
        Candle bar;
        const auto date = QDateTime::fromString(at(0).toString(), "yyyy-MM-dd HH:mm:ss");
        if (!date.isValid() || date.date().year() < 1990 || date.date().year() > 3000) continue;
        bar.time_ms = QDateTime(date.date(), date.time(), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
        double* fields[]{&bar.open, &bar.high, &bar.low, &bar.close, &bar.volume};
        bool valid = true;
        for (int i = 0; i < 5; ++i) {
            const auto v = at(i + 1);
            const double n = v.toDouble(NAN);
            if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > (i == 4 ? 1e18 : 1e12) || (i != 4 && n == 0)) valid = false;
            *fields[i] = n;
        }
        if (!valid || bar.high < std::max(bar.open, bar.close) || bar.low > std::min(bar.open, bar.close) || bar.low > bar.high) continue;
        if (!bars.isEmpty() && bar.time_ms <= bars.back().time_ms) continue;
        bars.push_back(bar);
        if (bars.size() >= 500) break;
    }
    return bars;
}
void MarketHistory::cancel() { ++generation_; if (reply_) { auto old = reply_; reply_.clear(); old->abort(); } }
void MarketHistory::clear() { cancel(); cache_.clear(); }
void MarketHistory::load(const QString& symbol, int preset, bool force) {
    cancel();
    try { if (parse_symbols(symbol) != QStringList{symbol} || preset < 0 || preset > 2) throw std::invalid_argument("History request"); }
    catch (...) { emit failed(QStringLiteral("Выберите корректный тикер и период.")); return; }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString key = symbol + ':' + QString::number(preset);
    const auto cached = cache_.constFind(key);
    if (!force && cached != cache_.cend() && now - cached->fetched_ms < 300000) { emit loaded(symbol, preset, cached->bars, true); return; }
    if (now < resume_ms_) { emit failed(QStringLiteral("Источник ограничил запросы. Повторите через %1 с.").arg((resume_ms_ - now + 999) / 1000)); return; }
    const int days = preset == 0 ? 90 : preset == 1 ? 365 : 7;
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    QUrl url = public_base_.resolved(QUrl(symbol + "/candles.json"));
    QUrlQuery query; query.addQueryItem("iss.meta", "off"); query.addQueryItem("iss.only", "candles");
    query.addQueryItem("from", today.addDays(-days).toString(Qt::ISODate)); query.addQueryItem("till", today.toString(Qt::ISODate));
    query.addQueryItem("interval", preset == 2 ? "60" : "24"); query.addQueryItem("start", "0"); query.addQueryItem("limit", "500");
    query.addQueryItem("candles.columns", "begin,open,high,low,close,volume"); url.setQuery(query);
    const auto generation = generation_;
    reply_ = request_json(network_, QNetworkRequest(url), this, [this, symbol, preset, key, generation](const HttpResult& result) {
        if (generation != generation_) return;
        reply_.clear();
        if (!result.ok()) {
            if (result.status == 429) resume_ms_ = QDateTime::currentMSecsSinceEpoch() + std::max(5, result.retry_after_seconds) * 1000;
            emit failed(http_error_text(result)); return;
        }
        auto bars = parse_candles(result.document);
        if (bars.isEmpty()) { emit failed(QStringLiteral("Нет доступных свечей за этот период. Источник мог ограничить публичную историю; регистрация в приложении не требуется.")); return; }
        if (cache_.size() >= 8 && !cache_.contains(key)) {
            auto oldest = cache_.begin();
            for (auto it = cache_.begin(); it != cache_.end(); ++it) if (it->fetched_ms < oldest->fetched_ms) oldest = it;
            cache_.erase(oldest);
        }
        cache_[key] = {bars, QDateTime::currentMSecsSinceEpoch()}; emit loaded(symbol, preset, bars, false);
    }, {}, {512 * 1024, 15000});
}
}
