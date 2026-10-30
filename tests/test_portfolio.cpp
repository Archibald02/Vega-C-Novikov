#include "quant/portfolio.hpp"
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <numeric>
namespace {
quant::PortfolioAsset asset(const std::string &name, double weight, const std::vector<double> &prices) {
    quant::PortfolioAsset a;
    a.symbol = name;
    a.weight = weight;
    for (std::size_t i = 0; i < prices.size(); ++i) {
        const auto t = 1700000000000LL + static_cast<std::int64_t>(i) * 86400000LL;
        const auto p = prices[i];
        a.bars.push_back({t, t + 86400000, p, p, p, p, 10000, 10800});
    }
    return a;
}
quant::PortfolioConfig plain() {
    quant::PortfolioConfig c;
    c.commission_rate = c.spread_bps = c.slippage_bps = 0;
    c.max_weight = c.max_invested = c.max_drawdown = c.daily_loss = 1;
    c.rebalance_every = 0;
    return c;
}
} // namespace
TEST_CASE("Portfolio uses one cash ledger and next-open execution") {
    auto c = plain();
    auto a = asset("A", .5, {100, 100, 110}), b = asset("B", .5, {100, 100, 90});
    const auto r = quant::portfolio_backtest({b, a}, c);
    REQUIRE(r.positions.size() == 2);
    CHECK(r.positions[0].symbol == "A");
    CHECK(r.positions[0].units == 500);
    CHECK(r.positions[1].units == 500);
    CHECK(r.cash == doctest::Approx(0));
    CHECK(r.equity[1] == doctest::Approx(100000));
    CHECK(r.equity.back() == doctest::Approx(100000));
    CHECK(r.positions[0].pnl == doctest::Approx(5000));
    CHECK(r.positions[1].pnl == doctest::Approx(-5000));
    CHECK(r.journal[1].time == a.bars[1].time);
    const auto same = quant::portfolio_backtest({a, b}, c);
    CHECK(r.equity == same.equity);
    CHECK(r.cash == same.cash);
}
TEST_CASE("Portfolio gaps cannot spend the same cash twice") {
    auto c = plain();
    c.max_weight = .6;
    const auto r =
        quant::portfolio_backtest({asset("A", .5, {100, 200, 200}), asset("B", .5, {100, 100, 100})}, c);
    CHECK(r.positions[0].units == 300);
    CHECK(r.positions[1].units == 400);
    CHECK(r.cash >= 0);
    CHECK(r.positions[0].value / r.equity.back() <= .6 + 1e-12);
}
TEST_CASE("Portfolio attribution includes commissions and adverse fills exactly once") {
    auto c = plain();
    c.commission_rate = .001;
    c.fixed_fee = 5;
    c.spread_bps = 10;
    c.slippage_bps = 5;
    c.rebalance_every = 1;
    auto a = asset("A", .4, {100, 100, 110, 90}), b = asset("B", .4, {100, 100, 90, 120});
    a.lot = 10;
    a.tick_size = .1;
    b.lot = 5;
    b.tick_size = .1;
    const auto r = quant::portfolio_backtest({a, b}, c);
    double pnl = 0, fees = 0;
    for (const auto &p : r.positions) {
        pnl += p.pnl;
        fees += p.fees;
    }
    CHECK(pnl == doctest::Approx(r.equity.back() - c.initial_cash));
    CHECK(fees == doctest::Approx(r.fees));
    CHECK(r.cash >= 0);
    for (const auto &e : r.journal)
        if (e.quantity) {
            CHECK(e.fee == doctest::Approx(std::abs(e.quantity) * e.price * .001 + 5));
            CHECK(e.cash >= 0);
        }
}
TEST_CASE("Portfolio risk halt exits at next observed common open") {
    auto c = plain();
    c.max_drawdown = .09;
    auto a = asset("A", .5, {100, 100, 70});
    a.bars[1].close = a.bars[1].low = 80;
    const auto r = quant::portfolio_backtest({a}, c);
    CHECK(r.halted);
    CHECK(r.halt_reason == "DRAWDOWN");
    CHECK(r.positions[0].units == 0);
    CHECK(r.equity.back() == doctest::Approx(85000));
    auto last = a;
    last.bars.resize(2);
    const auto pending = quant::portfolio_backtest({last}, c);
    CHECK(pending.halted);
    CHECK(pending.positions[0].units == 500);
}
TEST_CASE("Portfolio is causal across future prices and uses explicit intersection") {
    auto c = plain();
    c.rebalance_every = 1;
    auto a = asset("A", .4, {100, 101, 103, 105, 100}), b = asset("B", .4, {100, 99, 98, 105, 100});
    const auto before = quant::portfolio_backtest({a, b}, c);
    a.bars[4].open = a.bars[4].high = a.bars[4].low = a.bars[4].close = 200;
    const auto after = quant::portfolio_backtest({a, b}, c);
    CHECK(std::equal(before.equity.begin(), before.equity.begin() + 5, after.equity.begin()));
    b.bars.erase(b.bars.begin() + 2);
    const auto sparse = quant::portfolio_backtest({a, b}, c);
    CHECK(sparse.common_bars == 4);
    CHECK(sparse.excluded[0] == 1);
    CHECK(sparse.excluded[1] == 0);
    for (const auto &e : sparse.journal)
        CHECK((e.time != a.bars[2].time || e.quantity == 0));
}
TEST_CASE("Portfolio rejects bad limits and handles groups and cancellation") {
    auto c = plain();
    auto a = asset("A", .4, {100, 100, 100}), b = asset("B", .4, {100, 100, 100});
    CHECK_THROWS(quant::portfolio_backtest({a, a}, c));
    c.group_limit = .6;
    CHECK_THROWS(quant::portfolio_backtest({a, b}, c));
    a.group = b.group = "same";
    CHECK_THROWS(quant::portfolio_backtest({a, b}, c));
    a.group = "one";
    b.group = "two";
    CHECK_NOTHROW(quant::portfolio_backtest({a, b}, c));
    std::atomic<bool> stop{true};
    CHECK_THROWS(quant::portfolio_backtest({a, b}, c, &stop));
    a.strategy.slow = 1000000000;
    CHECK_THROWS(quant::portfolio_backtest({a, b}, c));
}

