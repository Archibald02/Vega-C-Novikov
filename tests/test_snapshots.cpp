#include "research_snapshot.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <doctest/doctest.h>
namespace {
live::ReplaySnapshot example(int count = 180) {
    live::ReplaySnapshot s;
    s.config.strategy.fast = 2;
    s.config.strategy.slow = 4;
    s.config.tick_size = .01;
    s.config.lot = 10;
    for (int i = 0; i < count; ++i) {
        const auto t = 1700000000000LL + i * 86400000LL;
        const double p = 100 + (i % 12 < 6 ? i % 6 : 6 - i % 6);
        s.bars.push_back({t, t + 86400000, p, p + 1, p - 1, p, 10000, 10800});
    }
    s.reference = quant::backtest(s.bars, s.config);
    return s;
}
QByteArray encode(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
} // namespace
TEST_CASE("Snapshot reproduces complete causal backtest independently of UI settings") {
    const auto s = example();
    std::atomic<bool> cancelled{false};
    const auto bytes = live::encode_snapshot(s, "СИНТЕТИЧЕСКОЕ ДЕМО", "Опыт", "Комиссии включены");
    CHECK(live::verify_snapshot(bytes, cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
    auto root = QJsonDocument::fromJson(bytes).object();
    SUBCASE("tampered OHLCV hash is rejected") {
        auto bars = root["bars"].toArray();
        auto b = bars[0].toArray();
        b[5] = 100.1;
        bars[0] = b;
        root["bars"] = bars;
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
    SUBCASE("changed reference is reported without claiming success") {
        auto r = root["reference"].toObject();
        auto e = r["equity"].toArray();
        e[0] = e[0].toDouble() + 100;
        r["equity"] = e;
        root["reference"] = r;
        CHECK(live::verify_snapshot(encode(root), cancelled).startsWith(QStringLiteral("НЕ СОВПАЛО:")));
    }
    SUBCASE("missing parameters cannot inherit current defaults") {
        auto c = root["config"].toObject();
        c.remove("lot");
        root["config"] = c;
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
    SUBCASE("huge indicator allocation is rejected before creating an engine") {
        auto c = root["config"].toObject();
        c["slow"] = 1000000000;
        root["config"] = c;
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
    SUBCASE("unknown engine is rejected") {
        root["engine"] = "future-model";
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
    SUBCASE("fractional timestamp and invalid schema are rejected") {
        auto bars = root["bars"].toArray();
        auto b = bars[0].toArray();
        b[0] = b[0].toDouble() + .5;
        bars[0] = b;
        root["bars"] = bars;
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
    SUBCASE("cancelled work cannot report success") {
        cancelled = true;
        CHECK(live::verify_snapshot(bytes, cancelled).contains(QStringLiteral("отменена")));
    }
}
TEST_CASE("Snapshot limits remain bounded at 5000 candles") {
    std::atomic<bool> cancelled{false};
    const auto s = example(5000);
    const auto bytes = live::encode_snapshot(s, "DEMO", "Boundary", {});
    CHECK(bytes.size() < live::snapshot_byte_limit);
    CHECK(live::verify_snapshot(bytes, cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
    CHECK_THROWS(live::verify_snapshot(QByteArray(live::snapshot_byte_limit + 1, ' '), cancelled));
}
TEST_CASE("Snapshots preserve temporal settings and verify actual execution trace") {
    auto s = example();
    s.config.entry_confirmation = 2;
    s.config.exit_confirmation = 3;
    s.config.cooldown_bars = 4;
    s.config.max_holding_bars = 5;
    s.config.strategy.entry.rules.resize(4);
    s.config.strategy.exit.rules.resize(4);
    for (int i = 1; i < 4; ++i) {
        s.config.strategy.entry.rules[i].enabled = false;
        s.config.strategy.exit.rules[i].enabled = false;
    }
    s.reference = quant::backtest(s.bars, s.config, 0, true);
    std::atomic<bool> cancelled{false};
    const auto bytes = live::encode_snapshot(s, "DEMO", "Timing", {});
    CHECK(live::verify_snapshot(bytes, cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
    auto root = QJsonDocument::fromJson(bytes).object();
    SUBCASE("trace tampering is reported") {
        auto ref = root["reference"].toObject();
        auto trace = ref["trace"].toArray();
        auto row = trace[0].toArray();
        row[3] = row[3].toDouble() + 1;
        trace[0] = row;
        ref["trace"] = trace;
        root["reference"] = ref;
        CHECK(live::verify_snapshot(encode(root), cancelled).startsWith(QStringLiteral("НЕ СОВПАЛО:")));
    }
    SUBCASE("new schema cannot silently default a missing timing setting") {
        auto c = root["config"].toObject();
        c.remove("cooldown_bars");
        root["config"] = c;
        CHECK_THROWS(live::verify_snapshot(encode(root), cancelled));
    }
}
TEST_CASE("Version one default snapshots replay with explicit legacy timing defaults") {
    auto s = example();
    auto root = QJsonDocument::fromJson(live::encode_snapshot(s, "DEMO", "Legacy", {})).object();
    root["schema"] = 1;
    root["engine"] = "qpl-single-backtest-v1";
    auto config = root["config"].toObject();
    for (const auto *key : {"entry", "exit"}) {
        auto group = config[key].toObject();
        group.remove("expression");
        config[key] = group;
    }
    for (const auto *key : {"entry_confirmation", "exit_confirmation", "cooldown_bars", "max_holding_bars"})
        config.remove(key);
    root["config"] = config;
    std::atomic<bool> cancelled{false};
    CHECK(live::verify_snapshot(encode(root), cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
}
TEST_CASE("Maximum-length traced snapshot stays within its byte budget") {
    auto s = example(5000);
    s.reference = quant::backtest(s.bars, s.config, 0, true);
    REQUIRE(s.reference.trace.size() == 5000);
    const auto bytes = live::encode_snapshot(s, "DEMO", "Trace boundary", {});
    CHECK(bytes.size() < live::snapshot_byte_limit);
    std::atomic<bool> cancelled{false};
    CHECK(live::verify_snapshot(bytes, cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
}
TEST_CASE("Nested snapshot reproduces expression and rejects unknown tokens or references") {
    auto s = example();
    s.config.strategy.entry.rules.resize(4);
    for (auto &r : s.config.strategy.entry.rules)
        r = {quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 100, true};
    s.config.strategy.entry.expression = {0, 1, 2, -2, -1, 3, -1};
    s.reference = quant::backtest(s.bars, s.config, 0, true);
    auto root = QJsonDocument::fromJson(live::encode_snapshot(s, "DEMO", "Nested", {})).object();
    std::atomic<bool> cancelled{false};
    CHECK(live::verify_snapshot(encode(root), cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
    {
        auto bad = root;
        auto c = bad["config"].toObject();
        auto g = c["entry"].toObject();
        g.remove("expression");
        c["entry"] = g;
        bad["config"] = c;
        CHECK_THROWS(live::verify_snapshot(encode(bad), cancelled));
    }
    for (const auto &tokens : std::vector<QJsonArray>{{0, 1, 2, -2, -1, 999, -1},
                                                      {0, 1, 2, -3, -1, 3, -1},
                                                      {0, 0, -1},
                                                      {4294967296.0, 1, 2, -2, -1, 3, -1}}) {
        auto bad = root;
        auto c = bad["config"].toObject();
        auto g = c["entry"].toObject();
        g["expression"] = tokens;
        c["entry"] = g;
        bad["config"] = c;
        CHECK_THROWS(live::verify_snapshot(encode(bad), cancelled));
    }
}
TEST_CASE("Version two flat snapshots retain timing and traces without inventing a formula") {
    auto s = example();
    s.config.cooldown_bars = 2;
    s.config.max_holding_bars = 3;
    s.reference = quant::backtest(s.bars, s.config, 0, true);
    auto root = QJsonDocument::fromJson(live::encode_snapshot(s, "DEMO", "V2", {})).object();
    root["schema"] = 2;
    root["engine"] = "qpl-single-backtest-v2";
    auto c = root["config"].toObject();
    for (const auto *key : {"entry", "exit"}) {
        auto g = c[key].toObject();
        g.remove("expression");
        c[key] = g;
    }
    root["config"] = c;
    std::atomic<bool> cancelled{false};
    CHECK(live::verify_snapshot(encode(root), cancelled).startsWith(QStringLiteral("СОВПАЛО:")));
}
