#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "quant/research.hpp"
#include "quant/robustness.hpp"
#include <doctest/doctest.h>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
using namespace quant;
namespace {
std::vector<Bar> bars(const std::vector<double> &prices) {
    std::vector<Bar> result;
    std::int64_t t = 1700000000000LL;
    for (double p : prices) {
        result.push_back({t, t + 86400000, p, p + 1, p - 1, p, 10000});
        t += 86400000;
    }
    return result;
}
SimConfig base() {
    SimConfig c;
    c.initial_cash = 1000;
    c.allocation = 1;
    c.commission_rate = 0;
    c.spread_bps = 0;
    c.slippage_bps = 0;
    c.stop_fraction = 0;
    c.target_fraction = 0;
    c.max_drawdown = 1;
    c.daily_loss = 1;
    c.risk_per_trade = 1;
    c.strategy.buy_hold = true;
    return c;
}
} // namespace
TEST_CASE("Distribution quantiles histogram and log returns agree with elementary identities") {
    const auto d = distribution({100, 110, 99, 99}, 5);
    REQUIRE(d.simple.size() == 3);
    CHECK(d.simple[0] == doctest::Approx(.1));
    CHECK(d.simple[1] == doctest::Approx(-.1));
    CHECK(d.simple[2] == 0);
    CHECK(d.mean == doctest::Approx(0).epsilon(1e-12));
    CHECK(d.sd == doctest::Approx(.1));
    CHECK(d.median == 0);
    CHECK(d.q05 == doctest::Approx(-.09));
    CHECK(d.q95 == doctest::Approx(.09));
    CHECK(std::accumulate(d.bin_counts.begin(), d.bin_counts.end(), 0.0) == 3);
    CHECK(std::accumulate(d.log_returns.begin(), d.log_returns.end(), 0.0) == doctest::Approx(std::log(.99)));
    const auto flat = distribution({100, 100, 100});
    CHECK(flat.sd == 0);
    CHECK(std::isnan(flat.skew));
    CHECK_THROWS(distribution({0, 100}));
}
TEST_CASE("ADF KPSS ACF PACF match independently generated statsmodels fixture") {
    std::ifstream file(REFERENCE_PATH);
    REQUIRE(file.good());
    std::map<std::string, std::vector<double>> reference;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream input(line);
        std::string name;
        int index;
        double value;
        input >> name >> index >> value;
        reference[name].push_back(value);
    }
    REQUIRE(reference["x"].size() == 240);
    const auto a = acf(reference["x"], 20), p = pacf(reference["x"], 20);
    for (int i = 0; i <= 20; ++i) {
        CHECK(a[i] == doctest::Approx(reference["acf"][i]).epsilon(1e-10));
        CHECK(p[i] == doctest::Approx(reference["pacf"][i]).epsilon(1e-8));
    }
    const auto s = stationarity(reference["x"], 2, 4);
    CHECK(s.adf == doctest::Approx(reference["adf"][0]).epsilon(1e-9));
    CHECK(s.adf_critical5 == doctest::Approx(reference["crit5"][0]).epsilon(1e-12));
    CHECK(s.kpss == doctest::Approx(reference["kpss"][0]).epsilon(1e-10));
    CHECK(s.observations == 237);
    const auto flat = stationarity(std::vector<double>(100, 7));
    CHECK(std::isnan(flat.adf));
    CHECK(std::isnan(flat.kpss));
    CHECK_THROWS(stationarity({1, 2, 3}));
    CHECK_THROWS(acf({1, 2, 3}, 3));
}
TEST_CASE("Candle indicators have causal warmup and ATR NATR use OHLC") {
    std::vector<double> prices;
    for (int i = 0; i < 80; ++i)
        prices.push_back(100 + i);
    auto b = bars(prices);
    const auto f = candle_features(b, 5, 20, 20);
    auto ix = [](Feature x) { return static_cast<std::size_t>(x); };
    CHECK(std::isnan(f[12][ix(Feature::Atr)]));
    CHECK(std::isnan(f[13][ix(Feature::Atr)]));
    CHECK(f[14][ix(Feature::Atr)] == doctest::Approx(2));
    CHECK(f.back()[ix(Feature::Natr)] == doctest::Approx(200.0 / 179));
    CHECK(f[19][ix(Feature::FastSma)] == doctest::Approx(117));
    CHECK(f[19][ix(Feature::SlowSma)] == doctest::Approx(109.5));
    CHECK(f[19][ix(Feature::Zscore)] == doctest::Approx(9.5 / std::sqrt(33.25)));
    CHECK(f[20][ix(Feature::Rsi)] == 100);
    auto altered = b;
    for (std::size_t i = 40; i < altered.size(); ++i) {
        altered[i].open *= 2;
        altered[i].high *= 2;
        altered[i].low *= 2;
        altered[i].close *= 2;
    }
    const auto changed = candle_features(altered, 5, 20, 20);
    for (int i = 0; i < 40; ++i)
        for (std::size_t k = 1; k < ix(Feature::Count); ++k)
            if (std::isfinite(f[i][k]))
                CHECK(f[i][k] == doctest::Approx(changed[i][k]));
}
TEST_CASE("Correlations align exact return intervals and do not forward fill") {
    auto a = bars({100, 101, 99, 104, 102, 105});
    auto b = a;
    for (auto &x : b) {
        x.open *= 2;
        x.high *= 2;
        x.low *= 2;
        x.close *= 2;
    }
    const auto same = aligned_correlation({a, b});
    CHECK(same.common_returns == 5);
    CHECK(same.matrix[0][1] == doctest::Approx(1));
    b.erase(b.begin() + 2);
    const auto missing = aligned_correlation({a, b});
    CHECK(missing.common_bars == 5);
    CHECK(missing.common_returns == 3);
    CHECK(missing.matrix[0][1] == doctest::Approx(1));
    b.erase(b.begin() + 1);
    const auto sparse = aligned_correlation({a, b});
    CHECK(sparse.common_returns == 2);
    CHECK(std::isnan(sparse.matrix[0][1]));
}
TEST_CASE("Rules support AND OR thresholds crossing and unavailable indicators") {
    Features before, now;
    before.fill(unavailable());
    now = before;
    const auto fast = static_cast<int>(Feature::FastSma), slow = static_cast<int>(Feature::SlowSma);
    before[fast] = 9;
    before[slow] = 10;
    now[fast] = 11;
    now[slow] = 10;
    RuleGroup group{{Rule{}}, true};
    CHECK(evaluate(group, before, now));
    CHECK_FALSE(evaluate(group, now, now));
    group.rules.push_back({Feature::Rsi, Feature::Constant, Compare::Below, 70, true});
    CHECK_FALSE(evaluate(group, before, now));
    group.all = false;
    CHECK(evaluate(group, before, now));
    CHECK_FALSE(evaluate({}, before, now));
}
TEST_CASE("Cash funded fills include both sides fees spread slippage and fixed cost") {
    auto c = base();
    const auto b = bars({100, 110});
    const auto free = backtest(b, c);
    CHECK(free.equity.back() == 1100);
    CHECK(free.units == 0);
    CHECK(free.metrics.closed_trades == 1);
    CHECK(free.metrics.fills == 2);
    c.commission_rate = .01;
    c.fixed_fee = 2;
    c.spread_bps = 20;
    c.slippage_bps = 10;
    const auto result = backtest(b, c);
    const double buy = 100 * 1.002, sell = 110 * .998;
    const int quantity = static_cast<int>(std::floor((1000 - 2) / (buy * 1.01)));
    const double expected = 1000 - quantity * buy * 1.01 - 2 + quantity * sell * .99 - 2;
    CHECK(result.equity.back() == doctest::Approx(expected));
    CHECK(result.journal.front().quantity == quantity);
    CHECK(result.metrics.turnover == doctest::Approx((quantity * buy + quantity * sell) / 1000));
    CHECK(result.equity.back() < free.equity.back());
}
TEST_CASE("Close signal cannot execute on same opening or see future bars") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.liquidate_end = false;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 0, true}};
    c.strategy.exit.rules.clear();
    auto b = bars({100, 110, 120});
    const auto r = backtest(b, c);
    REQUIRE(r.journal.size() >= 2);
    CHECK(r.journal[0].quantity == 0);
    CHECK(r.journal[1].time == b[1].time);
    CHECK(r.journal[1].price == 110);
    auto prefix = std::vector<Bar>(b.begin(), b.begin() + 2);
    const auto short_run = backtest(prefix, c);
    b[2].open = 800;
    b[2].close = 850;
    b[2].high = 900;
    b[2].low = 700;
    const auto changed = backtest(b, c);
    REQUIRE(short_run.equity.size() == 3);
    for (std::size_t i = 0; i < short_run.equity.size(); ++i)
        CHECK(short_run.equity[i] == changed.equity[i]);
}
TEST_CASE("Both stop target and opening gaps use documented assumptions") {
    auto c = base();
    c.stop_fraction = .02;
    c.target_fraction = .02;
    auto b = bars({100, 100});
    b[1].high = 104;
    b[1].low = 96;
    const auto worst = backtest(b, c);
    CHECK(worst.equity.back() == 980);
    c.pessimistic = false;
    const auto best = backtest(b, c);
    CHECK(best.equity.back() == 1020);
    b[1] = {b[1].time, b[1].end, 90, 95, 85, 93, 10000};
    const auto gap = backtest(b, c);
    CHECK(gap.equity.back() == 900);
}
TEST_CASE("Risk halt is causal and does not promise an unattainable stop price") {
    auto c = base();
    c.max_drawdown = .05;
    c.liquidate_end = false;
    auto b = bars({100, 90, 80});
    const auto r = backtest(b, c);
    CHECK(r.halted);
    CHECK(r.units == 0);
    CHECK(r.equity.back() == 800);
    CHECK(r.metrics.max_drawdown == doctest::Approx(.2));
    PaperEngine engine(base());
    engine.step(b[0]);
    engine.request_stop();
    engine.step(b[1]);
    CHECK(engine.result().units == 0);
    CHECK(engine.result().halted);
}
TEST_CASE("Paper replay and batch backtest have identical execution") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.strategy.fast = 3;
    c.strategy.slow = 7;
    std::vector<double> p;
    for (int i = 0; i < 200; ++i)
        p.push_back(100 + 8 * std::sin(i * .17));
    auto b = bars(p);
    const auto batch = backtest(b, c);
    PaperEngine paper(c);
    for (const auto &x : b)
        paper.step(x);
    paper.finish();
    const auto replay = paper.result();
    REQUIRE(batch.equity.size() == replay.equity.size());
    CHECK(batch.equity == replay.equity);
    CHECK(batch.journal.size() == replay.journal.size());
    CHECK(batch.metrics.total_return == replay.metrics.total_return);
}
TEST_CASE("Liquidity uses previous volume, no leverage, daily entry cap and bounded history") {
    auto c = base();
    c.initial_cash = 10000;
    c.strategy.buy_hold = false;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 0, true}};
    c.strategy.exit.rules.clear();
    c.participation = .2;
    c.lot = 10;
    c.liquidate_end = false;
    auto b = bars({100, 100, 100});
    b[0].volume = 100;
    b[1].volume = 1e9;
    const auto r = backtest(b, c);
    CHECK(r.units == 20);
    CHECK(r.cash >= 0);
    auto invalid = b;
    invalid[1].time = invalid[0].time;
    CHECK_THROWS(backtest(invalid, c));
    c.lot = 0;
    CHECK_THROWS(backtest(b, c));
    CHECK_THROWS(backtest({}, base()));
}
TEST_CASE("Metric definitions have undefined ratios for zero variance and calendar-aware CAGR") {
    auto c = base();
    const auto flat = backtest(bars(std::vector<double>(80, 100)), c);
    CHECK(std::isnan(flat.metrics.sharpe));
    CHECK(std::isnan(flat.metrics.sortino));
    CHECK(std::isnan(flat.metrics.calmar));
    CHECK(flat.metrics.cagr == doctest::Approx(0));
    const auto short_run = backtest(bars({100, 110}), c);
    CHECK(std::isnan(short_run.metrics.cagr));
}
TEST_CASE("Walk forward parameter selection cannot inspect its next test window") {
    auto c = base();
    c.strategy.buy_hold = false;
    std::vector<double> p;
    for (int i = 0; i < 220; ++i)
        p.push_back(100 + 10 * std::sin(i * .12) + i * .01);
    auto b = bars(p);
    const auto first = walk_forward(b, c, 120, 40);
    REQUIRE(first.folds.size() == 2);
    auto changed = b;
    for (std::size_t i = 120; i < changed.size(); ++i) {
        changed[i].open *= 1.3;
        changed[i].high *= 1.3;
        changed[i].low *= 1.3;
        changed[i].close *= 1.3;
    }
    const auto second = walk_forward(changed, c, 120, 40);
    CHECK(first.folds[0].fast == second.folds[0].fast);
    CHECK(first.folds[0].slow == second.folds[0].slow);
    CHECK(first.folds[0].train_return == second.folds[0].train_return);
    CHECK(first.folds[0].test_begin == 120);
    CHECK(first.folds[0].test_end == 160);
}

