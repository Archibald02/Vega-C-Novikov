#include "quant/research.hpp"
#include <doctest/doctest.h>
#include <limits>
namespace {
quant::SimConfig partial_config() {
    quant::SimConfig c;
    c.initial_cash = 10000;
    c.allocation = 1;
    c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = 0;
    c.stop_fraction = c.target_fraction = 0;
    c.max_drawdown = c.daily_loss = c.risk_per_trade = 1;
    c.liquidate_end = false;
    c.strategy.buy_hold = true;
    c.strategy.exit = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Below, 95, true}},
                       true};
    c.partial_target_fraction = .05;
    c.partial_exit_fraction = .5;
    return c;
}
std::vector<quant::Bar> prices(std::initializer_list<double> values) {
    std::vector<quant::Bar> bars;
    for (double p : values) {
        const auto t = 1700000000000LL + static_cast<std::int64_t>(bars.size()) * 86400000;
        bars.push_back({t, t + 86400000, p, p + 1, p - 1, p, 10000});
    }
    return bars;
}
} // namespace
TEST_CASE("One planned slice leaves position alive and final closure counts a single trade") {
    auto c = partial_config();
    c.lot = 10;
    c.cooldown_bars = 5;
    const auto bars = prices({100, 110, 120, 120, 90, 95});
    const auto r = quant::backtest(bars, c, 0, true);
    REQUIRE(r.trace[2].fill_count == 1);
    CHECK(r.trace[1].partial_remaining == 50);
    CHECK(r.trace[2].fills[0].quantity == -50);
    CHECK(r.trace[2].units_after == 50);
    CHECK(r.trace[2].cooldown_remaining == 0);
    CHECK(r.trace[3].fill_count == 0);
    CHECK(r.metrics.partial_fills == 1);
    CHECK(r.metrics.closed_trades == 1);
    CHECK(r.metrics.fills == 3);
    CHECK(r.cash == doctest::Approx(10750));
    CHECK(r.trace[5].cooldown_remaining == 5);
}
TEST_CASE("Manual commission and allocated entry cost reconcile through partial and final exit") {
    auto c = partial_config();
    c.commission_rate = .001;
    c.fixed_fee = 1;
    auto bars = prices({100, 110, 120});
    quant::PaperEngine paper(c, true);
    for (const auto &b : bars)
        paper.step(b);
    const auto open = paper.result();
    CHECK(open.units == 50);
    CHECK(open.cash == doctest::Approx(5962.22));
    CHECK(open.unrealized == doctest::Approx(994.4949494949));
    CHECK(open.metrics.closed_trades == 0);
    c.liquidate_end = true;
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(r.units == 0);
    CHECK(r.cash == doctest::Approx(11955.22));
    CHECK(r.metrics.fees == doctest::Approx(24.78));
    CHECK(r.metrics.turnover == doctest::Approx(2.178));
    CHECK(r.metrics.closed_trades == 1);
    CHECK(r.metrics.win_rate == 1);
    CHECK(r.metrics.partial_fills == 1);
    CHECK(r.trace.back().partial_remaining == 0);
}
TEST_CASE("A sub-lot plan is skipped once without forcing a larger sale or charging fees") {
    auto c = partial_config();
    c.initial_cash = 2500;
    c.lot = 10;
    c.partial_exit_fraction = .01;
    const auto r = quant::backtest(prices({100, 110, 120, 130, 140}), c, 0, true);
    CHECK(r.units == 20);
    CHECK(r.metrics.fills == 1);
    CHECK(r.metrics.partial_fills == 0);
    int skips = 0;
    for (const auto &e : r.journal)
        if (e.reason.find("partial exit skipped") == 0)
            ++skips;
    CHECK(skips == 1);
    CHECK(r.trace[1].partial_complete);
}
TEST_CASE("A volume-limited stage retries only its fixed remainder and charges nothing on no fill") {
    auto c = partial_config();
    c.participation = .1;
    auto warm = prices({100})[0];
    warm.volume = 1000;
    auto bars = prices({110, 110, 110, 110, 110});
    for (auto &b : bars) {
        b.time += 86400000;
        b.end += 86400000;
    }
    bars[0].open = 100;
    bars[0].low = 99;
    bars[0].volume = 200;
    bars[1].volume = 100;
    bars[2].volume = 0;
    bars[3].volume = 1000;
    quant::PaperEngine paper(c, true);
    paper.warmup(warm);
    paper.arm_from_warmup();
    for (const auto &b : bars)
        paper.step(b);
    const auto r = paper.result();
    CHECK(r.trace[0].partial_remaining == 50);
    CHECK(r.trace[1].partial_remaining == 30);
    CHECK(r.trace[2].partial_remaining == 20);
    CHECK(r.trace[3].fills[0].quantity == 0);
    CHECK(r.trace[3].fills[0].fee == 0);
    CHECK(r.trace[4].partial_remaining == 0);
    CHECK(r.units == 50);
    CHECK(r.cash == 5500);
    CHECK(r.metrics.partial_fills == 3);
    CHECK(r.metrics.closed_trades == 0);
}
TEST_CASE("An opening stop gap has priority over the previously scheduled partial slice") {
    auto c = partial_config();
    c.stop_fraction = .05;
    const auto r = quant::backtest(prices({100, 110, 90}), c, 0, true);
    CHECK(r.trace[1].partial_remaining == 50);
    CHECK(r.metrics.partial_fills == 0);
    CHECK(r.trace[2].fills[0].quantity == -100);
    CHECK(r.trace[2].fills[0].reason == "stop gap: opening price");
    CHECK(r.partial_remaining == 0);
}
TEST_CASE("Known opening partial precedes a later intrabar stop on the remaining shares") {
    auto c = partial_config();
    c.stop_fraction = .05;
    auto bars = prices({100, 110, 100});
    bars[2].low = 90;
    const auto r = quant::backtest(bars, c, 0, true);
    REQUIRE(r.trace[2].fill_count == 2);
    CHECK(r.trace[2].fills[0].quantity == -50);
    CHECK(r.trace[2].fills[0].price == 100);
    CHECK(r.trace[2].fills[1].quantity == -50);
    CHECK(r.trace[2].fills[1].price == 95);
    CHECK(r.cash == 9750);
    CHECK(r.metrics.closed_trades == 1);
}
TEST_CASE("Manual stop and holding deadline cancel the pending partial order") {
    auto c = partial_config();
    auto bars = prices({100, 110, 120});
    quant::PaperEngine paper(c, true);
    paper.step(bars[0]);
    paper.step(bars[1]);
    paper.request_stop();
    paper.step(bars[2]);
    CHECK(paper.result().metrics.partial_fills == 0);
    CHECK(paper.result().units == 0);
    c.max_holding_bars = 2;
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(r.metrics.partial_fills == 0);
    CHECK(r.units == 0);
}
TEST_CASE("Ordinary full exit cancels the delayed partial stage before it can fill") {
    auto c = partial_config();
    c.signal_delay = 2;
    c.strategy.exit = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 112, true}},
                       true};
    const auto r = quant::backtest(prices({100, 110, 115, 116, 117, 118}), c, 0, true);
    CHECK(r.trace[1].partial_remaining == 50);
    CHECK(r.trace[2].partial_remaining == 0);
    CHECK(r.metrics.partial_fills == 0);
    CHECK(r.units == 0);
}
TEST_CASE(
    "Partial-stage accounting is prefix causal, agrees with paper, and is neutral in buy-hold baseline") {
    auto c = partial_config();
    auto bars = prices({100, 110, 120, 130, 140, 150});
    const auto r = quant::backtest(bars, c, 0, true);
    quant::PaperEngine paper(c, true);
    for (const auto &b : bars)
        paper.step(b);
    CHECK(paper.result().equity == r.equity);
    auto changed = bars;
    for (std::size_t i = 3; i < changed.size(); ++i)
        changed[i].open = changed[i].close = 1, changed[i].low = .5, changed[i].high = 2;
    const auto future = quant::backtest(changed, c, 0, true);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(r.equity[i + 1] == future.equity[i + 1]);
        CHECK(r.trace[i].partial_remaining == future.trace[i].partial_remaining);
    }
    const auto baseline = quant::buy_hold(bars, c);
    c.partial_target_fraction = 0;
    CHECK(quant::buy_hold(bars, c).equity == baseline.equity);
    CHECK(quant::backtest(bars, c).metrics.partial_fills == 0);
}
TEST_CASE("Invalid partial fractions and targets are rejected before execution") {
    auto c = partial_config();
    for (double x : {0., 1., -1., std::numeric_limits<double>::quiet_NaN()}) {
        c.partial_exit_fraction = x;
        CHECK_THROWS(quant::PaperEngine(c));
    }
    c.partial_exit_fraction = .5;
    for (double x : {-1., 11., std::numeric_limits<double>::infinity()}) {
        c.partial_target_fraction = x;
        CHECK_THROWS(quant::PaperEngine(c));
    }
}
TEST_CASE("New entry resets the once-per-position stage") {
    auto c = partial_config();
    c.strategy.buy_hold = false;
    c.strategy.entry = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, true}},
                        true};
    const auto r = quant::backtest(prices({100, 100, 110, 120, 90, 100, 110, 120, 130}), c, 0, true);
    CHECK(r.metrics.partial_fills == 2);
    CHECK(r.metrics.closed_trades == 1);
    CHECK(r.trace[8].fills[0].quantity < 0);
    CHECK(r.units > 0);
}
TEST_CASE("Opening full target gap also cancels the pending partial stage") {
    auto c = partial_config();
    c.target_fraction = .2;
    const auto r = quant::backtest(prices({100, 110, 125}), c, 0, true);
    CHECK(r.metrics.partial_fills == 0);
    CHECK(r.units == 0);
    CHECK(r.trace[2].fills[0].reason == "target gap: market trigger at open");
}
TEST_CASE("Profitable first slice does not turn a net losing position into a winning trade") {
    auto c = partial_config();
    const auto r = quant::backtest(prices({100, 110, 110, 80, 80}), c);
    CHECK(r.cash == 9500);
    CHECK(r.metrics.partial_fills == 1);
    CHECK(r.metrics.closed_trades == 1);
    CHECK(r.metrics.win_rate == 0);
    CHECK(r.metrics.profit_factor == 0);
}
TEST_CASE("Partial trigger uses close only and its next-open fill is not a guaranteed profit") {
    auto c = partial_config();
    auto bars = prices({100, 104, 105, 90});
    bars[0].high = 200;
    bars[1].high = 200;
    const auto r = quant::backtest(bars, c, 0, true);
    CHECK(r.trace[0].partial_remaining == 0);
    CHECK(r.trace[1].partial_remaining == 0);
    CHECK(r.trace[2].partial_remaining == 50);
    CHECK(r.trace[3].fills[0].price == 90);
    CHECK(r.trace[3].fills[0].quantity == -50);
}
