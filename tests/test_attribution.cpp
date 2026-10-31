#include "quant/attribution.hpp"
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
namespace {
constexpr std::int64_t hour = 3600000;
std::vector<quant::Bar> history(int n = 6) {
    std::vector<quant::Bar> bars;
    for (int i = 0; i < n; ++i) {
        const auto t = 1706659200000LL + i * 86400000LL; // 31 January 2024 UTC
        const double p = 100 + i;
        bars.push_back({t, t + hour, p, p + 10, p - 10, p, 10000, 10800});
    }
    return bars;
}
struct Fixture {
    std::vector<quant::Bar> bars = history();
    quant::SimResult r;
};
Fixture ledger() {
    Fixture f;
    f.r.equity = {1000};
    double cash = 1000;
    std::int64_t units = 0;
    for (int i = 0; i < 6; ++i) {
        quant::ExecutionTrace t;
        t.time = f.bars[i].time;
        t.units_before = units;
        t.cash_before = cash;
        std::int64_t q = 0;
        double price = 0, fee = 0;
        if (i == 0) {
            q = 4;
            price = 100;
            fee = 1;
        }
        if (i == 1) {
            q = -2;
            price = 110;
            fee = 1;
        }
        if (i == 2) {
            q = -2;
            price = 90;
            fee = 1;
        }
        if (i == 3) {
            q = 2;
            price = 100;
            fee = 1;
        }
        if (i == 4) {
            q = -1;
            price = 120;
            fee = 1;
        }
        units += q;
        cash -= q * price + fee;
        if (q) {
            t.fills[t.fill_count++] = {q, q, units, price, price, fee, 0, cash, "fixture"};
            ++f.r.metrics.fills;
            f.r.metrics.fees += fee;
        }
        t.units_after = units;
        t.cash_after = cash;
        t.equity = cash + units * f.bars[i].close;
        f.r.equity.push_back(t.equity);
        f.r.times.push_back(f.bars[i].end);
        f.r.trace.push_back(t);
    }
    f.r.units = units;
    f.r.cash = cash;
    f.r.metrics.closed_trades = 1;
    return f;
}
} // namespace
TEST_CASE("Attribution reconciles partial sales, complete trades and marked open remainder") {
    const auto f = ledger();
    const auto a = quant::performance_attribution(f.bars, f.r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    REQUIRE(a.positions.size() == 1);
    const auto &p = a.positions[0];
    CHECK(p.entry == 0);
    CHECK(p.exit == 2);
    CHECK(p.quantity == 4);
    CHECK(p.fills == 3);
    CHECK(p.entry_cost == 401);
    CHECK(p.fees == 3);
    CHECK(p.pnl == -3);
    CHECK(p.return_fraction == doctest::Approx(-3. / 401));
    CHECK(p.initial_capital_fraction == doctest::Approx(-.003));
    CHECK(a.closed_pnl == -3);
    CHECK(a.open_position_pnl == 23);
    CHECK(a.open_units == 1);
    CHECK(a.total_pnl == 20);
    CHECK(a.fees == 5);
    CHECK(a.reconciliation_error == doctest::Approx(0));
    CHECK(a.gross_profit == 0);
    CHECK(a.gross_loss == 3);
    CHECK(std::isnan(a.top_one_profit_share));
}
TEST_CASE("Month and equal-count sections telescope and retain carried positions") {
    const auto f = ledger();
    const auto a = quant::performance_attribution(f.bars, f.r);
    REQUIRE(a.months.size() == 2);
    CHECK(a.months[0].key == 202401);
    CHECK(a.months[1].key == 202402);
    CHECK(a.months[0].starting_equity == 1000);
    CHECK(a.months[0].ending_equity == 999);
    CHECK(a.months[0].pnl == -1);
    CHECK(a.months[0].fees == 1);
    CHECK(a.months[1].starting_equity == 999);
    CHECK(a.months[1].pnl == 21);
    CHECK(a.months[1].fees == 4);
    CHECK(a.months[1].return_fraction == doctest::Approx(21. / 999));
    CHECK(a.months[0].max_drawdown == doctest::Approx(.001));
    REQUIRE(a.sections.size() == 3);
    CHECK(a.sections[0].begin == 0);
    CHECK(a.sections[0].end == 2);
    CHECK(a.sections[0].pnl == 20);
    CHECK(a.sections[1].pnl == -18);
    CHECK(a.sections[2].pnl == 18);
    CHECK(a.sections[0].pnl + a.sections[1].pnl + a.sections[2].pnl == a.total_pnl);
    CHECK(a.sections[1].starting_equity == 1020);
}
TEST_CASE("Opening month uses local candle start, leap years and historic offsets") {
    auto b = history(1)[0];
    b.time = 1706738400000LL;
    b.end = b.time + hour; // 31 Jan 22:00 UTC -> February in Moscow
    CHECK(quant::opening_month(b) == 202402);
    b.utc_offset_seconds = -10800;
    CHECK(quant::opening_month(b) == 202401);
    for (const auto &sample :
         std::vector<std::pair<std::int64_t, int>>{{951782400000LL, 200002},
                                                   {951868800000LL, 200003}, // 2000 leap year
                                                   {4107456000000LL, 210002},
                                                   {4107542400000LL, 210003}, // 2100 is not leap
                                                   {1735689600000LL, 202501}}) {
        b.time = sample.first;
        b.end = b.time + hour;
        b.utc_offset_seconds = 0;
        CHECK(quant::opening_month(b) == sample.second);
    }
    b.time = 1706659200000LL;
    b.end = b.time + 86400000;
    b.utc_offset_seconds = 0;
    CHECK(quant::opening_month(b) == 202401); // end is February, start remains January
}
TEST_CASE("Missing or corrupted trace never fabricates position attribution") {
    auto f = ledger();
    SUBCASE("no trace") { f.r.trace.clear(); }
    SUBCASE("truncated trace") { f.r.trace.pop_back(); }
    SUBCASE("cash mismatch") { f.r.trace[1].fills[0].cash_after += 10; }
    SUBCASE("quantity overflow") {
        f.r.trace[1].fills[0].quantity = std::numeric_limits<std::int64_t>::min();
    }
    SUBCASE("oversell") { f.r.trace[1].fills[0].quantity = -5; }
    SUBCASE("position mismatch") { f.r.trace[2].units_after += 1; }
    SUBCASE("commission mismatch") { f.r.metrics.fees += 1; }
    const auto a = quant::performance_attribution(f.bars, f.r);
    CHECK(a.trace_status != quant::AttributionTrace::Ready);
    CHECK(a.positions.empty());
    CHECK(std::isnan(a.closed_pnl));
    CHECK(std::isnan(a.months[0].fees));
    CHECK(a.total_pnl == 20);
    CHECK(a.months.size() == 2);
}
TEST_CASE("Attribution preserves negative capital without invented period percentages") {
    auto bars = history(3);
    quant::SimResult r;
    r.equity = {100, -20, -30, 10};
    for (const auto &b : bars)
        r.times.push_back(b.end);
    const auto a = quant::performance_attribution(bars, r);
    CHECK(a.sections[0].return_fraction == doctest::Approx(-1.2));
    CHECK(a.sections[0].max_drawdown == doctest::Approx(1.2));
    CHECK(std::isnan(a.sections[1].return_fraction));
    CHECK(a.sections[1].pnl == -10);
    CHECK(a.total_pnl == -90);
}
TEST_CASE("Actual backtest attribution agrees with full and incremental execution including costs") {
    auto bars = history(200);
    quant::SimConfig c;
    c.strategy.fast = 2;
    c.strategy.slow = 4;
    c.strategy.buy_hold = true;
    c.stop_fraction = c.target_fraction = 0;
    c.partial_target_fraction = .02;
    c.partial_exit_fraction = .5;
    c.allocation = .8;
    c.max_drawdown = c.daily_loss = 1;
    auto r = quant::backtest(bars, c, 0, true);
    auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    REQUIRE(a.positions.size() == 1);
    CHECK(a.positions[0].fills >= 3);
    CHECK(a.open_units == 0);
    CHECK(a.closed_pnl == doctest::Approx(r.equity.back() - c.initial_cash));
    CHECK(a.fees == r.metrics.fees);
    quant::PaperEngine engine(c, true);
    for (const auto &b : bars)
        engine.step(b);
    engine.finish();
    CHECK(quant::performance_attribution(bars, engine.result()).closed_pnl == a.closed_pnl);
    c.liquidate_end = false;
    r = quant::backtest(bars, c, 0, true);
    a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    CHECK(a.positions.empty());
    CHECK(a.open_units > 0);
    CHECK(a.open_position_pnl == doctest::Approx(r.equity.back() - c.initial_cash));
}
TEST_CASE("Attribution is bounded, cancellable and rejects misaligned inputs") {
    const auto bars = history(5000);
    quant::SimConfig c;
    const auto r = quant::backtest(bars, c, 0, true);
    const auto a = quant::performance_attribution(bars, r);
    CHECK(a.months.size() <= 5000);
    CHECK(a.positions.size() <= 5000);
    CHECK(a.sections.size() == 3);
    CHECK(a.trace_status == quant::AttributionTrace::Ready);
    std::atomic<bool> cancel{true};
    const auto stopped = quant::performance_attribution(bars, r, &cancel);
    CHECK(stopped.cancelled);
    CHECK(stopped.months.empty());
    CHECK(stopped.positions.empty());
    auto invalid = r;
    invalid.times[1] += 1;
    CHECK_THROWS(quant::performance_attribution(bars, invalid));
    invalid = r;
    invalid.equity[3] = std::numeric_limits<double>::infinity();
    CHECK_THROWS(quant::performance_attribution(bars, invalid));
    CHECK_THROWS(quant::performance_attribution({}, {}));
}
TEST_CASE("Concentration uses positive closed PnL and never divides by net profit") {
    const auto bars = history(10);
    quant::SimResult r;
    r.equity = {1000};
    double cash = 1000;
    std::int64_t units = 0;
    const double net[]{30, 20, 10, 5, -15};
    for (int i = 0; i < 10; ++i) {
        quant::ExecutionTrace t;
        t.time = bars[i].time;
        t.units_before = units;
        t.cash_before = cash;
        const std::int64_t q = i % 2 ? -1 : 1;
        const double price = i % 2 ? net[i / 2] + 102 : 100;
        units += q;
        cash -= q * price + 1;
        t.fill_count = 1;
        t.fills[0] = {q, q, units, price, price, 1, 0, cash, "fixture"};
        t.units_after = units;
        t.cash_after = cash;
        t.equity = cash + units * bars[i].close;
        r.equity.push_back(t.equity);
        r.times.push_back(bars[i].end);
        r.trace.push_back(t);
    }
    r.cash = cash;
    r.units = 0;
    r.metrics.fills = 10;
    r.metrics.closed_trades = 5;
    r.metrics.fees = 10;
    const auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    CHECK(a.closed_pnl == 50);
    CHECK(a.gross_profit == 65);
    CHECK(a.gross_loss == 15);
    CHECK(a.top_one_profit_share == doctest::Approx(30. / 65));
    CHECK(a.top_three_profit_share == doctest::Approx(60. / 65));
    double contribution = 0;
    for (const auto &p : a.positions)
        contribution += p.initial_capital_fraction;
    CHECK(contribution == doctest::Approx(.05));
}
TEST_CASE("Rejected fills and a missing bounded journal do not create fictitious trades") {
    auto f = ledger();
    f.r.journal.clear();
    auto &t = f.r.trace[0];
    t.fills[1] = t.fills[0];
    t.fills[0] = {4, 0, 0, 100, 100, 0, 0, 1000, "rejected"};
    t.fill_count = 2;
    const auto a = quant::performance_attribution(f.bars, f.r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    CHECK(a.positions[0].fills == 3);
    CHECK(a.positions.size() == 1);
    CHECK(a.fees == 5);
}
TEST_CASE("Same-bar stop and final close are attributed using actual model fills") {
    auto bars = history(1);
    bars[0].low = 90;
    bars[0].high = 110;
    quant::SimConfig c;
    c.strategy.buy_hold = true;
    c.stop_fraction = c.target_fraction = .05;
    const auto r = quant::backtest(bars, c, 0, true);
    const auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    REQUIRE(a.positions.size() == 1);
    CHECK(a.positions[0].entry == a.positions[0].exit);
    CHECK(a.positions[0].fills == 2);
    CHECK(a.closed_pnl == doctest::Approx(r.equity.back() - c.initial_cash));
}
TEST_CASE("Month gaps are not reported as zero-return months") {
    auto bars = history(2);
    bars[1].time += 60LL * 86400000;
    bars[1].end = bars[1].time + hour;
    quant::SimResult r;
    r.equity = {100, 110, 121};
    r.times = {bars[0].end, bars[1].end};
    const auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.months.size() == 2);
    CHECK(a.months[0].key == 202401);
    CHECK(a.months[1].key == 202404);
    CHECK(a.months[0].pnl + a.months[1].pnl == 21);
    CHECK((1 + a.months[0].return_fraction) * (1 + a.months[1].return_fraction) - 1 == doctest::Approx(.21));
}

TEST_CASE("Large cash and small net PnL expose rounding residual instead of dropping attribution") {
    auto bars = history(100);
    for (auto &b : bars)
        b.open = b.close = 100, b.high = 101, b.low = 99;
    quant::SimConfig c;
    c.initial_cash = 1e12;
    c.allocation = 1e-9;
    c.strategy.buy_hold = true;
    c.stop_fraction = c.target_fraction = c.commission_rate = c.spread_bps = c.slippage_bps = 0;
    c.fixed_fee = .01;
    const auto r = quant::backtest(bars, c, 0, true);
    const auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    CHECK(a.closed_pnl == doctest::Approx(-.02));
    CHECK(a.closed_pnl + a.open_position_pnl + a.reconciliation_error == doctest::Approx(a.total_pnl));
    CHECK(std::abs(a.reconciliation_error) < .01);
}
TEST_CASE("Thousands of completed trades remain bounded and reconcile with the account") {
    auto bars = history(5000);
    for (auto &b : bars)
        b.open = b.close = 100, b.high = 101, b.low = 99;
    quant::SimConfig c;
    c.strategy.entry = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, true}},
                        true};
    c.strategy.exit = {};
    c.max_holding_bars = 1;
    c.stop_fraction = c.target_fraction = 0;
    c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = 0;
    const auto r = quant::backtest(bars, c, 0, true);
    const auto a = quant::performance_attribution(bars, r);
    REQUIRE(a.trace_status == quant::AttributionTrace::Ready);
    CHECK(a.positions.size() >= 1000);
    CHECK(a.positions.size() <= 5000);
    CHECK(a.closed_pnl == 0);
    CHECK(a.open_position_pnl == 0);
    CHECK(a.total_pnl == 0);
    CHECK(a.months.size() > 120);
}