TEST_CASE("Session entry cap resets only on a new Moscow date") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.max_entries_per_day = 1;
    c.liquidate_end = false;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 0, true}};
    c.strategy.exit.rules = c.strategy.entry.rules;
    auto b = bars(std::vector<double>(10, 100));
    const std::int64_t start = 1700000000000LL;
    for (int i = 0; i < 10; ++i) {
        b[i].time = start + i * 3600000LL;
        b[i].end = b[i].time + 3600000;
    }
    const auto r = backtest(b, c);
    int buys = 0;
    for (const auto &row : r.journal)
        if (row.quantity > 0)
            ++buys;
    CHECK(buys == 1);
}
TEST_CASE("Long virtual session has bounded equity and journal; invalid windows fail before allocation") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 0, true}};
    c.strategy.exit.rules = c.strategy.entry.rules;
    auto b = bars(std::vector<double>(5000, 100));
    const auto r = backtest(b, c);
    CHECK(r.equity.size() == 5001);
    CHECK(r.journal.size() <= 10000);
    CHECK_THROWS(CandleFeatures(-1, 20));
    CHECK_THROWS(CandleFeatures(10, 1000000000));
}

TEST_CASE("Strategy EMA window is the same configurable window as candle analytics") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.liquidate_end = false;
    c.strategy.indicator_window = 3;
    c.strategy.entry.rules = {{Feature::Close, Feature::Ema, Compare::Above, 0, true}};
    c.strategy.exit.rules.clear();
    auto b = bars({10, 11, 12, 13, 14});
    const auto short_window = backtest(b, c);
    REQUIRE(short_window.metrics.fills == 1);
    bool correct = false;
    for (const auto &event : short_window.journal)
        if (event.quantity > 0)
            correct = event.time == b[3].time;
    CHECK(correct);
    c.strategy.indicator_window = 20;
    CHECK(backtest(b, c).metrics.fills == 0);
}

