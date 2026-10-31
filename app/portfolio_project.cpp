#include "portfolio_project.hpp"
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
#include <set>
#include <stdexcept>
namespace live {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument("Portfolio project schema/data");
}
void checkpoint(const std::atomic<bool> &cancel) { require(!cancel.load()); }
double number(const QJsonValue &v, double low, double high, double scale = 0) {
    require(v.isDouble());
    const double n = v.toDouble();
    require(std::isfinite(n) && n >= low && n <= high);
    if (scale)
        require(std::abs(n * scale - std::round(n * scale)) <= 1e-5);
    return n;
}
int integer(const QJsonValue &v, int low, int high) {
    const auto n = number(v, low, high);
    require(std::floor(n) == n);
    return static_cast<int>(n);
}
bool boolean(const QJsonValue &v) {
    require(v.isBool());
    return v.toBool();
}
QString text(const QJsonValue &v, int limit, bool empty = false) {
    require(v.isString());
    const auto s = v.toString();
    require((empty || !s.isEmpty()) && s.size() <= limit);
    for (const auto c : s)
        require(c.category() != QChar::Other_Control && c.category() != QChar::Other_Format);
    return s;
}
QJsonObject object(const QJsonValue &v, QStringList keys) {
    require(v.isObject());
    auto o = v.toObject();
    keys.sort();
    require(o.keys() == keys);
    return o;
}
QJsonObject config_json(const quant::PortfolioConfig &c) {
    return {{"cash", c.initial_cash},
            {"commission", c.commission_rate},
            {"fixed", c.fixed_fee},
            {"spread", c.spread_bps},
            {"slippage", c.slippage_bps},
            {"weight_limit", c.max_weight},
            {"invested_limit", c.max_invested},
            {"group_limit", c.group_limit},
            {"drawdown", c.max_drawdown},
            {"daily", c.daily_loss},
            {"rebalance", c.rebalance_every},
            {"manual_stop", c.manual_stop_after}};
}
quant::PortfolioConfig config(const QJsonValue &v) {
    quant::PortfolioConfig c;
    const auto o = object(v, config_json(c).keys());
    c.initial_cash = number(o["cash"], 1000, 1e12, 100);
    c.commission_rate = number(o["commission"], 0, .1, 100000);
    c.fixed_fee = number(o["fixed"], 0, 1e9, 100);
    c.spread_bps = number(o["spread"], 0, 1000, 100);
    c.slippage_bps = number(o["slippage"], 0, 1000, 100);
    c.max_weight = number(o["weight_limit"], .01, 1, 10000);
    c.max_invested = number(o["invested_limit"], .01, 1, 10000);
    c.group_limit = number(o["group_limit"], 0, 1, 10000);
    c.max_drawdown = number(o["drawdown"], .001, 1, 10000);
    c.daily_loss = number(o["daily"], .001, 1, 10000);
    c.rebalance_every = integer(o["rebalance"], 0, 5000);
    c.manual_stop_after = integer(o["manual_stop"], 0, 5000);
    return c;
}
QString digest(const QJsonObject &o) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
            .toHex());
}
QJsonObject payload(const PortfolioProject &p, const std::atomic<bool> &cancel) {
    require(p.input.assets.size() >= 1 && p.input.assets.size() <= 6 &&
            p.enabled.size() == p.input.assets.size() && p.modes.size() == p.input.assets.size());
    QJsonArray assets;
    for (std::size_t i = 0; i < p.input.assets.size(); ++i) {
        checkpoint(cancel);
        const auto &a = p.input.assets[i];
        require(a.bars.size() >= 2 && a.bars.size() <= 5000);
        QJsonArray bars;
        for (const auto &b : a.bars) {
            checkpoint(cancel);
            bars.append(QJsonArray{static_cast<double>(b.time), static_cast<double>(b.end), b.open, b.high,
                                   b.low, b.close, b.volume, b.utc_offset_seconds});
        }
        assets.append(QJsonObject{{"symbol", QString::fromStdString(a.symbol)},
                                  {"group", QString::fromStdString(a.group)},
                                  {"weight", a.weight},
                                  {"lot", static_cast<double>(a.lot)},
                                  {"tick", a.tick_size},
                                  {"enabled", p.enabled[i]},
                                  {"mode", p.modes[i]},
                                  {"bars", bars}});
    }
    return {{"source", p.input.source},
            {"synthetic", p.input.synthetic},
            {"config", config_json(p.config)},
            {"fast", p.fast},
            {"slow", p.slow},
            {"assets", assets}};
}
PortfolioProject parse(const QJsonValue &v, const std::atomic<bool> &cancel) {
    const auto o = object(v, {"source", "synthetic", "config", "fast", "slow", "assets"});
    PortfolioProject p;
    p.input.source = text(o["source"], 1000);
    p.input.synthetic = boolean(o["synthetic"]);
    p.input.metadata_known = false; // a checksum is not source authentication
    p.config = config(o["config"]);
    p.fast = integer(o["fast"], 2, 499);
    p.slow = integer(o["slow"], 3, 500);
    require(o["assets"].isArray());
    const auto assets = o["assets"].toArray();
    require(assets.size() >= 1 && assets.size() <= 6);
    std::set<QString> symbols;
    for (const auto &value : assets) {
        checkpoint(cancel);
        const auto a = object(value, {"symbol", "group", "weight", "lot", "tick", "enabled", "mode", "bars"});
        quant::PortfolioAsset asset;
        const auto symbol = text(a["symbol"], 32);
        require(symbol.toUtf8().size() <= 32 && symbols.insert(symbol).second);
        asset.symbol = symbol.toStdString();
        asset.group = text(a["group"], 40, true).toStdString();
        asset.weight = number(a["weight"], 0, 1, 10000);
        asset.lot = integer(a["lot"], 1, 1000000);
        asset.tick_size = number(a["tick"], 0, 1e9, 1e8);
        p.enabled.push_back(boolean(a["enabled"]));
        p.modes.push_back(integer(a["mode"], 0, 4));
        require(a["bars"].isArray());
        const auto bars = a["bars"].toArray();
        require(bars.size() >= 2 && bars.size() <= 5000);
        for (const auto &bar : bars) {
            checkpoint(cancel);
            require(bar.isArray());
            const auto b = bar.toArray();
            require(b.size() == 8);
            quant::Bar r;
            r.time = static_cast<qint64>(number(b[0], 1, 32503680000000., 1));
            r.end = static_cast<qint64>(number(b[1], 1, 32503852800000., 1));
            r.open = number(b[2], 1e-9, 1e12);
            r.high = number(b[3], 1e-9, 1e12);
            r.low = number(b[4], 1e-9, 1e12);
            r.close = number(b[5], 1e-9, 1e12);
            r.volume = number(b[6], 0, 1e18);
            r.utc_offset_seconds = integer(b[7], -86400, 86400);
            const auto offset =
                QDateTime::fromMSecsSinceEpoch(r.time, QTimeZone("Europe/Moscow")).offsetFromUtc();
            require(r.utc_offset_seconds == offset);
            asset.bars.push_back(r);
        }
        quant::validate_bars(asset.bars);
        p.input.assets.push_back(std::move(asset));
    }
    checkpoint(cancel);
    return p;
}
} // namespace
QByteArray encode_portfolio_project(const PortfolioProject &p, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    const auto data = payload(p, cancel);
    parse(data, cancel); // same strict contract for writes; never silently clamp
                         // or truncate
    const auto bytes = QJsonDocument(QJsonObject{{"format", "qpl-portfolio-project"},
                                                 {"version", 1},
                                                 {"model", "qpl-portfolio-next-common-open-v1"},
                                                 {"data", data},
                                                 {"sha256", digest(data)}})
                           .toJson(QJsonDocument::Compact);
    require(bytes.size() <= portfolio_project_byte_limit);
    checkpoint(cancel);
    return bytes;
}
PortfolioProject decode_portfolio_project(const QByteArray &bytes, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    require(!bytes.isEmpty() && bytes.size() <= portfolio_project_byte_limit);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject());
    const auto o = object(doc.object(), {"format", "version", "model", "data", "sha256"});
    require(o["format"] == "qpl-portfolio-project" && integer(o["version"], 1, 1) == 1 &&
            o["model"] == "qpl-portfolio-next-common-open-v1" && o["data"].isObject());
    require(text(o["sha256"], 64) == digest(o["data"].toObject()));
    return parse(o["data"], cancel);
}
PortfolioProject read_portfolio_project(const QString &path, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    require(QFileInfo(path).isFile());
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= portfolio_project_byte_limit);
    const auto bytes = file.read(portfolio_project_byte_limit + 1);
    require(file.error() == QFileDevice::NoError && file.atEnd());
    return decode_portfolio_project(bytes, cancel);
}
void write_portfolio_project(const QString &path, const PortfolioProject &p,
                             const std::atomic<bool> &cancel) {
    const auto bytes = encode_portfolio_project(p, cancel);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
    checkpoint(cancel);
    require(file.commit());
}
void portfolio_template(quant::PortfolioAsset &a, int mode, int fast, int slow) {
    require(mode >= 0 && mode <= 4);
    a.strategy = quant::Strategy{};
    a.use_rules = mode != 0;
    if (mode == 1) {
        a.strategy.fast = fast;
        a.strategy.slow = slow;
    }
    if (mode == 2) {
        a.strategy.fast = 5;
        a.strategy.slow = 20;
    }
    if (mode == 3) {
        a.strategy.fast = 20;
        a.strategy.slow = 60;
    }
    if (mode == 4) {
        a.strategy.entry = {
            {{quant::Feature::Rsi, quant::Feature::Constant, quant::Compare::Below, 30, true}}, true};
        a.strategy.exit = {{{quant::Feature::Rsi, quant::Feature::Constant, quant::Compare::Above, 70, true}},
                           true};
    }
}
std::vector<quant::PortfolioAsset> portfolio_project_assets(const PortfolioProject &p) {
    require(p.enabled.size() == p.input.assets.size() && p.modes.size() == p.input.assets.size());
    std::vector<quant::PortfolioAsset> assets;
    for (std::size_t i = 0; i < p.enabled.size(); ++i)
        if (p.enabled[i]) {
            auto a = p.input.assets[i];
            a.group = QString::fromStdString(a.group).trimmed().toCaseFolded().toStdString();
            portfolio_template(a, p.modes[i], p.fast, p.slow);
            assets.push_back(std::move(a));
        }
    std::sort(assets.begin(), assets.end(), [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
    return assets;
}
} // namespace live
