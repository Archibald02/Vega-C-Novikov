#include "quote_parser.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace live {
namespace {
double number(const QJsonValue& value) {
    return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
}
bool positive(double x) { return std::isfinite(x) && x > 0; }
}
std::optional<Quote> parse_alor(const QByteArray& payload) {
    const auto doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return std::nullopt;
    const auto data = doc.object().value("data").toObject();
    Quote q;
    q.symbol = data.value("symbol").toString();
    q.last = number(data.value("last_price"));
    q.bid = number(data.value("bid"));
    q.ask = number(data.value("ask"));
    q.change_percent = number(data.value("change_percent"));
    const double timestamp = number(data.value("last_price_timestamp"));
    if (q.symbol.isEmpty() || !positive(q.last) || !std::isfinite(timestamp)
        || timestamp <= 0 || timestamp > 32503680000.0) return std::nullopt;
    q.market_time_ms = static_cast<qint64>(timestamp * 1000);
    return q;
}
QVector<Quote> parse_moex(const QByteArray& payload) {
    QVector<Quote> result;
    const auto block = QJsonDocument::fromJson(payload).object().value("marketdata").toObject();
    const auto cols = block.value("columns").toArray();
    auto col = [&cols](const QString& key) {
        for (int i = 0; i < cols.size(); ++i) if (cols[i].toString() == key) return i;
        return -1;
    };
    const int symbol = col("SECID"), last = col("LAST"), bid = col("BID"), ask = col("OFFER");
    const int change = col("LASTTOPREVPRICE"), systime = col("SYSTIME");
    if (symbol < 0 || last < 0 || systime < 0) return result;
    for (const auto& v : block.value("data").toArray()) {
        const auto row = v.toArray();
        auto at = [&row](int i) { return i >= 0 && i < row.size() ? row[i] : QJsonValue(); };
        Quote q;
        q.symbol = at(symbol).toString(); q.last = number(at(last));
        q.bid = number(at(bid)); q.ask = number(at(ask)); q.change_percent = number(at(change));
        // SYSTIME is the ISS snapshot time in Moscow. It is not a trade timestamp.
        const auto date = QDateTime::fromString(at(systime).toString(), "yyyy-MM-dd HH:mm:ss");
        const QDateTime moscow(date.date(), date.time(), QTimeZone("Europe/Moscow"));
        if (q.symbol.isEmpty() || !positive(q.last) || !date.isValid() || !moscow.isValid()) continue;
        q.market_time_ms = moscow.toMSecsSinceEpoch();
        result.push_back(q);
    }
    return result;
}
QStringList parse_symbols(const QString& input) {
    const auto parts = input.toUpper().split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    QStringList result; QSet<QString> seen;
    const QRegularExpression valid("^[A-Z0-9][A-Z0-9._-]{0,19}$");
    for (const auto& part : parts) {
        if (!valid.match(part).hasMatch()) throw std::invalid_argument("Invalid ticker");
        if (!seen.contains(part)) { seen.insert(part); result.push_back(part); }
    }
    if (result.isEmpty() || result.size() > 20) throw std::invalid_argument("Need 1 to 20 tickers");
    return result;
}
}