TEST_CASE("Portfolio cannot invent a sale when fees or tick rounding prevent execution") {
    auto c = plain();
    c.max_drawdown = .1;
    auto a = asset("A", 1, {100, 100, 50});
    a.tick_size = 100;
    a.bars[1].close = a.bars[1].low = 50;
    const auto zero = quant::portfolio_backtest({a}, c);
    CHECK(zero.halted);
    CHECK(zero.positions[0].units == 1000);
    CHECK(zero.cash == 0);
    CHECK(zero.journal.back().reason == "SELL_BLOCKED_PRICE");
    c.initial_cash = 1000;
    c.fixed_fee = 99;
    a = asset("A", 1, {100, 100, 1});
    a.bars[1].close = a.bars[1].low = 90;
    const auto fee = quant::portfolio_backtest({a}, c);
    CHECK(fee.halted);
    CHECK(fee.positions[0].units == 9);
    CHECK(fee.cash == doctest::Approx(1));
    CHECK(fee.journal.back().reason == "SELL_BLOCKED_FEE");
}
TEST_CASE("Portfolio rule sleeves share cash and manual stop stays causal") {
    auto c = plain();
    auto a = asset("A", .5, {100, 100, 100, 100}), b = asset("B", .5, {100, 100, 100, 100});
    a.use_rules = b.use_rules = true;
    a.strategy.entry = {
        {quant::Rule{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, true}}, true};
    a.strategy.exit = a.strategy.entry;
    b.strategy = a.strategy;
    const auto r = quant::portfolio_backtest({a, b}, c);
    for (const auto &e : r.journal)
        CHECK(e.cash >= 0);
    CHECK(r.fills >= 4);
    CHECK(
        std::any_of(r.journal.begin(), r.journal.end(), [](const auto &e) { return e.reason == "RULE_ON"; }));
    CHECK(std::any_of(r.journal.begin(), r.journal.end(),
                      [](const auto &e) { return e.reason == "RULE_OFF"; }));
    c.manual_stop_after = 1;
    const auto stopped = quant::portfolio_backtest({a, b}, c);
    CHECK(stopped.halted);
    CHECK(stopped.halt_reason == "MANUAL_STOP");
    CHECK(stopped.fills == 0);
}
TEST_CASE("Portfolio bounds six long histories and reports a truncated journal") {
    auto c = plain();
    c.rebalance_every = 1;
    std::vector<double> prices;
    for (int i = 0; i < 5000; ++i)
        prices.push_back(100 + 20 * std::sin(i * .9));
    std::vector<quant::PortfolioAsset> assets;
    for (int j = 0; j < 6; ++j)
        assets.push_back(asset("A" + std::to_string(j), .15, prices));
    const auto r = quant::portfolio_backtest(assets, c);
    CHECK(r.common_bars == 5000);
    CHECK(r.equity.size() == 5001);
    CHECK(r.journal.size() == 10000);
    CHECK(r.dropped_events > 0);
    CHECK(r.cash >= 0);
    double pnl = 0;
    for (const auto &p : r.positions)
        pnl += p.pnl;
    CHECK(pnl == doctest::Approx(r.equity.back() - c.initial_cash));
}

TEST_CASE("Portfolio reports unavailable rule warmup rather than treating it as a successful signal") {
    auto a = asset("A", .5, {100, 101});
    a.use_rules = true;
    const auto r = quant::portfolio_backtest({a}, plain());
    CHECK_FALSE(r.positions[0].rules_ready);
    CHECK(r.fills == 0);
}

TEST_CASE("Portfolio cannot manufacture common observations") {
    auto a = asset("A", .4, {100, 101, 102}), b = asset("B", .4, {100, 101, 102});
    for (auto &bar : b.bars) {
        ++bar.time;
        ++bar.end;
    }
    CHECK_THROWS(quant::portfolio_backtest({a, b}, plain()));
}
