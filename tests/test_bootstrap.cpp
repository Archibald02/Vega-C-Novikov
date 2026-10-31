#include "quant/bootstrap.hpp"
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
namespace {
std::vector<double> flat_equity(int n = 24) { return std::vector<double>(n + 2, 100); }
std::vector<double> reference_equity() {
    std::vector<double> e{123, 100};
    for (int i = 0; i < 6; ++i)
        for (double x : {125., 100., 110., 100.})
            e.push_back(x);
    return e;
}
} // namespace
TEST_CASE("Bootstrap keeps constant capital and excludes the initial-cash transition") {
    auto e = flat_equity();
    e[0] = 100000;
    const auto b = quant::equity_bootstrap(e, {100, 10, 42});
    REQUIRE(b.status == quant::BootstrapStatus::Ready);
    CHECK(b.observations == 24);
    CHECK(b.actual_return == 0);
    CHECK(b.actual_drawdown == 0);
    CHECK(b.loss_frequency == 0);
    CHECK(b.returns == std::array<double, 3>{0, 0, 0});
    CHECK(b.drawdowns == std::array<double, 3>{0, 0, 0});
    REQUIRE(b.band.size() == 25);
    for (const auto &p : b.band) {
        CHECK(p.lower == 100);
        CHECK(p.median == 100);
        CHECK(p.upper == 100);
        CHECK(p.actual == 100);
    }
}
TEST_CASE("Bootstrap whole-series block reproduces ordered historical path") {
    auto e = flat_equity();
    for (std::size_t i = 1; i < e.size(); ++i)
        e[i] = i % 2 ? 100 : 120;
    const auto b = quant::equity_bootstrap(e, {100, 24, 1});
    REQUIRE(b.status == quant::BootstrapStatus::Ready);
    for (const auto &p : b.band) {
        CHECK(p.lower == doctest::Approx(p.actual));
        CHECK(p.median == doctest::Approx(p.actual));
        CHECK(p.upper == doctest::Approx(p.actual));
    }
    for (double v : b.returns)
        CHECK(v == doctest::Approx(b.actual_return));
    for (double v : b.drawdowns)
        CHECK(v == doctest::Approx(b.actual_drawdown));
}
TEST_CASE("Bootstrap agrees with independent rational-arithmetic Python reference") {
    const auto b = quant::equity_bootstrap(reference_equity(), {100, 3, 42});
    REQUIRE(b.status == quant::BootstrapStatus::Ready);
    // Fractions-based reference and explicit unsigned PRNG: tests/reference_bootstrap.py.
    const std::array<double, 3> returns{-0.5345454545454545, 0.0, 1.1484375};
    const std::array<double, 3> dd{0.2, 0.36, 0.6614876033057852};
    for (int i = 0; i < 3; ++i) {
        CHECK(b.returns[i] == doctest::Approx(returns[i]).epsilon(1e-11));
        CHECK(b.drawdowns[i] == doctest::Approx(dd[i]).epsilon(1e-11));
    }
    CHECK(b.loss_frequency == doctest::Approx(0.47));
    CHECK(b.band[7].median == doctest::Approx(106.65289256198348).epsilon(1e-11));
    CHECK(b.band[13].lower == doctest::Approx(51.2).epsilon(1e-11));
}
TEST_CASE("Bootstrap seed replay and single-observation blocks are deterministic") {
    const auto e = reference_equity();
    const auto a = quant::equity_bootstrap(e, {100, 1, 42});
    const auto b = quant::equity_bootstrap(e, {100, 1, 42});
    const auto changed = quant::equity_bootstrap(e, {100, 1, 43});
    REQUIRE(a.status == quant::BootstrapStatus::Ready);
    CHECK(a.terminal_returns == b.terminal_returns);
    CHECK(a.drawdowns == b.drawdowns);
    CHECK(a.terminal_returns != changed.terminal_returns);
}
TEST_CASE("Bootstrap does not discard insolvency or publish partial estimates") {
    auto e = flat_equity();
    e[10] = 0;
    CHECK(quant::equity_bootstrap(e).status == quant::BootstrapStatus::NonPositiveCapital);
    e[10] = -1;
    CHECK(quant::equity_bootstrap(e).status == quant::BootstrapStatus::NonPositiveCapital);
    e = flat_equity();
    std::atomic<bool> cancelled{true};
    const auto stopped = quant::equity_bootstrap(e, {}, &cancelled);
    CHECK(stopped.status == quant::BootstrapStatus::Cancelled);
    CHECK(stopped.band.empty());
    CHECK(stopped.terminal_returns.empty());
    CHECK(std::isnan(stopped.loss_frequency));
}
TEST_CASE("Bootstrap rejects invalid allocations and labels insufficient observations") {
    CHECK(quant::equity_bootstrap({}).status == quant::BootstrapStatus::TooShort);
    CHECK(quant::equity_bootstrap(flat_equity(19)).status == quant::BootstrapStatus::TooShort);
    CHECK(quant::equity_bootstrap(flat_equity(), {100, 25, 1}).status == quant::BootstrapStatus::TooShort);
    for (int simulations : {-1, 0, 99, 1001, 1000000000})
        CHECK_THROWS(quant::equity_bootstrap(flat_equity(), {simulations, 1, 1}));
    for (int block : {0, -1, 251, 1000000000})
        CHECK_THROWS(quant::equity_bootstrap(flat_equity(), {100, block, 1}));
    CHECK_THROWS(quant::equity_bootstrap(flat_equity(5000)));
    auto e = flat_equity();
    e[4] = std::numeric_limits<double>::infinity();
    CHECK_THROWS(quant::equity_bootstrap(e));
    e[4] = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS(quant::equity_bootstrap(e));
}
TEST_CASE("Bootstrap maximum history and scenarios retain at most 101 quantile points") {
    auto e = flat_equity(4999);
    for (std::size_t i = 1; i < e.size(); ++i)
        e[i] = 100 + (i % 7);
    const auto b = quant::equity_bootstrap(e, {1000, 250, 0});
    REQUIRE(b.status == quant::BootstrapStatus::Ready);
    CHECK(b.band.size() == 101);
    CHECK(b.terminal_returns.size() == 1000);
    CHECK(b.band.back().observation == 4999);
    for (const auto &p : b.band) {
        CHECK(p.lower <= p.median);
        CHECK(p.median <= p.upper);
        CHECK(std::isfinite(p.upper));
    }
    for (double d : b.drawdowns) {
        CHECK(d >= 0);
        CHECK(d <= 1);
    }
}
TEST_CASE("Bootstrap reports numerical overflow instead of clipping extreme paths") {
    auto e = flat_equity();
    for (std::size_t i = 2; i < e.size(); ++i)
        e[i] = e[i - 1] * 1e10;
    // Input is finite, but independent resampling of huge positive returns exceeds numerical limits.
    const auto b = quant::equity_bootstrap(e, {100, 1, 0});
    CHECK(b.status == quant::BootstrapStatus::Ready); // 24*log(1e10) is still representable
    e = flat_equity();
    for (std::size_t i = 1; i < e.size(); ++i)
        e[i] = i % 2 ? 1e-300 : 1e300;
    const auto extreme = quant::equity_bootstrap(e, {100, 1, 0});
    CHECK(extreme.status == quant::BootstrapStatus::NumericalLimit);
    CHECK(extreme.band.empty());
}

TEST_CASE("Bootstrap trims a nondivisible last block without wrapping history") {
    const auto b = quant::equity_bootstrap(reference_equity(), {100, 5, 42});
    REQUIRE(b.status == quant::BootstrapStatus::Ready);
    const std::array<double, 3> returns{-0.3619474079639369, 0.0, 0.5675625};
    const std::array<double, 3> dd{0.2, 0.296, 0.5199278737791134};
    for (int i = 0; i < 3; ++i) {
        CHECK(b.returns[i] == doctest::Approx(returns[i]).epsilon(1e-11));
        CHECK(b.drawdowns[i] == doctest::Approx(dd[i]).epsilon(1e-11));
    }
    CHECK(b.loss_frequency == doctest::Approx(0.44));
    CHECK(b.band.back().observation == 24);
}
