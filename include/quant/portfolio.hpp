#pragma once
#include "research.hpp"
#include <atomic>
#include <memory>
namespace quant {
/** One sleeve sharing portfolio cash. Group is a user-defined risk bucket, not verified sector metadata. */
struct PortfolioAsset {
    std::string symbol, group;
    std::vector<Bar> bars;
    double weight = 0, tick_size = 0;
    std::int64_t lot = 1;
    bool use_rules = false;
    Strategy strategy;
};
struct PortfolioConfig {
    double initial_cash = 100000, commission_rate = .0005, fixed_fee = 0;
    double spread_bps = 2, slippage_bps = 2;
    double max_weight = .4, max_invested = .8, group_limit = 0;
    double max_drawdown = .2, daily_loss = .05;
    int rebalance_every = 20;
    int manual_stop_after = 0; // one-based closed-bar count; zero disables
};
struct PortfolioEvent {
    std::int64_t time = 0, quantity = 0;
    std::string symbol, reason;
    double price = 0, fee = 0, cash = 0;
};
struct PortfolioPosition {
    std::string symbol;
    std::int64_t units = 0;
    double value = 0, pnl = 0, fees = 0;
    bool rules_ready = true;
};
struct PortfolioResult {
    std::vector<double> equity, drawdowns;
    std::vector<std::int64_t> times;
    std::vector<PortfolioPosition> positions;
    std::vector<PortfolioEvent> journal;
    std::vector<std::size_t> excluded;
    double cash = 0, total_return = 0, max_drawdown = 0, fees = 0, execution_cost = 0, turnover = 0;
    std::size_t common_bars = 0, dropped_events = 0;
    std::int64_t start_time = 0;
    int fills = 0;
    bool halted = false;
    std::string halt_reason;
};
/** Exact common intervals, ordered by ascending symbol. No forward-fill or interpolation. */
struct PortfolioSchedule {
    std::vector<std::vector<Bar>> frames;
    std::vector<std::size_t> excluded;
};
PortfolioSchedule portfolio_schedule(std::vector<PortfolioAsset> assets);
/** Incremental educational shared-cash account. Owns no source history; each closed common
 * frame is accepted once, in ascending symbol order. Invalid/duplicate frames are rejected before
 * mutation. At most 5000 trading frames, 5000 warmup frames and 10000 journal events.
 * No terminal liquidation; request_stop queues exits at the NEXT accepted opening.
 * result() returns a bounded value snapshot, without changing internal cash-flow attribution. */
class PortfolioAccount {
  public:
    PortfolioAccount(std::vector<PortfolioAsset> assets, const PortfolioConfig &config);
    ~PortfolioAccount();
    PortfolioAccount(PortfolioAccount &&) noexcept;
    PortfolioAccount &operator=(PortfolioAccount &&) noexcept;
    void step(const std::vector<Bar> &frame);
    /** Warmup features only: no trades, equity curve, historical profits or rule activations. */
    void warmup(const std::vector<Bar> &frame);
    /** Freeze targets from the final warmup close; call once before the first trading frame. */
    void arm_from_warmup();
    void request_stop(const std::string &reason = "MANUAL_STOP");
    bool halted() const;
    PortfolioResult result() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
/** Independent equal-weight buy/hold account, with the same cash/weight/cost constraints. */
PortfolioAccount portfolio_baseline_account(std::vector<PortfolioAsset> assets, PortfolioConfig config);
/** 1..6 assets, <=5000 bars each. Exact (start,end) intersection, no fill/interpolation.
 * Close signals freeze lot-rounded target quantities; orders execute on NEXT common open.
 * Sell first, then buy in sorted ticker order; shared cash never borrowed. Weight caps for new
 * increases reserve estimated execution costs. Price moves can change existing weights.
 * Risk stop is observed at close and exits at next common open; end positions remain marked to close.
 * rebalance_every=0 permits only the first allocation (signals/risk still evaluated).
 * Journal retains first 10000 events, with explicit dropped count. Throws on cancellation. */
PortfolioResult portfolio_backtest(std::vector<PortfolioAsset> assets, const PortfolioConfig &config,
                                   const std::atomic<bool> *cancel = nullptr);
/** Separate equally weighted buy/hold account with same costs and weight/cash caps, no drawdown/daily stop.
 */
PortfolioResult portfolio_baseline(std::vector<PortfolioAsset> assets, PortfolioConfig config,
                                   const std::atomic<bool> *cancel = nullptr);
} // namespace quant
