#include "quote_parser.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace live {
namespace {
double number(const QJsonValue& value) {
    return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
}
bool positive(double x) { return std::isfinite(x) && x > 0 && x <= 1e12; }
bool valid_symbol(const QString& symbol) {
    static const QRegularExpression expression("^[A-Z0-9][A-Z0-9._-]{0,19}$");
    return expression.match(symbol).hasMatch();
}
void sanitize(Quote& q) { if (!positive(q.bid)) q.bid = NAN; if (!positive(q.ask)) q.ask = NAN; if (!std::isfinite(q.change_percent) || std::abs(q.change_percent) > 1e6) q.change_percent = NAN; }
}
std::optional<Quote> parse_alor(const QByteArray& payload) {
    if (payload.size() > 64 * 1024) return std::nullopt;
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
    if (!valid_symbol(q.symbol) || !positive(q.last) || !std::isfinite(timestamp)
        || timestamp <= 0 || timestamp > 32503680000.0) return std::nullopt;
    q.market_time_ms = static_cast<qint64>(timestamp * 1000);
    sanitize(q);
    return q;
}
QVector<Quote> parse_moex(const QByteArray& payload) {
    QVector<Quote> result;
    if (payload.size() > 1024 * 1024) return result;
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
        if (!valid_symbol(q.symbol) || !positive(q.last) || !date.isValid() || !moscow.isValid()) continue;
        q.market_time_ms = moscow.toMSecsSinceEpoch();
        if (moscow.date().year() < 1990 || moscow.date().year() > 3000) continue;
        sanitize(q); result.push_back(q); if (result.size() >= 5000) break;
    }
    return result;
}

QVector<Instrument> parse_catalog(const QByteArray& payload) {
    if (payload.size() > 1024 * 1024) return {};
    const auto block=QJsonDocument::fromJson(payload).object().value("securities").toObject();
    const auto columns=block.value("columns").toArray();
    auto index=[&columns](const QString& name) { for(int i=0;i<columns.size();++i) if(columns[i].toString()==name) return i; return -1; };
    const int code=index("SECID"), name=index("SHORTNAME"), lot=index("LOTSIZE"), status=index("STATUS");
    QVector<Instrument> result; QSet<QString> seen;
    if(code<0 || name<0 || lot<0 || status<0) return result;
    for(const auto& value:block.value("data").toArray()) {
        const auto row=value.toArray();
        auto at=[&row](int i) { return i>=0 && i<row.size() ? row[i] : QJsonValue(); };
        const auto symbol=at(code).toString(); const double size=at(lot).toDouble(0);
        if(at(status).toString()!="A" || !std::isfinite(size) || size<1 || size>1000000 || std::floor(size)!=size || seen.contains(symbol)) continue;
        try { if(parse_symbols(symbol).size()!=1 || parse_symbols(symbol).front()!=symbol) continue; } catch(...) { continue; }
        result.push_back({symbol,at(name).toString(symbol),static_cast<int>(size)}); seen.insert(symbol);
        if(result.size()>=5000) break;
    }
    std::sort(result.begin(),result.end(),[](const Instrument& a,const Instrument& b) {return a.symbol<b.symbol;});
    return result;
}
QStringList parse_symbols(const QString& input) {
    if (input.size() > 1024) throw std::invalid_argument("Ticker input too long");
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
