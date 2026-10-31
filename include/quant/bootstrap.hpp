#pragma once
#include "research.hpp"
#include <atomic>
namespace quant {
/** Fixed-length overlapping blocks of observed close-to-close account returns. */
struct BootstrapConfig {
    int simulations = 400, block_length = 10;
    std::uint32_t seed = 42;
};
enum class BootstrapStatus { Disabled, Ready, TooShort, NonPositiveCapital, Cancelled, NumericalLimit };
struct BootstrapBand {
    int observation = 0;
    double lower = 0, median = 0, upper = 0, actual = 0; // capital index, first close = 100
};
struct BootstrapResult {
    BootstrapConfig config;
    BootstrapStatus status = BootstrapStatus::Disabled;
    int observations = 0;
    double actual_return = unavailable(), actual_drawdown = unavailable();
    double loss_frequency =
        unavailable(); // frequency of returns < -1e-12 (numeric zero tolerance), not a forecast probability
    std::array<double, 3> returns{}, drawdowns{}; // interpolated 5/50/95 percentiles
    std::vector<BootstrapBand> band;              // at most 101 pointwise percentile observations
    std::vector<double> terminal_returns;         // at most 1000, sorted, for explicit reporting/histogram
};
/** Excludes equity[0] (initial cash), starts at equity[1] (first candle close).
 * Resamples overlapping non-circular blocks, trims the last block, preserves within-block order.
 * No strategy rerun, forecast, annualization, or holdout access. Limits: 5001 equity points,
 * 100..1000 simulations, blocks 1..250; cancellation publishes no partial estimate. */
BootstrapResult equity_bootstrap(const std::vector<double> &equity, const BootstrapConfig &config = {},
                                 const std::atomic<bool> *cancel = nullptr);
} // namespace quant
