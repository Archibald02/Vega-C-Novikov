#include <cstdint>
#include "quant/rolling.hpp"
#include "quant/indicators.hpp"
#include "quant/research.hpp"
#include <benchmark/benchmark.h>
#include "quant/analytics.hpp"
#include <cmath>
namespace {
std::vector<double> make_prices(std::int64_t n) {
    std::vector<double> p(static_cast<std::size_t>(n));
    for (std::size_t i=0;i<p.size();++i) p[i]=100+5*std::sin(static_cast<double>(i)*0.01);
    return p;
}
void BM_Returns(benchmark::State& state) {
    const auto p=make_prices(state.range(0));
    for (auto _ : state) { auto r=quant::simple_returns(p); benchmark::DoNotOptimize(r.data()); benchmark::ClobberMemory(); }
    state.SetItemsProcessed(state.iterations()*state.range(0));
}
void BM_SMA(benchmark::State& state) {
    const auto p=make_prices(state.range(0));
    for (auto _ : state) { auto r=quant::moving_average(p,20); benchmark::DoNotOptimize(r.data()); benchmark::ClobberMemory(); }
    state.SetItemsProcessed(state.iterations()*state.range(0));
}
void BM_Volatility(benchmark::State& state) {
    const auto r=quant::simple_returns(make_prices(state.range(0)));
    for (auto _ : state) { auto v=quant::annualized_volatility(r); benchmark::DoNotOptimize(v); }
    state.SetItemsProcessed(state.iterations()*(state.range(0)-1));
}
}
BENCHMARK(BM_Returns)->Arg(1000)->Arg(100000);
BENCHMARK(BM_SMA)->Arg(1000)->Arg(100000);
BENCHMARK(BM_Volatility)->Arg(1000)->Arg(100000);

static void BM_RollingUpdate(benchmark::State& state) {
    quant::RollingSeries series(static_cast<std::size_t>(state.range(0)));
    int i=0;
    for (auto _ : state) {
        i=(i+1)%200;
        series.push(100.0+i*0.001);
        auto v=series.mean_price(); benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingUpdate)->Arg(20)->Arg(2000);

static void BM_IndicatorsUpdate(benchmark::State& state) {
    quant::StreamingIndicators indicators(static_cast<std::size_t>(state.range(0)));int i=0;
    for(auto _ : state){i=(i+1)%1000;auto v=indicators.push(100+i*0.001);benchmark::DoNotOptimize(v);}
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IndicatorsUpdate)->Arg(20)->Arg(500);
static void BM_ExperimentalForecast(benchmark::State& state) {
    std::vector<double> p(500);for(int i=0;i<500;++i)p[i]=100+std::sin(i*0.1);
    for(auto _ : state){auto v=quant::experimental_forecast(p,30,5);benchmark::DoNotOptimize(v);}
}
BENCHMARK(BM_ExperimentalForecast);
static void BM_CandleResearch(benchmark::State& state){auto p=make_prices(500);for(auto _:state){auto r=quant::stationarity(p,2,4);benchmark::DoNotOptimize(r);}}
BENCHMARK(BM_CandleResearch);
static void BM_Backtest500(benchmark::State& state){std::vector<quant::Bar> bars;for(int i=0;i<500;++i){const auto t=1700000000000LL+i*86400000LL;const double p=100+5*std::sin(i*.1);bars.push_back({t,t+86400000,p,p+1,p-1,p,10000});}quant::SimConfig config;for(auto _:state){auto r=quant::backtest(bars,config);benchmark::DoNotOptimize(r.metrics.total_return);}}
BENCHMARK(BM_Backtest500);
BENCHMARK_MAIN();
