#include "quant/research.hpp"
#include <cmath>
#include <doctest/doctest.h>
namespace {
constexpr std::int64_t hour = 3600000, base = 20000LL * 24 * hour - 3 * hour;
std::vector<quant::Bar> hourly(int n = 20) {
    std::vector<quant::Bar> bars;
    for (int i = 0; i < n; ++i) {
        const auto t = base + i * hour;
        const double p = 100 + i;
        bars.push_back({t, t + hour, p, p + .5, p - .5, p, 10000, 10800});
    }
    return bars;
}
quant::SimConfig higher_config() {
    quant::SimConfig c;
    c.initial_cash = 10000;
    c.allocation = .5;
    c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = c.stop_fraction = c.target_fraction = 0;
    c.max_drawdown = c.daily_loss = 1;
    c.liquidate_end = false;
    c.strategy.entry = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, true}},
                        true};
    c.strategy.exit = {};
    c.higher_factor = 2;
    c.higher_fast = 2;
    c.higher_slow = 3;
    return c;
}
} // namespace
TEST_CASE("Higher hourly blocks become visible exactly at their closed boundary") {
    quant::ClosedHourTrend h(2, 2, 3);
    auto bars = hourly(8);
    for (int i = 0; i < 5; ++i) {
        const auto s = h.push(bars[i]);
        CHECK_FALSE(s.ready);
        CHECK(s.last_end <= bars[i].end);
    }
    const auto ready = h.push(bars[5]);
    REQUIRE(ready.ready);
    CHECK(ready.completed == 3);
    CHECK(ready.progress == 0);
    CHECK(ready.last_end == bars[5].end);
    CHECK(ready.close == 105);
    CHECK(ready.fast == 104);
    CHECK(ready.slow == 103);
    bars[6].open = bars[6].close = 999;
    bars[6].low = 998;
    bars[6].high = 1000;
    const auto incomplete = h.push(bars[6]);
    CHECK(incomplete.progress == 1);
    CHECK(incomplete.last_end == ready.last_end);
    CHECK(incomplete.fast == ready.fast);
    CHECK(incomplete.slow == ready.slow);
}
TEST_CASE("A partial first bucket never supplies its missing initial candle") {
    auto bars = hourly(8);
    quant::ClosedHourTrend h(2, 2, 3);
    auto s = h.push(bars[1]);
    CHECK(s.completed == 0);
    CHECK(s.last_end == 0);
    CHECK(s.discarded == 1);
    s = h.push(bars[2]);
    CHECK(s.last_end == 0);
    s = h.push(bars[3]);
    CHECK(s.completed == 1);
    CHECK(s.last_end == bars[3].end);
    for (int i = 4; i < 8; ++i)
        s = h.push(bars[i]);
    CHECK(s.ready);
    CHECK(s.completed == 3);
    CHECK(s.fast == 106);
    CHECK(s.slow == 105);
}
TEST_CASE("Missing hourly slots invalidate the filter without forward-fill or "
          "invented candles") {
    auto bars = hourly(10);
    quant::ClosedHourTrend h(2, 2, 3);
    for (int i = 0; i < 7; ++i)
        h.push(bars[i]);
    const auto gap = h.push(bars[8]);
    CHECK_FALSE(gap.ready);
    CHECK(gap.completed == 3);
    CHECK(gap.last_end == bars[5].end);
    CHECK(gap.breaks == 1);
    CHECK(gap.discarded == 1);
    const auto complete = h.push(bars[9]);
    CHECK(complete.ready);
    CHECK(complete.completed == 4);
    CHECK(complete.fast == 107);
    CHECK(complete.slow == doctest::Approx(317. / 3));
}
TEST_CASE("Local-hour offset changes reset higher SMA warmup") {
    auto bars = hourly(14);
    quant::ClosedHourTrend h(2, 2, 3);
    for (int i = 0; i < 6; ++i)
        h.push(bars[i]);
    for (int i = 6; i < 14; ++i)
        bars[i].utc_offset_seconds = 14400;
    const auto changed = h.push(bars[6]);
    CHECK_FALSE(changed.ready);
    CHECK(changed.completed == 0);
    CHECK(changed.last_end == 0);
    CHECK(std::isnan(changed.fast));
    for (int i = 7; i < 12; ++i)
        CHECK_FALSE(h.push(bars[i]).ready);
    CHECK(h.push(bars[12]).ready);
}
TEST_CASE("Higher entry gate uses only completed blocks and executes the next open") {
    const auto bars = hourly(10);
    const auto c = higher_config();
    const auto r = quant::backtest(bars, c, 0, true);
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(r.trace[i].pending_entry);
        CHECK(r.trace[i].gate == "higher entry blocked");
    }
    CHECK(r.trace[5].pending_entry);
    CHECK(r.trace[5].higher_entry_allowed);
    REQUIRE(r.trace[6].fill_count == 1);
    CHECK(r.trace[6].fills[0].quantity > 0);
    CHECK(r.trace[6].higher.last_end == bars[5].end);
}
TEST_CASE("A chronological input error is rejected before virtual cash or "
          "position changes") {
    auto c = higher_config();
    c.strategy.buy_hold = true;
    quant::PaperEngine paper(c, true);
    auto bars = hourly(2);
    auto bad = bars[0];
    bad.end += hour;
    CHECK_THROWS(paper.step(bad));
    CHECK(paper.result().cash == 10000);
    CHECK(paper.result().units == 0);
    paper.step(bars[0]);
    const auto before = paper.result();
    bad = bars[1];
    bad.time += 1;
    bad.end += 1;
    CHECK_THROWS(paper.step(bad));
    CHECK(paper.result().equity == before.equity);
    CHECK(paper.result().units == before.units);
}
TEST_CASE("Higher confirmation is prefix causal and shared with incremental "
          "warmup replay") {
    auto c = higher_config();
    auto bars = hourly(20);
    const auto batch = quant::backtest(bars, c, 0, true);
    quant::PaperEngine paper(c, true);
    for (const auto &b : bars)
        paper.step(b);
    CHECK(paper.result().equity == batch.equity);
    const auto warm = quant::backtest(bars, c, 6, true);
    CHECK(warm.units == batch.units);
    CHECK(warm.cash == batch.cash);
    auto changed = bars;
    for (std::size_t i = 9; i < changed.size(); ++i)
        changed[i].open = changed[i].close = 1, changed[i].low = .5, changed[i].high = 2;
    const auto future = quant::backtest(changed, c, 0, true);
    for (int i = 0; i < 9; ++i) {
        CHECK(batch.equity[i + 1] == future.equity[i + 1]);
        CHECK(batch.trace[i].higher.last_end == future.trace[i].higher.last_end);
        if (std::isfinite(batch.trace[i].higher.fast))
            CHECK(batch.trace[i].higher.fast == future.trace[i].higher.fast);
    }
}
TEST_CASE("Higher exit confirmation never delays stop or maximum holding time") {
    auto c = higher_config();
    c.strategy.buy_hold = true;
    c.higher_confirm_exit = true;
    c.strategy.exit = c.strategy.entry;
    auto bars = hourly(10);
    auto r = quant::backtest(bars, c, 0, true);
    CHECK(r.units > 0);
    CHECK(r.trace[1].gate == "higher exit blocked");
    c.max_holding_bars = 2;
    r = quant::backtest(bars, c, 0, true);
    CHECK(r.units == 0);
    CHECK(r.trace[2].fills[0].reason == "time exit: next open");
    c.max_holding_bars = 0;
    c.stop_fraction = .05;
    bars[1].open = bars[1].close = 90;
    bars[1].high = 91;
    bars[1].low = 89;
    r = quant::backtest(bars, c, 0, true);
    CHECK(r.trace[1].fills[0].reason == "stop gap: opening price");
}
TEST_CASE("Full buy-and-hold baseline neutralizes higher filtering") {
    auto c = higher_config();
    const auto bars = hourly(8);
    const auto r = quant::buy_hold(bars, c);
    c.higher_factor = 0;
    CHECK(quant::buy_hold(bars, c).equity == r.equity);
}
TEST_CASE("All supported grids are anchored at local midnight with bounded warmup") {
    const auto bars = hourly(24 * 4);
    for (int factor : {2, 3, 4, 6, 12, 24}) {
        quant::ClosedHourTrend h(factor, 2, 3);
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const auto s = h.push(bars[i]);
            CHECK(s.completed == static_cast<int>((i + 1) / factor));
            CHECK(s.last_end <= bars[i].end);
        }
    }
    for (int factor : {-1, 1, 5, 25, 1000000000})
        CHECK_THROWS(quant::ClosedHourTrend(factor, 2, 3));
    CHECK_THROWS(quant::ClosedHourTrend(2, 2, 1000000000));
    auto c = higher_config();
    c.higher_slow = 1000000000;
    CHECK_THROWS(quant::PaperEngine(c));
}

