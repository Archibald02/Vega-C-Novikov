#include "portfolio_account_file.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace live {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument("Portfolio account schema/replay");
}
void check(const std::atomic<bool> &c) { require(!c.load()); }
double number(const QJsonValue &v, double lo, double hi) {
    require(v.isDouble());
    auto n = v.toDouble();
    require(std::isfinite(n) && n >= lo && n <= hi);
    return n;
}
qint64 integer(const QJsonValue &v, qint64 lo, qint64 hi) {
    auto n = number(v, static_cast<double>(lo), static_cast<double>(hi));
    require(std::floor(n) == n);
    return static_cast<qint64>(n);
}
bool boolean(const QJsonValue &v) {
    require(v.isBool());
    return v.toBool();
}
QJsonObject object(const QJsonValue &v, QStringList keys) {
    require(v.isObject());
    keys.sort();
    auto o = v.toObject();
    require(o.keys() == keys);
    return o;
}
QString hash(const QJsonObject &o) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
            .toHex());
}
QJsonArray bar_json(const quant::Bar &b) {
    return {static_cast<double>(b.time), static_cast<double>(b.end), b.open, b.high, b.low, b.close, b.volume,
            b.utc_offset_seconds};
}
quant::Bar bar(const QJsonValue &v) {
    require(v.isArray());
    auto a = v.toArray();
    require(a.size() == 8);
    quant::Bar b;
    b.time = integer(a[0], 1, 32503680000000LL);
    b.end = integer(a[1], 1, 32503852800000LL);
    b.open = number(a[2], 1e-9, 1e12);
    b.high = number(a[3], 1e-9, 1e12);
    b.low = number(a[4], 1e-9, 1e12);
    b.close = number(a[5], 1e-9, 1e12);
    b.volume = number(a[6], 0, 1e18);
    b.utc_offset_seconds = static_cast<int>(integer(a[7], -50400, 50400));
    require(b.utc_offset_seconds ==
            QDateTime::fromMSecsSinceEpoch(b.time, QTimeZone("Europe/Moscow")).offsetFromUtc());
    quant::validate_bars({b});
    return b;
}
QJsonArray frames_json(const std::vector<std::vector<quant::Bar>> &frames) {
    QJsonArray out;
    for (const auto &f : frames) {
        QJsonArray row;
        for (const auto &b : f)
            row.append(bar_json(b));
        out.append(row);
    }
    return out;
}
std::vector<std::vector<quant::Bar>> frames(const QJsonValue &v, std::size_t count, int limit,
                                            const std::atomic<bool> &cancel) {
    require(v.isArray());
    auto a = v.toArray();
    require(a.size() <= limit);
    std::vector<std::vector<quant::Bar>> out;
    for (const auto &r : a) {
        check(cancel);
        require(r.isArray());
        auto row = r.toArray();
        require(row.size() == static_cast<qsizetype>(count));
        std::vector<quant::Bar> f;
        for (const auto &b : row)
            f.push_back(bar(b));
        out.push_back(std::move(f));
    }
    return out;
}
QJsonObject audit_json(const quant::PortfolioStreamState &s) {
    QJsonArray symbols, series;
    for (const auto &v : s.symbols)
        symbols.append(QString::fromStdString(v));
    for (const auto &list : s.series) {
        QJsonArray a;
        for (const auto &b : list)
            a.append(bar_json(b));
        series.append(a);
    }
    return {{"symbols", symbols},
            {"series", series},
            {"watermark", static_cast<double>(s.watermark)},
            {"end", static_cast<double>(s.end)},
            {"coverage", static_cast<double>(s.coverage)},
            {"interval", s.interval}};
}
quant::PortfolioStreamState audit(const QJsonValue &v, const std::atomic<bool> &cancel) {
    auto o = object(v, {"symbols", "series", "watermark", "end", "coverage", "interval"});
    quant::PortfolioStreamState s;
    require(o["symbols"].isArray() && o["series"].isArray());
    auto syms = o["symbols"].toArray(), rows = o["series"].toArray();
    require(syms.size() >= 1 && syms.size() <= 6 && syms.size() == rows.size());
    for (const auto &x : syms) {
        require(x.isString() && !x.toString().isEmpty() && x.toString().toUtf8().size() <= 32);
        s.symbols.push_back(x.toString().toStdString());
    }
    for (const auto &r : rows) {
        check(cancel);
        require(r.isArray());
        auto a = r.toArray();
        require(!a.isEmpty() && a.size() <= 512);
        std::vector<quant::Bar> list;
        for (const auto &x : a)
            list.push_back(bar(x));
        s.series.push_back(std::move(list));
    }
    s.watermark = integer(o["watermark"], 1, 32503680000000LL);
    s.end = integer(o["end"], 1, 32503852800000LL);
    s.coverage = integer(o["coverage"], 1, s.watermark);
    s.interval = static_cast<int>(integer(o["interval"], 24, 60));
    quant::PortfolioStreamBuffer::restore(s);
    return s;
}
QJsonArray doubles(const std::vector<double> &v) {
    QJsonArray a;
    for (auto x : v) {
        require(std::isfinite(x));
        a.append(x);
    }
    return a;
}
QJsonObject result_json(const quant::PortfolioResult &r) {
    require(r.equity.size() <= 5001 && r.drawdowns.size() <= 5001 && r.times.size() <= 5000 &&
            r.positions.size() <= 6 && r.excluded.size() <= 6 && r.journal.size() <= 10000);
    QJsonArray times, positions, journal, excluded;
    for (auto t : r.times)
        times.append(QString::number(t));
    for (auto n : r.excluded)
        excluded.append(QString::number(n));
    for (const auto &p : r.positions)
        positions.append(QJsonArray{QString::fromStdString(p.symbol), QString::number(p.units), p.value,
                                    p.pnl, p.fees, p.rules_ready});
    for (const auto &e : r.journal)
        journal.append(QJsonArray{QString::number(e.time), QString::number(e.quantity),
                                  QString::fromStdString(e.symbol), QString::fromStdString(e.reason), e.price,
                                  e.fee, e.cash});
    return {{"equity", doubles(r.equity)},
            {"drawdowns", doubles(r.drawdowns)},
            {"times", times},
            {"positions", positions},
            {"journal", journal},
            {"excluded", excluded},
            {"cash", r.cash},
            {"return", r.total_return},
            {"max_drawdown", r.max_drawdown},
            {"fees", r.fees},
            {"execution", r.execution_cost},
            {"turnover", r.turnover},
            {"count", QString::number(r.common_bars)},
            {"dropped", QString::number(r.dropped_events)},
            {"start", QString::number(r.start_time)},
            {"fills", QString::number(r.fills)},
            {"halted", r.halted},
            {"reason", QString::fromStdString(r.halt_reason)}};
}
bool same(const QJsonValue &a, const QJsonValue &b) {
    if (a.type() != b.type())
        return false;
    if (a.isDouble())
        return std::isfinite(a.toDouble()) && std::isfinite(b.toDouble()) &&
               std::abs(a.toDouble() - b.toDouble()) <=
                   1e-7 + 1e-9 * std::max(std::abs(a.toDouble()), std::abs(b.toDouble()));
    if (a.isArray()) {
        auto x = a.toArray(), y = b.toArray();
        if (x.size() != y.size())
            return false;
        for (int i = 0; i < x.size(); ++i)
            if (!same(x[i], y[i]))
                return false;
        return true;
    }
    if (a.isObject()) {
        auto x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys())
            return false;
        for (auto i = x.begin(); i != x.end(); ++i)
            if (!same(i.value(), y.value(i.key())))
                return false;
        return true;
    }
    return a == b;
}
QJsonObject references(const quant::PortfolioResult &a, const quant::PortfolioResult &b) {
    return {{"account", result_json(a)}, {"baseline", result_json(b)}};
}
} // namespace
PortfolioAccountRestore replay_portfolio_account(const PortfolioAccountSnapshot &s,
                                                 const std::atomic<bool> &cancel) {
    check(cancel);
    require(s.frames.size() <= 5000 && s.index <= 5000 && s.stop_after >= -1 &&
            s.stop_after <= static_cast<int>(s.index));
    require(s.stop_after < 0 ? s.stop_reason.empty()
                             : (s.stop_reason == "MANUAL_STOP" || s.stop_reason == "DATA_ERROR" ||
                                s.stop_reason == "SESSION_LIMIT"));
    auto assets = portfolio_project_assets(s.project);
    require(!assets.empty() && assets.size() <= 6);
    auto initial = assets;
    if (s.live) {
        require(!s.project.input.synthetic);
        require((s.interval == 60 || s.interval == 24) && s.warmup_end > 0 &&
                s.warmup_cutoff >= s.warmup_end && s.audit.has_value() && s.index == s.frames.size());
        for (auto &a : initial) {
            require(a.tick_size > 0);
            a.bars.erase(std::remove_if(a.bars.begin(), a.bars.end(),
                                        [&](const auto &b) { return b.end > s.warmup_cutoff; }),
                         a.bars.end());
        }
    } else
        require(s.interval == 0 && !s.audit && s.frames.empty() && s.warmup_cutoff == 0 && s.warmup_end == 0);
    auto schedule = quant::portfolio_schedule(initial);
    require(s.excluded == schedule.excluded);
    PortfolioAccountRestore out;
    out.account = std::make_unique<quant::PortfolioAccount>(assets, s.project.config);
    out.baseline = std::make_unique<quant::PortfolioAccount>(
        quant::portfolio_baseline_account(assets, s.project.config));
    if (s.live) {
        require(schedule.frames.size() >= 40 && schedule.frames.back()[0].end == s.warmup_end);
        for (const auto &f : schedule.frames) {
            check(cancel);
            out.account->warmup(f);
            out.baseline->warmup(f);
        }
        out.account->arm_from_warmup();
        out.baseline->arm_from_warmup();
    } else
        require(s.index <= schedule.frames.size());
    for (std::size_t i = 0; i <= s.index; ++i) {
        check(cancel);
        if (s.stop_after == static_cast<int>(i))
            out.account->request_stop(s.stop_reason);
        if (i == s.index)
            break;
        const auto &f = s.live ? s.frames[i] : schedule.frames[i];
        if (s.live)
            for (const auto &b : f)
                require(
                    b.end - b.time == (s.interval == 60 ? 3600000LL : 86400000LL) &&
                    b.utc_offset_seconds ==
                        QDateTime::fromMSecsSinceEpoch(b.time, QTimeZone("Europe/Moscow")).offsetFromUtc());
        out.account->step(f);
        out.baseline->step(f);
    }
    if (s.live) {
        const auto &last = s.frames.empty() ? schedule.frames.back()[0] : s.frames.back()[0];
        const auto &a = *s.audit;
        require(a.watermark == last.time && a.end == last.end && a.interval == s.interval &&
                a.symbols.size() == assets.size());
        for (std::size_t i = 0; i < assets.size(); ++i)
            require(a.symbols[i] == assets[i].symbol);
        quant::PortfolioStreamBuffer::restore(a);
        std::vector<std::map<std::int64_t, quant::Bar>> history(assets.size());
        for (std::size_t j = 0; j < initial.size(); ++j)
            for (const auto &b : initial[j].bars)
                history[j].emplace(b.time, b);
        for (const auto &f : s.frames)
            for (std::size_t j = 0; j < f.size(); ++j) {
                const auto old = history[j].find(f[j].time);
                require(old == history[j].end() || bar_json(old->second) == bar_json(f[j]));
                history[j][f[j].time] = f[j];
            }
        for (std::size_t j = 0; j < a.series.size(); ++j)
            for (const auto &b : a.series[j]) {
                require(b.utc_offset_seconds ==
                        QDateTime::fromMSecsSinceEpoch(b.time, QTimeZone("Europe/Moscow")).offsetFromUtc());
                const auto old = history[j].find(b.time);
                require(old == history[j].end() || bar_json(old->second) == bar_json(b));
            }
        schedule.frames.clear();
        schedule.frames.shrink_to_fit();
    }
    out.schedule = std::move(schedule);
    require(!s.failed || out.account->halted());
    out.snapshot = s;
    return out;
}
QByteArray encode_portfolio_account(const PortfolioAccountSnapshot &s, const std::atomic<bool> &cancel) {
    check(cancel);
    const auto project = QJsonDocument::fromJson(encode_portfolio_project(s.project, cancel)).object();
    auto replay = replay_portfolio_account(s, cancel);
    require(same(references(s.reference, s.baseline_reference),
                 references(replay.account->result(), replay.baseline->result())));
    QJsonArray excluded;
    for (auto n : s.excluded)
        excluded.append(static_cast<int>(n));
    QJsonObject data{{"project", project},
                     {"live", s.live},
                     {"failed", s.failed},
                     {"interval", s.interval},
                     {"cutoff", static_cast<double>(s.warmup_cutoff)},
                     {"warmup_end", static_cast<double>(s.warmup_end)},
                     {"index", static_cast<int>(s.index)},
                     {"stop_after", s.stop_after},
                     {"stop_reason", QString::fromStdString(s.stop_reason)},
                     {"frames", frames_json(s.frames)},
                     {"audit", s.audit ? QJsonValue(audit_json(*s.audit)) : QJsonValue(QJsonValue::Null)},
                     {"excluded", excluded},
                     {"reference", references(s.reference, s.baseline_reference)}};
    auto bytes = QJsonDocument(QJsonObject{{"format", "qpl-portfolio-account"},
                                           {"version", 1},
                                           {"model", "qpl-portfolio-account-v1"},
                                           {"data", data},
                                           {"sha256", hash(data)}})
                     .toJson(QJsonDocument::Compact);
    require(bytes.size() <= portfolio_account_byte_limit);
    check(cancel);
    return bytes;
}
PortfolioAccountRestore decode_portfolio_account(const QByteArray &bytes, const std::atomic<bool> &cancel) {
    check(cancel);
    require(!bytes.isEmpty() && bytes.size() <= portfolio_account_byte_limit);
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject());
    auto root = object(doc.object(), {"format", "version", "model", "data", "sha256"});
    require(root["format"] == "qpl-portfolio-account" && integer(root["version"], 1, 1) == 1 &&
            root["model"] == "qpl-portfolio-account-v1" && root["data"].isObject() &&
            root["sha256"].isString() && root["sha256"].toString() == hash(root["data"].toObject()));
    auto o = object(root["data"], {"project", "live", "failed", "interval", "cutoff", "warmup_end", "index",
                                   "stop_after", "stop_reason", "frames", "audit", "excluded", "reference"});
    PortfolioAccountSnapshot s;
    require(o["project"].isObject());
    s.project = decode_portfolio_project(
        QJsonDocument(o["project"].toObject()).toJson(QJsonDocument::Compact), cancel);
    s.live = boolean(o["live"]);
    s.failed = boolean(o["failed"]);
    s.interval = static_cast<int>(integer(o["interval"], 0, 60));
    s.warmup_cutoff = integer(o["cutoff"], 0, 32503852800000LL);
    s.warmup_end = integer(o["warmup_end"], 0, 32503852800000LL);
    s.index = static_cast<std::size_t>(integer(o["index"], 0, 5000));
    s.stop_after = static_cast<int>(integer(o["stop_after"], -1, 5000));
    require(o["stop_reason"].isString() && o["stop_reason"].toString().size() <= 20);
    s.stop_reason = o["stop_reason"].toString().toStdString();
    auto assets = portfolio_project_assets(s.project);
    s.frames = frames(o["frames"], assets.size(), 5000, cancel);
    if (!o["audit"].isNull())
        s.audit = audit(o["audit"], cancel);
    require(o["excluded"].isArray());
    auto excluded = o["excluded"].toArray();
    require(excluded.size() == static_cast<qsizetype>(assets.size()));
    for (const auto &n : excluded)
        s.excluded.push_back(static_cast<std::size_t>(integer(n, 0, 5000)));
    auto out = replay_portfolio_account(s, cancel);
    const auto a = out.account->result(), b = out.baseline->result();
    require(same(o["reference"], references(a, b)));
    out.snapshot.reference = a;
    out.snapshot.baseline_reference = b;
    return out;
}
void write_portfolio_account(const QString &path, const PortfolioAccountSnapshot &s,
                             const std::atomic<bool> &cancel) {
    auto bytes = encode_portfolio_account(s, cancel);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
    check(cancel);
    require(file.commit());
}
PortfolioAccountRestore read_portfolio_account(const QString &path, const std::atomic<bool> &cancel) {
    check(cancel);
    require(QFileInfo(path).isFile());
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= portfolio_account_byte_limit);
    auto bytes = file.read(portfolio_account_byte_limit + 1);
    require(file.error() == QFileDevice::NoError && file.atEnd());
    return decode_portfolio_account(bytes, cancel);
}
} // namespace live