TEST_CASE("Risk size budgets both sides of fees slippage and adverse tick rounding") {
    auto c = base();
    c.initial_cash = 100000;
    c.risk_per_trade = .01;
    c.stop_fraction = .02;
    c.commission_rate = .001;
    c.fixed_fee = 7;
    c.spread_bps = 4;
    c.slippage_bps = 3;
    c.tick_size = .05;
    auto data = bars({100., 100.});
    data[0].low = 100.;
    data[0].high = 100.;
    PaperEngine engine(c);
    engine.step(data[0]);
    const auto r = engine.result();
    REQUIRE(r.journal.front().quantity > 0);
    const double buy = 100.05;
    const double stop_fill = std::floor(buy * .98 * (1 - .0005) / .05 + 1e-10) * .05;
    const double unit = buy - stop_fill + .001 * (buy + stop_fill);
    const auto expected = static_cast<std::int64_t>(std::floor((1000. - 14) / unit));
    CHECK(r.journal.front().price == doctest::Approx(buy));
    CHECK(r.units == expected);
    CHECK(r.units * unit + 14 <= 1000. + 1e-8);
    CHECK((r.units + 1) * unit + 14 > 1000.);
    c.fixed_fee = 600;
    PaperEngine too_costly(c);
    too_costly.step(data[0]);
    CHECK(too_costly.result().units == 0);
    c.tick_size = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS(PaperEngine{c});
}
TEST_CASE("Signal delay uses only completed observations and never delays emergency exits") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.signal_delay = 1;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 1, true}};
    auto data = bars({100., 101., 102., 103.});
    PaperEngine e(c);
    e.step(data[0]);
    CHECK(e.result().units == 0);
    e.step(data[1]);
    CHECK(e.result().units == 0);
    e.step(data[2]);
    CHECK(e.result().units > 0);
    CHECK(e.result().journal.back().time == data[2].time);
    e.request_stop();
    e.step(data[3]);
    CHECK(e.result().units == 0);
}
TEST_CASE("Final holdout parameter choice is unaffected by future prices and temporal embargo") {
    std::vector<double> prices;
    for (int i = 0; i < 300; ++i)
        prices.push_back(100 + 10 * std::sin(i * .12) + i * .05);
    const auto a = bars(prices);
    auto modified = a;
    for (std::size_t i = 240; i < modified.size(); ++i)
        for (double *p : {&modified[i].open, &modified[i].close, &modified[i].high, &modified[i].low})
            *p *= 2.;
    auto c = base();
    c.strategy.buy_hold = false;
    const auto x = final_holdout(a, c, 240, 3), y = final_holdout(modified, c, 240, 3);
    CHECK(x.frozen.strategy.fast == y.frozen.strategy.fast);
    CHECK(x.frozen.strategy.slow == y.frozen.strategy.slow);
    CHECK(x.test.times.size() == 57);
    CHECK(x.test.start_time == a[243].time);
    CHECK_THROWS(final_holdout(a, c, 20));
    CHECK_THROWS(final_holdout(a, c, 299));
}
TEST_CASE("Robustness grid and deterministic stress remain bounded and cancellable") {
    std::vector<double> prices;
    for (int i = 0; i < 5000; ++i)
        prices.push_back(100 + std::sin(i * .3));
    auto data = bars(prices);
    auto c = base();
    const auto original = data;
    const auto r = robustness(data, c);
    CHECK(r.stability.size() == 9);
    CHECK(r.stress.size() == 6);
    CHECK(data.size() == original.size());
    CHECK(data.back().close == original.back().close);
    for (const auto &trial : r.stress)
        CHECK(std::isfinite(trial.metrics.total_return));
    std::atomic<bool> cancel{true};
    CHECK(robustness(data, c, &cancel).stability.empty());
}

