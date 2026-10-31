#include "portfolio_project.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <doctest/doctest.h>
namespace {
live::PortfolioProject example(int count = 90, int assets = 3) {
    live::PortfolioProject p;
    p.input.source = QStringLiteral("ДЕМО • тестовый набор");
    p.input.synthetic = true;
    p.fast = 7;
    p.slow = 21;
    p.config.initial_cash = 120000;
    p.config.fixed_fee = .25;
    for (int j = 0; j < assets; ++j) {
        quant::PortfolioAsset a;
        a.symbol = "DEMO_" + std::to_string(j);
        a.group = "group " + std::to_string(j);
        a.weight = .1;
        a.lot = 10;
        a.tick_size = .01;
        for (int i = 0; i < count; ++i) {
            const auto t = 1700000000000LL + i * 86400000LL;
            const double price = 100 + i * .01;
            a.bars.push_back({t, t + 86400000, price, price + 1, price - 1, price, 10000, 10800});
        }
        p.input.assets.push_back(std::move(a));
        p.enabled.push_back(j != 1);
        p.modes.push_back(j % 5);
    }
    return p;
}
QByteArray repack(QJsonObject o, QJsonObject data) {
    o["data"] = data;
    o["sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(data).toJson(QJsonDocument::Compact),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
} // namespace
TEST_CASE("Portfolio project roundtrip preserves bounded settings and data, "
          "but not source trust") {
    std::atomic<bool> cancel{false};
    auto p = example();
    p.input.moex_live = true;
    p.input.interval = 60;
    const auto bytes = live::encode_portfolio_project(p, cancel);
    const auto q = live::decode_portfolio_project(bytes, cancel);
    CHECK(live::encode_portfolio_project(q, cancel) == bytes);
    CHECK(q.fast == 7);
    CHECK(q.slow == 21);
    CHECK(q.enabled == p.enabled);
    CHECK(q.modes == p.modes);
    CHECK_FALSE(q.input.metadata_known);
    CHECK_FALSE(q.input.moex_live);
    CHECK(q.input.interval == 0);
    CHECK(q.config.initial_cash == 120000);
    CHECK(q.input.assets[2].bars.back().end == p.input.assets[2].bars.back().end);
    const auto before = quant::portfolio_backtest(p.input.assets, p.config);
    const auto after = quant::portfolio_backtest(q.input.assets, q.config);
    CHECK(before.equity == after.equity);
    CHECK(before.cash == after.cash);
    CHECK(before.fills == after.fills);
    CHECK_FALSE(bytes.contains("token"));
    CHECK_FALSE(bytes.contains("password"));
    CHECK_FALSE(bytes.contains("journal"));
}
TEST_CASE("Portfolio project rejects damage, future formats, unknown fields "
          "and invalid UI ranges") {
    std::atomic<bool> cancel{false};
    const auto bytes = live::encode_portfolio_project(example(), cancel);
    const auto original = QJsonDocument::fromJson(bytes).object();
    SUBCASE("unknown version") {
        auto o = original;
        o["version"] = 2;
        CHECK_THROWS(live::decode_portfolio_project(QJsonDocument(o).toJson(), cancel));
    }
    SUBCASE("unknown engine") {
        auto o = original;
        o["model"] = "future";
        CHECK_THROWS(live::decode_portfolio_project(QJsonDocument(o).toJson(), cancel));
    }
    SUBCASE("checksum mismatch") {
        auto data = original["data"].toObject();
        data["fast"] = 8;
        auto o = original;
        o["data"] = data;
        CHECK_THROWS(live::decode_portfolio_project(QJsonDocument(o).toJson(), cancel));
    }
    for (const auto &key : {QString("token"), QString("fast")}) {
        auto data = original["data"].toObject();
        data[key] = key == "fast" ? QJsonValue(2.5) : QJsonValue("not allowed");
        CHECK_THROWS(live::decode_portfolio_project(repack(original, data), cancel));
    }
    auto data = original["data"].toObject();
    auto c = data["config"].toObject();
    c["cash"] = 999;
    data["config"] = c;
    CHECK_THROWS(live::decode_portfolio_project(repack(original, data), cancel));
    CHECK_THROWS(live::decode_portfolio_project("{}", cancel));
    CHECK_THROWS(live::decode_portfolio_project(bytes.left(bytes.size() / 2), cancel));
}
TEST_CASE("Portfolio project validates every candle and row even when deselected") {
    std::atomic<bool> cancel{false};
    auto p = example();
    SUBCASE("duplicate symbols") { p.input.assets[1].symbol = p.input.assets[0].symbol; }
    SUBCASE("invalid OHLC") { p.input.assets[1].bars[0].low = 1e6; }
    SUBCASE("incorrect historical Moscow offset") { p.input.assets[1].bars[0].utc_offset_seconds = 0; }
    SUBCASE("fractional timestamps") {
        const auto bytes = live::encode_portfolio_project(p, cancel);
        auto o = QJsonDocument::fromJson(bytes).object();
        auto data = o["data"].toObject();
        auto assets = data["assets"].toArray();
        auto a = assets[0].toObject();
        auto bars = a["bars"].toArray();
        auto b = bars[0].toArray();
        b[0] = b[0].toDouble() + .5;
        bars[0] = b;
        a["bars"] = bars;
        assets[0] = a;
        data["assets"] = assets;
        CHECK_THROWS(live::decode_portfolio_project(repack(o, data), cancel));
        return;
    }
    SUBCASE("unknown mode") { p.modes[1] = 5; }
    SUBCASE("unexpected label controls") { p.input.source = "source\npassword"; }
    SUBCASE("missing enabled flag") { p.enabled.pop_back(); }
    CHECK_THROWS(live::encode_portfolio_project(p, cancel));
}
TEST_CASE("Portfolio project maximum history, oversize and cancellation have "
          "hard limits") {
    std::atomic<bool> cancel{false};
    auto p = example(5000, 6);
    const auto bytes = live::encode_portfolio_project(p, cancel);
    CHECK(bytes.size() < live::portfolio_project_byte_limit);
    const auto q = live::decode_portfolio_project(bytes, cancel);
    CHECK(q.input.assets.size() == 6);
    CHECK(q.input.assets.back().bars.size() == 5000);
    p.input.assets[0].bars.push_back(p.input.assets[0].bars.back());
    CHECK_THROWS(live::encode_portfolio_project(p, cancel));
    CHECK_THROWS(
        live::decode_portfolio_project(QByteArray(live::portfolio_project_byte_limit + 1, ' '), cancel));
    cancel = true;
    CHECK_THROWS(live::decode_portfolio_project(bytes, cancel));
    CHECK_THROWS(live::encode_portfolio_project(q, cancel));
}
TEST_CASE("Portfolio project atomic explicit file IO preserves old file on "
          "rejected or cancelled write") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto path = dir.filePath("portfolio.qpp.json");
    std::atomic<bool> cancel{false};
    auto p = example();
    live::write_portfolio_project(path, p, cancel);
    CHECK(live::read_portfolio_project(path, cancel).config.initial_cash == 120000);
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    file.close();
    cancel = true;
    CHECK_THROWS(live::write_portfolio_project(path, p, cancel));
    cancel = false;
    p.config.initial_cash = 0;
    CHECK_THROWS(live::write_portfolio_project(path, p, cancel));
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == original);
    file.close();
    CHECK_THROWS(live::read_portfolio_project(dir.path(), cancel));
    CHECK_THROWS(live::read_portfolio_project(dir.filePath("absent"), cancel));
    CHECK_THROWS(live::write_portfolio_project(dir.filePath("absent/portfolio.json"), example(), cancel));
}
