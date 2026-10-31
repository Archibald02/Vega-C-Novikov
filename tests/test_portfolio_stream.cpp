#include "quant/portfolio_stream.hpp"
#include <doctest/doctest.h>
namespace {
constexpr std::int64_t start = 1700000000000LL, hour = 3600000;
quant::Bar bar(int i, double p = 100) {
    return {start + i * hour, start + (i + 1) * hour, p, p, p, p, 10000, 10800};
}
std::vector<quant::PortfolioAsset> seed() {
    std::vector<quant::PortfolioAsset> a(2);
    a[0].symbol = "A";
    a[1].symbol = "B";
    for (auto &v : a) {
        v.weight = .4;
        v.bars = {bar(0), bar(1)};
    }
    return a;
}
quant::PortfolioStreamBuffer buffer() { return {seed(), bar(1).time, bar(1).end, start, 60}; }
} // namespace
TEST_CASE("Portfolio stream complete cycles are idempotent and wait for common bars") {
    auto b = buffer();
    CHECK(b.merge({{bar(0), bar(1)}, {bar(0), bar(1)}}, start, bar(2).end).empty());
    CHECK(b.merge({{bar(0), bar(1), bar(2)}, {bar(0), bar(1)}}, start, bar(2).end).empty());
    auto f = b.merge({{bar(0), bar(1), bar(2)}, {bar(0), bar(1), bar(2)}}, start, bar(2).end);
    REQUIRE(f.size() == 1);
    CHECK(f[0][0].time == bar(2).time);
    CHECK(b.merge({{bar(0), bar(1), bar(2)}, {bar(0), bar(1), bar(2)}}, start, bar(2).end).empty());
    CHECK(b.retained_bars() == 6);
}
TEST_CASE("Portfolio stream revisions deletions late inserts and bad cycles fail atomically") {
    auto b = buffer();
    CHECK_THROWS(b.merge({{bar(0), bar(1), bar(2)}, {bar(0), bar(1, 101)}}, start, bar(2).end));
    CHECK(b.retained_bars() == 4);
    CHECK(b.last_time() == bar(1).time);
    CHECK_THROWS(b.merge({{bar(0), bar(1)}, {bar(1)}}, start, bar(2).end));
    CHECK_THROWS(b.merge({{bar(0), bar(1)}, {}}, start, bar(2).end));
    CHECK_THROWS(b.merge({{bar(0), bar(1), bar(1)}, {bar(0), bar(1)}}, start, bar(2).end));
    CHECK_THROWS(b.merge({{bar(0), bar(1), bar(2)}, {bar(0), bar(1), bar(2)}}, start, bar(1).end));
    CHECK_THROWS(b.merge({{bar(0), bar(1)}}, start, bar(2).end));
    CHECK(b.retained_bars() == 4);
    auto f = b.merge({{bar(0), bar(1), bar(2), bar(3)}, {bar(0), bar(1), bar(3)}}, start, bar(3).end);
    REQUIRE(f.size() == 1);
    CHECK(f[0][0].time == bar(3).time);
    CHECK_THROWS(
        b.merge({{bar(0), bar(1), bar(2), bar(3)}, {bar(0), bar(1), bar(2), bar(3)}}, start, bar(3).end));
    CHECK(b.last_time() == bar(3).time);
}
TEST_CASE("Portfolio stream audit overlap and memory limits are explicit") {
    auto b = buffer();
    for (int i = 2; i < 1000; ++i) {
        auto f = b.merge({{bar(i - 1), bar(i)}, {bar(i - 1), bar(i)}}, bar(i - 1).time, bar(i).end);
        REQUIRE(f.size() == 1);
        CHECK(b.retained_bars() <= 4);
    }
    CHECK(b.merge({{bar(0, 101), bar(998), bar(999)}, {bar(998), bar(999)}}, bar(998).time, bar(999).end)
              .empty());
    auto other = buffer();
    std::vector<quant::Bar> many;
    for (int i = 0; i < 513; ++i)
        many.push_back(bar(i));
    CHECK_THROWS(other.merge({many, many}, start, bar(512).end));
    CHECK(other.retained_bars() == 4);
}
TEST_CASE("Portfolio stream catch-up equals sequential warmed execution") {
    quant::PortfolioConfig c;
    c.commission_rate = c.spread_bps = c.slippage_bps = 0;
    quant::PortfolioAccount a(seed(), c), b(seed(), c);
    for (auto *v : {&a, &b}) {
        v->warmup({bar(0), bar(0)});
        v->warmup({bar(1), bar(1)});
        v->arm_from_warmup();
    }
    auto stream = buffer();
    auto f =
        stream.merge({{bar(0), bar(1), bar(2, 110), bar(3, 120)}, {bar(0), bar(1), bar(2, 90), bar(3, 80)}},
                     start, bar(3).end);
    for (const auto &frame : f)
        a.step(frame);
    b.step({bar(2, 110), bar(2, 90)});
    b.step({bar(3, 120), bar(3, 80)});
    CHECK(a.result().equity == b.result().equity);
    CHECK(a.result().cash == b.result().cash);
    CHECK(a.result().fills == 2);
    a.request_stop("DATA_ERROR");
    CHECK(a.result().halt_reason == "DATA_ERROR");
    CHECK_THROWS(a.request_stop("raw reply"));
}