TEST_CASE("Session limits use supplied historical timezone offset rather than fixed current Moscow offset") {
    auto c = base();
    c.strategy.buy_hold = false;
    c.max_entries_per_day = 1;
    c.strategy.entry.rules = {{Feature::Close, Feature::Constant, Compare::Above, 1, true}};
    c.strategy.exit.rules = {{Feature::Close, Feature::Constant, Compare::Above, 1, true}};
    std::vector<Bar> data;
    // 2013 Moscow was UTC+4. 19:00 UTC is the prior date, 20:00 UTC starts the next session date.
    const std::int64_t start = 1383159600000LL;
    for (int i = 0; i < 5; ++i)
        data.push_back(
            {start + i * 1800000LL, start + (i + 1) * 1800000LL, 100, 100, 100, 100, 10000, 14400});
    PaperEngine historical(c);
    for (const auto &bar : data)
        historical.step(bar);
    auto current = data;
    for (auto &bar : current)
        bar.utc_offset_seconds = 10800;
    PaperEngine fixed(c);
    for (const auto &bar : current)
        fixed.step(bar);
    int a = 0, b = 0;
    for (const auto &event : historical.result().journal)
        if (event.quantity > 0)
            ++a;
    for (const auto &event : fixed.result().journal)
        if (event.quantity > 0)
            ++b;
    CHECK(a == 2);
    CHECK(b == 1);
    data[0].utc_offset_seconds = 60000;
    CHECK_THROWS(validate_bars(data));
}
