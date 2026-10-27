#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "quant/indicators.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
TEST_CASE("Ring preserves chronological order at a fixed allocation") {
    quant::RingBuffer<int> ring(3);const auto bytes=ring.allocated_bytes();
    for(int i=0;i<100000;++i)ring.push(i);
    CHECK(ring.size()==3);CHECK(ring[0]==99997);CHECK(ring.back()==99999);CHECK(ring.allocated_bytes()==bytes);
    CHECK_THROWS_AS(quant::RingBuffer<int>(0),std::invalid_argument);
}
TEST_CASE("Rolling price moments match a two-pass window reference") {
    quant::PriceMoments moments(20);std::vector<double> p;
    for(int i=0;i<2000;++i){const double x=100+4*std::sin(i*0.11);p.push_back(x);moments.push(x);const auto start=p.size()>20?p.size()-20:0;
        long double sum=0,squared=0;for(std::size_t j=start;j<p.size();++j)sum+=p[j];const long double mean=sum/(p.size()-start);
        for(std::size_t j=start;j<p.size();++j)squared+=(p[j]-mean)*(p[j]-mean);
        CHECK(moments.mean()==doctest::Approx(static_cast<double>(mean)).epsilon(1e-9));
        CHECK(moments.stddev()==doctest::Approx(std::sqrt(static_cast<double>(squared/(p.size()-start)))).epsilon(1e-7));
    }
}
TEST_CASE("EMA uses SMA seed and correct recursion") {
    quant::Ema ema(3);CHECK(std::isnan(ema.push(1)));CHECK(std::isnan(ema.push(2)));
    CHECK(ema.push(3)==doctest::Approx(2));CHECK(ema.push(4)==doctest::Approx(3));
    CHECK_THROWS_AS(quant::Ema(0),std::invalid_argument);
}
TEST_CASE("Wilder RSI warmup, boundaries and smoothing") {
    quant::StreamingIndicators up,down,flat,alternating;
    quant::IndicatorValues u,d,f,a;
    for(int i=0;i<15;++i){u=up.push(100+i);d=down.push(100-i);f=flat.push(100);a=alternating.push(100+i%2);if(i<14)CHECK(std::isnan(u.rsi));}
    CHECK(u.rsi==doctest::Approx(100));CHECK(d.rsi==doctest::Approx(0));CHECK(f.rsi==doctest::Approx(50));CHECK(a.rsi==doctest::Approx(50));
    CHECK(alternating.push(101).rsi==doctest::Approx(53.5714285714));
}
TEST_CASE("Bollinger population deviation and MACD warmup") {
    quant::StreamingIndicators engine(20);quant::IndicatorValues v;
    for(int i=1;i<=40;++i){v=engine.push(i);if(i<20)CHECK(std::isnan(v.upper));if(i<26)CHECK(std::isnan(v.macd));if(i<34)CHECK(std::isnan(v.signal));}
    CHECK(v.sma==doctest::Approx(30.5));CHECK(v.upper==doctest::Approx(30.5+2*std::sqrt(33.25)));CHECK(v.lower==doctest::Approx(30.5-2*std::sqrt(33.25)));
    CHECK(v.macd==doctest::Approx(7));CHECK(v.signal==doctest::Approx(7));
    CHECK_THROWS_AS(engine.push(-1),std::invalid_argument);CHECK_THROWS_AS(engine.push(std::numeric_limits<double>::infinity()),std::invalid_argument);
}
TEST_CASE("Forecast rolling origin predicts a deterministic linear series") {
    std::vector<double> p;for(int i=0;i<120;++i)p.push_back(100+2*i);
    const auto f=quant::experimental_forecast(p,20,5);CHECK(f.evaluations==96);CHECK(f.trend==doctest::Approx(348));CHECK(f.naive==338);
    CHECK(f.trend_mae==doctest::Approx(0).epsilon(1e-9));CHECK(f.trend_rmse==doctest::Approx(0).epsilon(1e-9));CHECK(f.naive_mae==doctest::Approx(10));
}
TEST_CASE("Backtest does not train on intermediate future observations") {
    std::vector<double> p;for(int i=0;i<25;++i)p.push_back(100+i);
    const auto a=quant::experimental_forecast(p,20,5);
    for(int i=20;i<24;++i)p[i]=1000+i;
    const auto b=quant::experimental_forecast(p,20,5);
    CHECK(a.evaluations==1);CHECK(b.evaluations==1);CHECK(a.trend_mae==doctest::Approx(b.trend_mae));CHECK(a.naive_mae==doctest::Approx(b.naive_mae));
}
TEST_CASE("Forecast flat prices, short history and invalid parameters") {
    const auto flat=quant::experimental_forecast(std::vector<double>(100,100),20,3);CHECK(flat.trend==doctest::Approx(100));CHECK(flat.trend_mae==doctest::Approx(0));
    const auto short_history=quant::experimental_forecast({100,101},20,1);CHECK(short_history.evaluations==0);CHECK(std::isnan(short_history.trend));
    CHECK_THROWS_AS(quant::experimental_forecast({100,101,102},2,1),std::invalid_argument);CHECK_THROWS_AS(quant::experimental_forecast({100,101,102},3,0),std::invalid_argument);CHECK_THROWS_AS(quant::experimental_forecast({100,-1,102},3,1),std::invalid_argument);
}
TEST_CASE("Position sizing respects both cash and risk, with whole lots") {
    auto r=quant::size_long_position(100000,1,100,95,110,10);CHECK(r.lots==20);CHECK(r.units==200);CHECK(r.cost==20000);CHECK(r.risk==1000);CHECK(r.reward==2000);CHECK(r.reward_risk==2);
    r=quant::size_long_position(1000,100,100,99,110,10);CHECK(r.lots==1);CHECK(r.cost<=1000);
    r=quant::size_long_position(100,1,100,95,110,10);CHECK(r.lots==0);
    CHECK_THROWS_AS(quant::size_long_position(1000,1,100,110,90,10),std::invalid_argument);CHECK_THROWS_AS(quant::size_long_position(1000,1,100,95,110,0),std::invalid_argument);
}