TEST_CASE("Bearish higher blocks confirm an ordinary exit only at their own close") {
    auto c = higher_config();
    c.strategy.buy_hold = true;
    c.higher_confirm_exit = true;
    c.strategy.exit = c.strategy.entry;
    auto bars = hourly(12);
    for (int i = 6; i < 12; ++i) {
        const double p = 100 - (i - 6) * 2;
        bars[i].open = bars[i].close = p;
        bars[i].low = p - .5;
        bars[i].high = p + .5;
    }
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK_FALSE(r.trace[6].pending_exit);
    REQUIRE(r.trace[7].higher.ready);
    CHECK(r.trace[7].higher.fast < r.trace[7].higher.slow);
    CHECK(r.trace[7].pending_exit);
    REQUIRE(r.trace[8].fill_count == 1);
    CHECK(r.trace[8].fills[0].quantity < 0);
}
TEST_CASE("Already confirmed entry stays causal across the next session gap") {
    auto c = higher_config();
    auto bars = hourly(10);
    bars.erase(bars.begin() + 6, bars.begin() + 8);
    const auto r = quant::backtest(bars, c, 0, true);
    REQUIRE(r.trace[5].pending_entry);
    CHECK_FALSE(r.trace[6].higher.ready);
    REQUIRE(r.trace[6].fill_count == 1);
    CHECK(r.trace[6].fills[0].quantity > 0);
}
