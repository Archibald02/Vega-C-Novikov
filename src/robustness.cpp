#include "quant/robustness.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace quant {
Robustness robustness(const std::vector<Bar> &bars, const SimConfig &config,
                      const std::atomic<bool> *cancel) {
    validate_bars(bars);
    Robustness out;
    auto cancelled = [&] { return cancel && cancel->load(); };
    for (double f : {.8, 1., 1.2})
        for (double s : {.8, 1., 1.2}) {
            if (cancelled())
                return out;
            auto c = config;
            c.strategy.fast = std::clamp(static_cast<int>(config.strategy.fast * f), 2, 499);
            c.strategy.slow = std::clamp(static_cast<int>(config.strategy.slow * s), 3, 500);
            Trial trial{"SMA", c.strategy.fast, c.strategy.slow, {}};
            if (c.strategy.fast < c.strategy.slow)
                trial.metrics = backtest(bars, c).metrics;
            else
                trial.metrics.total_return = unavailable();
            out.stability.push_back(trial);
        }
    for (int scenario = 0; scenario < 6; ++scenario) {
        if (cancelled())
            return out;
        auto c = config;
        auto sample = bars;
        const char *names[] = {"Комиссии ×2",
                               "Спред и проскальзывание ×3",
                               "Сигнал +1 свеча",
                               "Каждая 10-я свеча отсутствует",
                               "Остановка: 5 свечей без наблюдений",
                               "Объём 1% прошлой свечи"};
        if (scenario == 0) {
            c.commission_rate = std::min(.1, c.commission_rate * 2);
            c.fixed_fee = std::min(c.initial_cash, c.fixed_fee * 2);
        }
        if (scenario == 1) {
            c.spread_bps = std::min(600., c.spread_bps * 3 + 2);
            c.slippage_bps = std::min(600., c.slippage_bps * 3 + 2);
        }
        if (scenario == 2)
            c.signal_delay = std::min(10, c.signal_delay + 1);
        if (scenario == 3) {
            sample.clear();
            for (std::size_t i = 0; i < bars.size(); ++i)
                if (i % 10 != 9)
                    sample.push_back(bars[i]);
        }
        if (scenario == 4 && sample.size() > 20)
            sample.erase(sample.begin() + sample.size() / 2, sample.begin() + sample.size() / 2 + 5);
        if (scenario == 5)
            c.participation = c.participation > 0 ? std::min(c.participation, .01) : .01;
        out.stress.push_back(
            {names[scenario], c.strategy.fast, c.strategy.slow, backtest(sample, c).metrics});
    }
    return out;
}
HoldoutResult final_holdout(const std::vector<Bar> &bars, const SimConfig &config, std::size_t split,
                            std::size_t gap) {
    validate_bars(bars);
    if (split < 60 || split >= bars.size() || gap > 100 || split + gap >= bars.size() - 9)
        throw std::invalid_argument("Holdout needs train >=60 and test >=10 after gap");
    std::vector<Bar> train(bars.begin(), bars.begin() + split);
    auto frozen = config;
    double best = -std::numeric_limits<double>::infinity();
    for (int fast : {5, 10, 20})
        for (int slow : {20, 40, 60})
            if (fast < slow) {
                auto c = config;
                c.strategy.fast = fast;
                c.strategy.slow = slow;
                const auto score = backtest(train, c).metrics.total_return;
                if (score > best) {
                    best = score;
                    frozen = c;
                }
            }
    // Gap is excluded from fitting/scoring. Its observations may warm causal features before test.
    return {frozen, backtest(bars, frozen, split + gap), split, gap};
}
} // namespace quant
