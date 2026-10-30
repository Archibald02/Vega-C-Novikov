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
TEST_CASE("Portfolio incremental snapshots preserve the ledger and match each batch prefix") {
    auto c = plain();
    c.rebalance_every = 2;
    c.commission_rate = .0005;
    auto a = asset("A", .4, {100, 101, 95, 105, 99, 110});
    auto b = asset("B", .4, {100, 99, 102, 98, 101, 100});
    const auto schedule = quant::portfolio_schedule({b, a});
    quant::PortfolioAccount account({b, a}, c);
    CHECK(account.result().common_bars == 0);
    CHECK(account.result().cash == 100000);
    for (std::size_t i = 0; i < schedule.frames.size(); ++i) {
        account.step(schedule.frames[i]);
        const auto first = account.result(), second = account.result();
        CHECK(first.equity == second.equity);
        CHECK(first.positions[0].pnl == second.positions[0].pnl);
        if (i == 0)
            continue;
        auto aa = a, bb = b;
        aa.bars.resize(i + 1);
        bb.bars.resize(i + 1);
        const auto reference = quant::portfolio_backtest({aa, bb}, c);
        CHECK(first.equity == reference.equity);
        CHECK(first.drawdowns == reference.drawdowns);
        CHECK(first.cash == reference.cash);
        CHECK(first.positions[0].pnl == reference.positions[0].pnl);
        CHECK(first.journal.size() == reference.journal.size());
    }
}
TEST_CASE("Portfolio incremental rejects duplicate malformed unsynchronised and reversed frames atomically") {
    auto a = asset("A", .5, {100, 100, 110}), b = asset("B", .5, {100, 100, 90});
    auto s = quant::portfolio_schedule({a, b});
    quant::PortfolioAccount account({a, b}, plain());
    account.step(s.frames[0]);
    const auto before = account.result();
    CHECK_THROWS(account.step(s.frames[0]));
    auto bad = s.frames[1];
    bad[1].low = 1e6;
    CHECK_THROWS(account.step(bad));
    bad = s.frames[1];
    ++bad[1].end;
    CHECK_THROWS(account.step(bad));
    bad = s.frames[1];
    bad[1].utc_offset_seconds = 0;
    CHECK_THROWS(account.step(bad));
    CHECK_THROWS(account.step({s.frames[1][0]}));
    CHECK(account.result().cash == before.cash);
    CHECK(account.result().equity == before.equity);
    CHECK(account.result().journal.size() == before.journal.size());
    account.step(s.frames[1]);
    CHECK(account.result().positions[0].units == 500);
    CHECK(account.result().positions[1].units == 500);
    CHECK_THROWS(account.step(s.frames[0]));
}
TEST_CASE("Portfolio manual emergency stop is permanent and exits at next common open") {
    auto a = asset("A", .5, {100, 100, 80}), b = asset("B", .5, {100, 100, 90});
    const auto s = quant::portfolio_schedule({a, b});
    quant::PortfolioAccount account({a, b}, plain());
    account.step(s.frames[0]);
    account.step(s.frames[1]);
    account.request_stop();
    const auto stopped = account.result();
    CHECK(stopped.halted);
    CHECK(stopped.positions[0].units == 500);
    account.request_stop();
    CHECK(account.result().journal.size() == stopped.journal.size());
    account.step(s.frames[2]);
    const auto r = account.result();
    CHECK(r.cash == doctest::Approx(85000));
    CHECK(r.positions[0].units == 0);
    CHECK(r.positions[1].units == 0);
    CHECK(r.halt_reason == "MANUAL_STOP");
    CHECK(r.journal.back().reason == "RISK_EXIT");
    CHECK(r.total_return == doctest::Approx(-.15));
    quant::PortfolioAccount early({a, b}, plain());
    early.request_stop();
    early.step(s.frames[0]);
    early.step(s.frames[1]);
    CHECK(early.result().fills == 0);
}
TEST_CASE("Portfolio warmup never opens historical positions or credits historical profits") {
    auto a = asset("A", .5, {100, 200, 400, 500});
    quant::PortfolioAccount account({a}, plain());
    account.warmup({a.bars[0]});
    account.warmup({a.bars[1]});
    CHECK(account.result().common_bars == 0);
    CHECK(account.result().fills == 0);
    CHECK(account.result().equity.size() == 1);
    CHECK(account.result().cash == 100000);
    CHECK_THROWS(account.step({a.bars[2]}));
    account.arm_from_warmup();
    CHECK_THROWS(account.arm_from_warmup());
    CHECK_THROWS(account.warmup({a.bars[2]}));
    account.step({a.bars[2]});
    auto r = account.result();
    CHECK(r.positions[0].units == 250);
    CHECK(r.equity.back() == 100000);
    CHECK(r.total_return == 0);
    CHECK(r.start_time == a.bars[1].end);
    account.step({a.bars[3]});
    CHECK(account.result().equity.back() == 125000);
}
TEST_CASE("Portfolio incremental baseline equals batch and stops at the bounded session cap") {
    auto c = plain();
    c.rebalance_every = 2;
    auto a = asset("A", .7, {100, 105, 110}), b = asset("B", .2, {100, 99, 98});
    auto account = quant::portfolio_baseline_account({b, a}, c);
    for (const auto &frame : quant::portfolio_schedule({a, b}).frames)
        account.step(frame);
    CHECK(account.result().equity == quant::portfolio_baseline({a, b}, c).equity);
    a = asset("A", .5, std::vector<double>(5000, 100));
    quant::PortfolioAccount bounded({a}, c);
    for (const auto &bar : a.bars)
        bounded.step({bar});
    auto extra = a.bars.back();
    extra.time = extra.end;
    extra.end += 86400000;
    CHECK_THROWS(bounded.step({extra}));
    CHECK(bounded.result().common_bars == 5000);
    CHECK(bounded.result().equity.size() == 5001);
    quant::PortfolioAccount warmed({a}, c);
    for (const auto &bar : a.bars)
        warmed.warmup({bar});
    CHECK_THROWS(warmed.warmup({extra}));
    CHECK(warmed.result().equity.size() == 1);
    CHECK(warmed.result().fills == 0);
    warmed.arm_from_warmup();
    warmed.step({extra});
    CHECK(warmed.result().common_bars == 1);
}
