#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "quant/analytics.hpp"
#include <cmath>
#include "quant/rolling.hpp"
#include <fstream>
#include <limits>
#include <stdexcept>
TEST_CASE("Simple returns match hand calculation") {
    const auto r=quant::simple_returns({100,110,99});
    REQUIRE(r.size()==2);
    CHECK(r[0]==doctest::Approx(0.1));
    CHECK(r[1]==doctest::Approx(-0.1));
}
TEST_CASE("Rolling averages and boundary windows") {
    const std::vector<double> p{10,20,30,40};
    CHECK(quant::moving_average(p,1)==p);
    CHECK(quant::moving_average(p,2)==std::vector<double>{15,25,35});
    CHECK(quant::moving_average(p,4)==std::vector<double>{25});
    CHECK_THROWS_AS(quant::moving_average(p,0),std::invalid_argument);
    CHECK_THROWS_AS(quant::moving_average(p,5),std::invalid_argument);
}
TEST_CASE("Sample volatility and annualization") {
    CHECK(quant::annualized_volatility({0.1,-0.1},1)==doctest::Approx(std::sqrt(0.02)));
    CHECK(quant::annualized_volatility({0.1,-0.1})==doctest::Approx(std::sqrt(0.02*252)));
    CHECK(quant::annualized_volatility({0.05,0.05,0.05})==doctest::Approx(0));
}
TEST_CASE("Invalid numerical inputs are rejected") {
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(quant::simple_returns({}),std::invalid_argument);
    CHECK_THROWS_AS(quant::simple_returns({100}),std::invalid_argument);
    for (const auto bad : {0.0,-1.0,nan,inf}) {
        CHECK_THROWS_AS(quant::simple_returns({100,bad}),std::invalid_argument);
        CHECK_THROWS_AS(quant::moving_average({100,bad},1),std::invalid_argument);
    }
    CHECK_THROWS_AS(quant::annualized_volatility({0.1}),std::invalid_argument);
    CHECK_THROWS_AS(quant::annualized_volatility({0.1,nan}),std::invalid_argument);
    CHECK_THROWS_AS(quant::annualized_volatility({0.1,0.2},0),std::invalid_argument);
}

TEST_CASE("Rolling series matches batch statistics after eviction") {
    quant::RollingSeries stream(20);
    std::vector<double> all;
    for (int i=0;i<10000;++i) {
        const double x=100+std::sin(i*0.031)*8;
        stream.push(x); all.push_back(x);
        const auto n=std::min<std::size_t>(20,all.size());
        const std::vector<double> window(all.end()-static_cast<std::ptrdiff_t>(n),all.end());
        CHECK(stream.size()==n);
        CHECK(stream.mean_price()==doctest::Approx(quant::moving_average(window,n).front()).epsilon(1e-9));
        if (n>=3) CHECK(stream.return_stddev()==doctest::Approx(quant::annualized_volatility(quant::simple_returns(window),1)).epsilon(1e-8));
    }
}
TEST_CASE("Rolling series validates input and warmup") {
    CHECK_THROWS_AS(quant::RollingSeries(2),std::invalid_argument);
    quant::RollingSeries stream(3);
    CHECK_THROWS_AS(stream.mean_price(),std::logic_error);
    CHECK_THROWS_AS(stream.last_return(),std::logic_error);
    CHECK_THROWS_AS(stream.return_stddev(),std::logic_error);
    CHECK_THROWS_AS(stream.push(0),std::invalid_argument);
    CHECK_THROWS_AS(stream.push(std::numeric_limits<double>::infinity()),std::invalid_argument);
    stream.push(100); stream.push(110); stream.push(99);
    CHECK(stream.last_return()==doctest::Approx(-0.1));
    CHECK(stream.return_stddev()==doctest::Approx(std::sqrt(0.02)));
    stream.push(99); stream.push(99); stream.push(99);
    CHECK(stream.size()==3);
    CHECK(stream.return_stddev()==doctest::Approx(0).epsilon(1e-9));
}
