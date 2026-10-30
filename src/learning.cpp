#include "quant/learning.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace quant {
namespace {
SimConfig base() {
    SimConfig c;
    c.initial_cash = 100000;
    c.strategy.fast = 2;
    c.strategy.slow = 4;
    c.commission_rate = c.spread_bps = c.slippage_bps = 0;
    c.allocation = c.risk_per_trade = c.max_drawdown = c.daily_loss = 1;
    c.stop_fraction = c.target_fraction = 0;
    return c;
}
Bar bar(int i, double open, double high, double low, double close) {
    const auto t = 1700000000000LL + i * 86400000LL;
    return {t, t + 86400000LL, open, high, low, close, 1000000, 10800};
}
} // namespace
LessonResult commission_lesson(double rate) {
    if (!std::isfinite(rate) || rate < .0001 || rate > .01)
        throw std::invalid_argument("Lesson commission");
    std::vector<Bar> bars;
    for (int i = 0; i < 41; ++i)
        bars.push_back(bar(i, 100, 100, 100, 100));
    auto c = base();
    c.strategy.entry = {{Rule{Feature::Close, Feature::Constant, Compare::Above, 0, true}}, true};
    c.strategy.exit = c.strategy.entry;
    LessonResult r;
    r.first = backtest(bars, c);
    c.commission_rate = rate;
    r.second = backtest(bars, c);
    return r;
}
LessonResult gap_lesson(double risk) {
    if (!std::isfinite(risk) || risk < .001 || risk > .05)
        throw std::invalid_argument("Lesson risk");
    auto c = base();
    c.strategy.buy_hold = true;
    c.stop_fraction = .02;
    c.risk_per_trade = risk;
    const auto first = bar(0, 100, 101, 99, 100);
    LessonResult r;
    r.first = backtest({first, bar(1, 100, 101, 97, 98)}, c);
    r.second = backtest({first, bar(1, 90, 91, 89, 90)}, c);
    return r;
}
std::vector<Bar> learning_bars() {
    std::vector<Bar> result;
    double previous = 100;
    const double anchor = 100 + .14 * 199 + 1.5 * std::sin(199 * .35);
    for (int i = 0; i < 400; ++i) {
        const double close = i < 200 ? 100 + .14 * i + 1.5 * std::sin(i * .35)
                                     : anchor + .02 * (i - 199) + 6 * std::sin((i - 199) * .5);
        result.push_back(
            bar(i, previous, std::max(previous, close) + .25, std::min(previous, close) - .25, close));
        previous = close;
    }
    return result;
}
LessonResult selection_lesson(const std::vector<Bar> &bars, int candidates) {
    if (candidates < 1 || candidates > 16 || bars.size() < 200 || bars.size() > 600)
        throw std::invalid_argument("Lesson grid");
    validate_bars(bars);
    LessonResult r;
    r.split = bars.size() / 2;
    const std::vector<Bar> train(bars.begin(), bars.begin() + r.split);
    const int fast[]{2, 4, 8, 16}, slow[]{20, 30, 40, 60};
    auto c = base();
    c.commission_rate = .0005;
    double best = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < candidates; ++i) {
        c.strategy.fast = fast[i / 4];
        c.strategy.slow = slow[i % 4];
        auto trial = backtest(train, c);
        const double score = trial.metrics.total_return;
        r.train_scores.push_back(score);
        if (score > best) {
            best = score;
            r.fast = c.strategy.fast;
            r.slow = c.strategy.slow;
            r.first = std::move(trial);
        }
    }
    c.strategy.fast = r.fast;
    c.strategy.slow = r.slow;
    r.second = backtest(bars, c, r.split);
    return r;
}
} // namespace quant
