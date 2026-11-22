#include "quant/analytics.hpp"
#include "quant/indicators.hpp"
#include "quant/research.hpp"
#include "quant/rolling.hpp"
#include "quant/portfolio.hpp"
#include "quant/robustness.hpp"
#include "quant/bootstrap.hpp"

#include <benchmark/benchmark.h>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
std::vector<double> make_prices(std::int64_t n) {
    std::vector<double> p(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < p.size(); ++i) {
        p[i] = 100 + 5 * std::sin(static_cast<double>(i) * 0.01);
    }
    return p;
}

void BM_Returns(benchmark::State& state) {
    const auto p = make_prices(state.range(0));
    for (auto _ : state) {
        auto r = quant::simple_returns(p);
        benchmark::DoNotOptimize(r.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_SMA(benchmark::State& state) {
    const auto p = make_prices(state.range(0));
    for (auto _ : state) {
        auto r = quant::moving_average(p, 20);
        benchmark::DoNotOptimize(r.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_Volatility(benchmark::State& state) {
    const auto r = quant::simple_returns(make_prices(state.range(0)));
    for (auto _ : state) {
        auto v = quant::annualized_volatility(r);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations() * (state.range(0) - 1));
}
} // namespace

BENCHMARK(BM_Returns)->Arg(1000)->Arg(100000);
BENCHMARK(BM_SMA)->Arg(1000)->Arg(100000);
BENCHMARK(BM_Volatility)->Arg(1000)->Arg(100000);

static void BM_RollingUpdate(benchmark::State& state) {
    quant::RollingSeries series(static_cast<std::size_t>(state.range(0)));
    int i = 0;
    for (auto _ : state) {
        i = (i + 1) % 200;
        series.push(100.0 + i * 0.001);
        auto v = series.mean_price();
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingUpdate)->Arg(20)->Arg(2000);

static void BM_IndicatorsUpdate(benchmark::State& state) {
    quant::StreamingIndicators indicators(static_cast<std::size_t>(state.range(0)));
    int i = 0;
    for (auto _ : state) {
        i = (i + 1) % 1000;
        auto v = indicators.push(100 + i * 0.001);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IndicatorsUpdate)->Arg(20)->Arg(500);

static void BM_ExperimentalForecast(benchmark::State& state) {
    std::vector<double> p(500);
    for (int i = 0; i < 500; ++i) {
        p[i] = 100 + std::sin(i * 0.1);
    }
    for (auto _ : state) {
        auto v = quant::experimental_forecast(p, 30, 5);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_ExperimentalForecast);

static void BM_CandleResearch(benchmark::State& state) {
    auto p = make_prices(500);
    for (auto _ : state) {
        auto r = quant::stationarity(p, 2, 4);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_CandleResearch);

static void BM_Backtest500(benchmark::State& state) {
    std::vector<quant::Bar> bars;
    for (int i = 0; i < 500; ++i) {
        const auto t = 1700000000000LL + i * 86400000LL;
        const double p = 100 + 5 * std::sin(i * .1);
        bars.push_back({t, t + 86400000, p, p + 1, p - 1, p, 10000});
    }
    quant::SimConfig config;
    for (auto _ : state) {
        auto r = quant::backtest(bars, config);
        benchmark::DoNotOptimize(r.metrics.total_return);
    }
}
BENCHMARK(BM_Backtest500);

namespace {
std::vector<quant::Bar> research_bars() {
    std::vector<quant::Bar> bars;
    for (int i = 0; i < 5000; ++i) {
        const auto t = 1700000000000LL + i * 3600000LL;
        const double p = 100 + 5 * std::sin(i * .1);
        bars.push_back({t, t + 3600000, p, p + 1, p - 1, p, 10000});
    }
    return bars;
}
void BM_Research5000(benchmark::State &state) {
    const auto bars = research_bars();
    const quant::SimConfig config;
    for (auto _ : state) {
        if (state.range(0) == 0) {
            auto r = quant::backtest(bars, config, 0, true);
            benchmark::DoNotOptimize(r.metrics.total_return);
        } else if (state.range(0) == 1) {
            auto r = quant::walk_forward(bars, config);
            benchmark::DoNotOptimize(r.compounded_return);
        } else {
            auto r = quant::robustness(bars, config);
            benchmark::DoNotOptimize(r.stress.data());
        }
    }
}
void BM_Portfolio6x5000(benchmark::State &state) {
    std::vector<quant::PortfolioAsset> assets;
    for (int i = 0; i < 6; ++i) {
        quant::PortfolioAsset a;
        a.symbol = "ASSET" + std::to_string(i);
        a.bars = research_bars(); a.weight = .1;
        assets.push_back(std::move(a));
    }
    for (auto _ : state) {
        auto r = quant::portfolio_backtest(assets, {});
        benchmark::DoNotOptimize(r.total_return);
    }
}
void BM_Bootstrap5000(benchmark::State &state) {
    const auto simulation = quant::backtest(research_bars(), {});
    for (auto _ : state) {
        auto r = quant::equity_bootstrap(simulation.equity, {400, 10, 42});
        benchmark::DoNotOptimize(r.loss_frequency);
    }
}
}
BENCHMARK(BM_Research5000)->ArgName("0=trace,1=walk,2=stress")->Arg(0)->Arg(1)->Arg(2);
BENCHMARK(BM_Portfolio6x5000);
BENCHMARK(BM_Bootstrap5000);

BENCHMARK_MAIN();
