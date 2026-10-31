#include "quant/research.hpp"
#include <cmath>
#include <doctest/doctest.h>
namespace {
quant::SimConfig timing_config() {
    quant::SimConfig c;
    c.initial_cash = 10000;
    c.allocation = .5;
    c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = 0;
    c.stop_fraction = c.target_fraction = 0;
    c.max_drawdown = c.daily_loss = c.risk_per_trade = 1;
    c.liquidate_end = false;
    c.strategy.entry = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 100, true}},
                        true};
    c.strategy.exit = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Below, 100, true}},
                       true};
    return c;
}
std::vector<quant::Bar> timing_bars(std::initializer_list<double> prices) {
    std::vector<quant::Bar> bars;
    for (double price : prices) {
        const auto t = 1700000000000LL + static_cast<std::int64_t>(bars.size()) * 86400000;
        bars.push_back({t, t + 86400000, price, price + 1, price - 1, price, 10000});
    }
    return bars;
}
std::vector<std::int64_t> fill_times(const quant::SimResult &r, bool buys) {
    std::vector<std::int64_t> times;
    for (const auto &e : r.journal)
        if (buys ? e.quantity > 0 : e.quantity < 0)
            times.push_back(e.time);
    return times;
}
} // namespace
TEST_CASE("Consecutive confirmation resets on a false close and executes only next open") {
    auto c = timing_config();
    c.entry_confirmation = c.exit_confirmation = 2;
    const auto bars = timing_bars({101, 102, 99, 101, 102, 103, 99, 98, 97});
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(fill_times(r, true) == std::vector<std::int64_t>{bars[2].time});
    CHECK(fill_times(r, false) == std::vector<std::int64_t>{bars[8].time});
    REQUIRE(r.trace.size() == bars.size());
    CHECK(r.trace[0].gate == "confirmation");
    CHECK(r.trace[1].pending_entry);
    CHECK(r.trace[3].exit_streak == 0);
    CHECK(r.trace[7].exit_streak == 2);
    auto warm = quant::backtest(timing_bars({101, 102, 103, 104}), c, 3, true);
    CHECK(warm.metrics.fills == 1);
    CHECK(warm.trace.front().fill_count == 1);
}
TEST_CASE("A crossing is a one-bar event and is not silently converted to a level") {
    auto c = timing_config();
    c.entry_confirmation = 2;
    c.strategy.entry.rules[0].op = quant::Compare::CrossAbove;
    CHECK(quant::backtest(timing_bars({99, 101, 102, 103, 99, 101, 102}), c).metrics.fills == 0);
}
TEST_CASE("Cooldown excludes exit bar and blocks exactly N subsequent full closes") {
    auto c = timing_config();
    c.strategy.entry.rules[0].value = 0;
    c.cooldown_bars = 2;
    c.max_holding_bars = 1;
    const auto bars = timing_bars({101, 101, 101, 101, 101, 101, 101, 101});
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(fill_times(r, true) == std::vector<std::int64_t>{bars[1].time, bars[6].time});
    CHECK(fill_times(r, false) == std::vector<std::int64_t>{bars[2].time, bars[7].time});
    CHECK(r.trace[2].cooldown_remaining == 2);
    CHECK(r.trace[3].gate == "cooldown");
    CHECK(r.trace[4].gate == "cooldown");
    CHECK(r.trace[5].pending_entry);
}
TEST_CASE("Holding deadline overrides an already delayed rule exit") {
    auto c = timing_config();
    c.strategy.entry.rules[0].value = 0;
    c.strategy.exit = c.strategy.entry;
    c.signal_delay = 3;
    c.max_holding_bars = 2;
    const auto bars = timing_bars({101, 101, 101, 101, 101, 101, 101});
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(fill_times(r, true) == std::vector<std::int64_t>{bars[4].time});
    CHECK(fill_times(r, false) == std::vector<std::int64_t>{bars[6].time});
    CHECK(r.trace[4].exit_wait == 3);
    CHECK(r.trace[5].exit_wait == 0);
    CHECK(r.trace[6].fills[0].reason == "time exit: next open");
}
TEST_CASE("Protective exit with no volume is retried next open without inherited signal delay") {
    auto c = timing_config();
    c.strategy.entry.rules[0].value = 0;
    c.strategy.exit = c.strategy.entry;
    c.signal_delay = 3;
    c.stop_fraction = .02;
    c.participation = .001;
    auto bars = timing_bars({101, 101, 101, 101, 101, 101, 101});
    bars[4].volume = 0;
    bars[5].low = 90;
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(fill_times(r, true) == std::vector<std::int64_t>{bars[4].time});
    CHECK(fill_times(r, false) == std::vector<std::int64_t>{bars[6].time});
    CHECK(r.trace[5].fills[0].quantity == 0);
    CHECK(r.trace[5].exit_wait == 0);
}
TEST_CASE("Risk exits do not wait for confirmation or cooldown") {
    auto c = timing_config();
    c.strategy.buy_hold = true;
    c.entry_confirmation = c.exit_confirmation = 50;
    c.cooldown_bars = 500;
    c.max_drawdown = .1;
    const auto bars = timing_bars({100, 60, 50});
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(r.halted);
    CHECK(fill_times(r, false) == std::vector<std::int64_t>{bars[2].time});
    CHECK(r.trace[1].gate == "halt");
}
TEST_CASE("Execution trace reconciles manual cash fees rounding and final liquidation") {
    auto c = timing_config();
    c.strategy.buy_hold = true;
    c.liquidate_end = true;
    c.commission_rate = .001;
    c.fixed_fee = 1;
    c.spread_bps = 10;
    c.slippage_bps = 5;
    c.tick_size = .05;
    const auto bars = timing_bars({100, 110});
    const auto r = quant::backtest(bars, c, 0, true);
    REQUIRE(r.trace.size() == 2);
    const auto &buy = r.trace[0].fills[0];
    CHECK(buy.quantity == 49);
    CHECK(buy.price == doctest::Approx(100.1));
    CHECK(buy.fee == doctest::Approx(5.9049));
    CHECK(buy.cost == doctest::Approx(4.9));
    CHECK(buy.cash_after == doctest::Approx(5089.1951));
    const auto &sell = r.trace[1].fills[0];
    CHECK(sell.quantity == -49);
    CHECK(sell.price == doctest::Approx(109.85));
    CHECK(sell.fee == doctest::Approx(6.38265));
    CHECK(sell.cost == doctest::Approx(7.35));
    CHECK(r.cash == doctest::Approx(10465.46245));
    CHECK(buy.fee + sell.fee == doctest::Approx(r.metrics.fees));
    CHECK(buy.cost + sell.cost == doctest::Approx(r.metrics.execution_cost));
    CHECK(r.trace.back().cash_after == r.cash);
    CHECK(r.trace.back().gate == "predefined end");
    CHECK(quant::backtest(bars, c).trace.empty());
}
TEST_CASE("Temporal execution and trace are prefix causal and match incremental paper replay") {
    auto c = timing_config();
    c.entry_confirmation = 2;
    c.exit_confirmation = 2;
    c.cooldown_bars = 2;
    c.max_holding_bars = 3;
    auto bars = timing_bars({101, 102, 103, 104, 105, 106, 107, 108, 109, 110});
    const auto original = quant::backtest(bars, c, 0, true);
    quant::PaperEngine paper(c, true);
    for (const auto &bar : bars)
        paper.step(bar);
    const auto replay = paper.result();
    CHECK(replay.equity == original.equity);
    CHECK(replay.cash == original.cash);
    for (std::size_t i = 7; i < bars.size(); ++i)
        bars[i].open = bars[i].close = 1, bars[i].low = .5, bars[i].high = 2;
    const auto changed = quant::backtest(bars, c, 0, true);
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK(original.trace[i].cash_after == changed.trace[i].cash_after);
        CHECK(original.trace[i].units_after == changed.trace[i].units_after);
        CHECK(original.trace[i].entry_streak == changed.trace[i].entry_streak);
        CHECK(original.trace[i].fill_count == changed.trace[i].fill_count);
        CHECK(original.trace[i].gate == changed.trace[i].gate);
        CHECK(original.equity[i + 1] == changed.equity[i + 1]);
        for (int f = 0; f < original.trace[i].fill_count; ++f) {
            CHECK(original.trace[i].fills[f].price == changed.trace[i].fills[f].price);
            CHECK(original.trace[i].fills[f].quantity == changed.trace[i].fills[f].quantity);
            CHECK(original.trace[i].fills[f].reason == changed.trace[i].fills[f].reason);
        }
    }
}
TEST_CASE("Temporal settings reject excessive and negative resource bounds") {
    auto c = timing_config();
    c.entry_confirmation = 0;
    CHECK_THROWS(quant::PaperEngine(c));
    c.entry_confirmation = 51;
    CHECK_THROWS(quant::PaperEngine(c));
    c.entry_confirmation = 1;
    c.exit_confirmation = 51;
    CHECK_THROWS(quant::PaperEngine(c));
    c.exit_confirmation = 1;
    c.cooldown_bars = -1;
    CHECK_THROWS(quant::PaperEngine(c));
    c.cooldown_bars = 501;
    CHECK_THROWS(quant::PaperEngine(c));
    c.cooldown_bars = 0;
    c.max_holding_bars = 5001;
    CHECK_THROWS(quant::PaperEngine(c));
    c.max_holding_bars = -1;
    CHECK_THROWS(quant::PaperEngine(c));
}
TEST_CASE("Buy-and-hold baseline neutralizes strategy timing overlays") {
    auto c = timing_config();
    const auto bars = timing_bars({100, 101, 102, 103, 104, 105});
    const auto baseline = quant::buy_hold(bars, c);
    c.entry_confirmation = c.exit_confirmation = 50;
    c.max_holding_bars = 1;
    c.cooldown_bars = 500;
    CHECK(quant::buy_hold(bars, c).equity == baseline.equity);
}
