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
