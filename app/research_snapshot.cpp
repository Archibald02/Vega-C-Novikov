#include "research_snapshot.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace live {
namespace {
constexpr auto engine = "qpl-single-backtest-v3";
void require(bool condition) {
    if (!condition)
        throw std::invalid_argument("Snapshot schema/data");
}
double number(const QJsonValue &v) {
    require(v.isDouble() && std::isfinite(v.toDouble()));
    return v.toDouble();
}
int integer(const QJsonValue &v) {
    const auto n = number(v);
    require(std::floor(n) == n && std::abs(n) <= 1000000000);
    return static_cast<int>(n);
}
bool boolean(const QJsonValue &v) {
    require(v.isBool());
    return v.toBool();
}
QJsonArray bars_json(const std::vector<quant::Bar> &bars) {
    QJsonArray out;
    for (const auto &b : bars)
        out.append(QJsonArray{static_cast<double>(b.time), static_cast<double>(b.end), b.open, b.high, b.low,
                              b.close, b.volume, b.utc_offset_seconds});
    return out;
}
QString hash_bars(const QJsonArray &bars) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(bars).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
QJsonObject group_json(const quant::RuleGroup &g) {
    QJsonArray rules;
    QJsonArray expression;
    for (int token : g.expression)
        expression.append(token);
    for (const auto &r : g.rules)
        rules.append(QJsonArray{static_cast<int>(r.lhs), static_cast<int>(r.rhs), static_cast<int>(r.op),
                                r.value, r.enabled});
    return {{"all", g.all}, {"rules", rules}, {"expression", expression}};
}
quant::RuleGroup group(const QJsonValue &v, bool with_expression) {
    require(v.isObject());
    const auto o = v.toObject();
    require(o.keys() ==
            (with_expression ? QStringList({"all", "expression", "rules"}) : QStringList({"all", "rules"})));
    quant::RuleGroup g;
    g.all = boolean(o.value("all"));
    require(o.value("rules").isArray());
    const auto list = o.value("rules").toArray();
    require(list.size() <= 4);
    for (const auto &item : list) {
        require(item.isArray());
        const auto a = item.toArray();
        require(a.size() == 5);
        const int lhs = integer(a[0]), rhs = integer(a[1]), op = integer(a[2]);
        require(lhs >= 0 && lhs < static_cast<int>(quant::Feature::Count) && rhs >= 0 &&
                rhs < static_cast<int>(quant::Feature::Count) && op >= 0 && op <= 3);
        g.rules.push_back({static_cast<quant::Feature>(lhs), static_cast<quant::Feature>(rhs),
                           static_cast<quant::Compare>(op), number(a[3]), boolean(a[4])});
    }
    if (with_expression) {
        require(o["expression"].isArray() && o["expression"].toArray().size() <= 7);
        for (const auto &token : o["expression"].toArray()) {
            const auto value = integer(token);
            require(value >= -2 && value <= 3);
            g.expression.push_back(static_cast<int>(value));
        }
    }
    require(quant::valid_rule_expression(g));
    return g;
}
QJsonObject config_json(const quant::SimConfig &c) {
    return {{"fast", c.strategy.fast},
            {"slow", c.strategy.slow},
            {"window", c.strategy.indicator_window},
            {"entry", group_json(c.strategy.entry)},
            {"exit", group_json(c.strategy.exit)},
            {"buy_hold", c.strategy.buy_hold},
            {"cash", c.initial_cash},
            {"commission", c.commission_rate},
            {"fixed", c.fixed_fee},
            {"spread", c.spread_bps},
            {"slippage", c.slippage_bps},
            {"allocation", c.allocation},
            {"stop", c.stop_fraction},
            {"target", c.target_fraction},
            {"risk", c.risk_per_trade},
            {"drawdown", c.max_drawdown},
            {"daily", c.daily_loss},
            {"tick", c.tick_size},
            {"delay", c.signal_delay},
            {"entry_confirmation", c.entry_confirmation},
            {"exit_confirmation", c.exit_confirmation},
            {"cooldown_bars", c.cooldown_bars},
            {"max_holding_bars", c.max_holding_bars},
            {"participation", c.participation},
            {"lot", static_cast<double>(c.lot)},
            {"max_entries", c.max_entries_per_day},
            {"annual", c.annual_periods},
            {"pessimistic", c.pessimistic},
            {"liquidate", c.liquidate_end}};
}
quant::SimConfig config(const QJsonValue &v, int schema) {
    const bool legacy = schema == 1;
    require(v.isObject());
    const auto o = v.toObject();
    quant::SimConfig c;
    auto expected = config_json(c);
    if (legacy)
        for (const auto *key :
             {"entry_confirmation", "exit_confirmation", "cooldown_bars", "max_holding_bars"})
            expected.remove(key);
    require(o.keys() == expected.keys());
    c.strategy.fast = integer(o["fast"]);
    c.strategy.slow = integer(o["slow"]);
    c.strategy.indicator_window = integer(o["window"]);
    require(c.strategy.fast >= 2 && c.strategy.fast < c.strategy.slow && c.strategy.slow <= 500 &&
            c.strategy.indicator_window >= 2 && c.strategy.indicator_window <= 500);
    c.strategy.entry = group(o["entry"], schema >= 3);
    c.strategy.exit = group(o["exit"], schema >= 3);
    require(!legacy || (c.strategy.entry.rules.size() <= 2 && c.strategy.exit.rules.size() <= 2));
    c.strategy.buy_hold = boolean(o["buy_hold"]);
    c.initial_cash = number(o["cash"]);
    c.commission_rate = number(o["commission"]);
    c.fixed_fee = number(o["fixed"]);
    c.spread_bps = number(o["spread"]);
    c.slippage_bps = number(o["slippage"]);
    c.allocation = number(o["allocation"]);
    c.stop_fraction = number(o["stop"]);
    c.target_fraction = number(o["target"]);
    c.risk_per_trade = number(o["risk"]);
    c.max_drawdown = number(o["drawdown"]);
    c.daily_loss = number(o["daily"]);
    c.tick_size = number(o["tick"]);
    c.signal_delay = integer(o["delay"]);
    if (!legacy) {
        c.entry_confirmation = integer(o["entry_confirmation"]);
        c.exit_confirmation = integer(o["exit_confirmation"]);
        c.cooldown_bars = integer(o["cooldown_bars"]);
        c.max_holding_bars = integer(o["max_holding_bars"]);
    }
    c.participation = number(o["participation"]);
    c.lot = integer(o["lot"]);
    c.max_entries_per_day = integer(o["max_entries"]);
    c.annual_periods = integer(o["annual"]);
    c.pessimistic = boolean(o["pessimistic"]);
    c.liquidate_end = boolean(o["liquidate"]);
    quant::PaperEngine validate(c);
    return c;
}
QJsonArray doubles(const std::vector<double> &values) {
    QJsonArray a;
    for (double v : values)
        a.append(std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null));
    return a;
}
QJsonArray trace_json(const std::vector<quant::ExecutionTrace> &trace) {
    QJsonArray a;
    for (const auto &t : trace) {
        require(t.fill_count >= 0 && t.fill_count <= static_cast<int>(t.fills.size()));
        QJsonArray fills;
        for (int i = 0; i < t.fill_count; ++i) {
            const auto &f = t.fills[i];
            fills.append(QJsonArray{QString::number(f.requested), QString::number(f.quantity),
                                    QString::number(f.units_after), f.trigger, f.price, f.fee, f.cost,
                                    f.cash_after, QString::fromStdString(f.reason)});
        }
        a.append(QJsonArray{QString::number(t.time), QString::number(t.units_before),
                            QString::number(t.units_after), t.cash_before, t.cash_after, t.equity,
                            t.entry_condition, t.exit_condition, t.pending_entry, t.pending_exit,
                            t.entry_streak, t.exit_streak, t.held_bars, t.cooldown_remaining, t.entry_wait,
                            t.exit_wait, QString::fromStdString(t.gate), fills});
    }
    return a;
}
QJsonObject result_json(const quant::SimResult &r) {
    const auto &m = r.metrics;
    QJsonArray journal, times;
    for (const auto &e : r.journal)
        journal.append(QJsonArray{QString::number(e.time), QString::number(e.quantity), e.price, e.cash,
                                  e.equity, QString::fromStdString(e.reason)});
    for (auto t : r.times)
        times.append(QString::number(t));
    QJsonObject object{
        {"equity", doubles(r.equity)},
        {"drawdowns", doubles(r.drawdowns)},
        {"times", times},
        {"journal", journal},
        {"metrics", doubles({m.total_return, m.max_drawdown, m.turnover, m.sharpe, m.sortino, m.cagr,
                             m.calmar, m.profit_factor, m.win_rate, m.fees, m.execution_cost})},
        {"fills", m.fills},
        {"closed_trades", m.closed_trades},
        {"cash", r.cash},
        {"unrealized", r.unrealized},
        {"units", QString::number(r.units)},
        {"start", QString::number(r.start_time)},
        {"halted", r.halted},
        {"halt_reason", QString::fromStdString(r.halt_reason)}};
    if (!r.trace.empty())
        object.insert("trace", trace_json(r.trace));
    return object;
}
bool same(const QJsonValue &a, const QJsonValue &b) {
    if (a.type() != b.type())
        return false;
    if (a.isDouble())
        return std::isfinite(a.toDouble()) && std::isfinite(b.toDouble()) &&
               std::abs(a.toDouble() - b.toDouble()) <=
                   1e-7 + 1e-9 * std::max(std::abs(a.toDouble()), std::abs(b.toDouble()));
    if (a.isArray()) {
        const auto x = a.toArray(), y = b.toArray();
        if (x.size() != y.size())
            return false;
        for (int i = 0; i < x.size(); ++i)
            if (!same(x[i], y[i]))
                return false;
        return true;
    }
    if (a.isObject()) {
        const auto x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys())
            return false;
        for (auto i = x.begin(); i != x.end(); ++i) {
            if (i.key() == "fills" || i.key() == "closed_trades") {
                if (i.value() != y.value(i.key()))
                    return false;
            } else if (!same(i.value(), y.value(i.key())))
                return false;
        }
        return true;
    }
    return a == b;
}
} // namespace
QByteArray encode_snapshot(const ReplaySnapshot &s, const QString &source, const QString &name,
                           const QString &hypothesis) {
    require(!s.bars.empty() && s.bars.size() <= 5000 && s.reference.journal.size() <= 10000 &&
            s.reference.equity.size() <= 5001 && s.reference.drawdowns.size() <= 5001 &&
            s.reference.times.size() <= 5000 && s.reference.trace.size() <= 5000);
    require(source.size() <= 256 && !name.isEmpty() && name.size() <= 120 && hypothesis.size() <= 2000);
    quant::validate_bars(s.bars);
    const auto bars = bars_json(s.bars);
    const auto bytes = QJsonDocument(QJsonObject{{"format", "QuantPriceLiveSnapshot"},
                                                 {"schema", 3},
                                                 {"engine", engine},
                                                 {"source", source},
                                                 {"name", name},
                                                 {"hypothesis", hypothesis},
                                                 {"bars", bars},
                                                 {"sha256", hash_bars(bars)},
                                                 {"config", config_json(s.config)},
                                                 {"reference", result_json(s.reference)}})
                           .toJson(QJsonDocument::Compact);
    require(bytes.size() <= snapshot_byte_limit);
    return bytes;
}
QString verify_snapshot(const QByteArray &bytes, const std::atomic<bool> &cancelled) {
    require(bytes.size() <= snapshot_byte_limit);
    if (cancelled.load())
        return QStringLiteral("Проверка отменена.");
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject());
    const auto root = doc.object();
    const bool legacy = root["schema"] == 1 && root["engine"] == "qpl-single-backtest-v1";
    const bool legacy_two = root["schema"] == 2 && root["engine"] == "qpl-single-backtest-v2";
    require(root.size() == 10 && root["format"] == "QuantPriceLiveSnapshot" &&
            (legacy || legacy_two || (root["schema"] == 3 && root["engine"] == engine)));
    for (const auto &key : {"name", "hypothesis", "source"})
        require(root[key].isString());
    require(!root["name"].toString().isEmpty() && root["name"].toString().size() <= 120 &&
            root["hypothesis"].toString().size() <= 2000 && root["source"].toString().size() <= 256);
    require(root["bars"].isArray());
    const auto list = root["bars"].toArray();
    require(!list.isEmpty() && list.size() <= 5000);
    std::vector<quant::Bar> bars;
    bars.reserve(list.size());
    for (const auto &v : list) {
        if (cancelled.load())
            return QStringLiteral("Проверка отменена.");
        require(v.isArray());
        const auto a = v.toArray();
        require(a.size() == 8);
        for (const auto &n : a)
            number(n);
        const double t = a[0].toDouble(), end = a[1].toDouble();
        require(t >= 1 && t <= 32503680000000. && end >= 1 && end <= 32503852800000. && std::floor(t) == t &&
                std::floor(end) == end);
        const auto offset = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(t))
                                .toTimeZone(QTimeZone("Europe/Moscow"))
                                .offsetFromUtc();
        require(a[7].toDouble() == offset);
        bars.push_back({static_cast<qint64>(t), static_cast<qint64>(end), a[2].toDouble(), a[3].toDouble(),
                        a[4].toDouble(), a[5].toDouble(), a[6].toDouble(), offset});
    }
    quant::validate_bars(bars);
    require(root["sha256"].isString() && root["sha256"].toString() == hash_bars(bars_json(bars)));
    const auto c = config(root["config"], root["schema"].toInt());
    if (cancelled.load())
        return QStringLiteral("Проверка отменена.");
    require(root["reference"].isObject());
    const bool capture_trace = root["reference"].toObject().contains("trace");
    require(!capture_trace || (!legacy && root["reference"].toObject()["trace"].isArray() &&
                               root["reference"].toObject()["trace"].toArray().size() <= 5000));
    const auto actual = quant::backtest(bars, c, 0, capture_trace);
    if (cancelled.load())
        return QStringLiteral("Проверка отменена.");
    require(root["reference"].isObject());
    if (!same(root["reference"], result_json(actual)))
        return QStringLiteral("НЕ СОВПАЛО: эталон и повторный бэктест различаются. Проверьте версию модели и "
                              "файл. Данные и настройки рабочего исследования не изменены.");
    return QStringLiteral("СОВПАЛО: %1 свечей, %2 исполнений. Капитал, просадка, метрики и журнал "
                          "воспроизведены (допуск 1e-9 относительный + 1e-7 абсолютный). Проверена "
                          "согласованность файла, не подлинность его автора. Это исходный бэктест; "
                          "walk-forward и holdout отдельно не повторялись.")
        .arg(bars.size())
        .arg(actual.metrics.fills);
}
} // namespace live
