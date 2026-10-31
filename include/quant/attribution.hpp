#pragma once
#include "research.hpp"
#include <atomic>
namespace quant {
struct PerformancePeriod {
    int key = 0; // YYYYMM for local opening month, or 1..3 for consecutive equal-count sections
    std::size_t begin = 0, end = 0; // bar indices [begin, end)
    double starting_equity = 0, ending_equity = 0, pnl = 0;
    double return_fraction = unavailable(), max_drawdown = unavailable(), fees = unavailable();
};
struct PositionContribution {
    std::size_t entry = 0, exit = 0;
    std::int64_t quantity = 0;
    int fills = 0;
    double entry_cost = 0, fees = 0, pnl = 0, return_fraction = 0, initial_capital_fraction = 0;
};
enum class AttributionTrace { Missing, Invalid, Ready };
struct PerformanceAttribution {
    bool cancelled = false;
    AttributionTrace trace_status = AttributionTrace::Missing;
    std::vector<PerformancePeriod> months, sections;
    std::vector<PositionContribution> positions; // complete round trips, including all partial sales
    double initial_capital = 0, final_capital = 0, total_pnl = 0;
    double closed_pnl = unavailable(), open_position_pnl = unavailable(), fees = unavailable();
    double gross_profit = unavailable(), gross_loss = unavailable();
    double top_one_profit_share = unavailable(), top_three_profit_share = unavailable();
    double reconciliation_error = unavailable();
    std::int64_t open_units = 0;
};
/** Gregorian month of bar.time using the bar's UTC offset; no exchange-session calendar inference. */
int opening_month(const Bar &bar);
/** Describe one frozen full backtest, without rerunning signals or resetting positions at boundaries.
 * Equity includes initial cash. Period profit/loss telescopes to total profit/loss. Trade attribution requires a complete,
 * internally consistent execution trace; the bounded journal is never used to reconstruct missing fills.
 * At most 5000 bars/positions/months; cancellation publishes no partial report. */
PerformanceAttribution performance_attribution(const std::vector<Bar> &bars, const SimResult &simulation,
                                               const std::atomic<bool> *cancel = nullptr);
} // namespace quant
