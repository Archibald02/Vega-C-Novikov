#pragma once
#include "indicators.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace quant {
/** Closed OHLCV bar. Time is UTC milliseconds, interval boundaries supplied by caller. */
struct Bar {
    std::int64_t time = 0, end = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
    int utc_offset_seconds = 10800; // Session zone at bar start; caller supplies historic offset.
};
void validate_bars(const std::vector<Bar> &bars);
struct Distribution {
    std::vector<double> simple, log_returns;
    std::vector<double> bin_centers, bin_counts;
    double mean = unavailable(), sd = unavailable(), q05 = unavailable(), median = unavailable(),
           q95 = unavailable();
    double skew = unavailable(), excess_kurtosis = unavailable();
};
Distribution distribution(const std::vector<double> &prices, int bins = 20);
std::vector<double> acf(const std::vector<double> &values, int lags);
std::vector<double> pacf(const std::vector<double> &values, int lags);
struct Stationarity {
    double adf = unavailable(), adf_critical5 = unavailable(), kpss = unavailable();
    int adf_lags = 0, kpss_lags = 0;
    std::size_t observations = 0;
};
/** Constant-only ADF (fixed lag) and level KPSS (Bartlett HAC). No fabricated exact p-values. */
Stationarity stationarity(const std::vector<double> &values, int adf_lags = 2, int kpss_lags = 4);
struct Correlation {
    std::vector<std::vector<double>> matrix;
    std::size_t common_bars = 0, common_returns = 0;
};
/** Exact bar keys, then intersect exact return intervals. No forward-fill or unequal horizon returns. */
Correlation aligned_correlation(const std::vector<std::vector<Bar>> &series);
enum class Feature {
    Constant,
    Close,
    FastSma,
    SlowSma,
    Ema,
    Rsi,
    Macd,
    MacdSignal,
    Upper,
    Lower,
    Atr,
    Natr,
    Zscore,
    Volume,
    Count
};
using Features = std::array<double, static_cast<std::size_t>(Feature::Count)>;
class CandleFeatures {
  public:
    CandleFeatures(int fast, int slow, int indicator_window = 20);
    Features push(const Bar &bar);

  private:
    StreamingIndicators indicators_;
    PriceMoments fast_, slow_, z_;
    int tr_count_ = 0;
    double tr_sum_ = 0, atr_ = unavailable(), previous_ = unavailable();
};
std::vector<Features> candle_features(const std::vector<Bar> &bars, int fast = 10, int slow = 30,
                                      int window = 20);
enum class Compare { Above, Below, CrossAbove, CrossBelow };
struct Rule {
    Feature lhs = Feature::FastSma, rhs = Feature::SlowSma;
    Compare op = Compare::CrossAbove;
    double value = 0;
    bool enabled = true;
};
struct RuleGroup {
    std::vector<Rule> rules;
    bool all = true;
};
struct Strategy {
    int fast = 10, slow = 30, indicator_window = 20;
    RuleGroup entry{{Rule{}}, true};
    RuleGroup exit{{Rule{Feature::FastSma, Feature::SlowSma, Compare::CrossBelow, 0, true}}, true};
    bool buy_hold = false;
};
bool evaluate(const RuleGroup &group, const Features &previous, const Features &current);
struct SimConfig {
    Strategy strategy;
    double initial_cash = 100000, commission_rate = 0.0005, fixed_fee = 0;
    double spread_bps = 2, slippage_bps = 2, allocation = 0.2;
    double stop_fraction = 0.02, target_fraction = 0.04;
    double risk_per_trade = 0.01, max_drawdown = 0.2, daily_loss = 0.05;
    double tick_size = 0;     // zero only for synthetic/explicit manual metadata
    int signal_delay = 0;     // additional closed bars before signal execution; stops remain active
    double participation = 0; // cap using PREVIOUS bar volume; zero disables this approximation
    std::int64_t lot = 1;
    int max_entries_per_day = 10, annual_periods = 0;
    bool pessimistic = true, liquidate_end = true;
};
struct JournalEntry {
    std::int64_t time = 0, quantity = 0;
    double price = 0, cash = 0, equity = 0;
    std::string reason;
};
struct Metrics {
    double total_return = 0, max_drawdown = 0, turnover = 0, sharpe = unavailable(), sortino = unavailable(),
           cagr = unavailable(), calmar = unavailable();
    double profit_factor = unavailable(), win_rate = unavailable();
    double fees = 0, execution_cost = 0; // cash commissions and adverse fill vs model trigger
    int fills = 0, closed_trades = 0;
};
struct SimResult {
    std::vector<double> equity, drawdowns;
    std::vector<std::int64_t> times;
    std::int64_t start_time = 0;
    std::vector<JournalEntry> journal;
    Metrics metrics;
    double cash = 0, unrealized = 0;
    std::int64_t units = 0;
    bool halted = false;
    std::string halt_reason;
};
/** One bounded causal execution engine shared by backtest and virtual account replay. Long-only, cash funded.
 */
class PaperEngine {
  public:
    explicit PaperEngine(const SimConfig &config);
    void warmup(const Bar &bar);
    void arm_from_warmup();
    void step(const Bar &bar);
    void request_stop(const std::string &reason = "manual stop");
    void finish();
    SimResult result() const;

  private:
    void entry(const Bar &bar);
    void exit(const Bar &bar, double price, const std::string &reason, bool unlimited = false);
    void log(const Bar &bar, std::int64_t quantity, double price, const std::string &reason);
    void mark(const Bar &bar);
    SimConfig config_;
    CandleFeatures features_;
    Features current_{}, previous_{};
    SimResult result_;
    Bar last_{};
    double cash_ = 0, entry_price_ = 0, cost_basis_ = 0, peak_ = 0, session_equity_ = 0;
    double traded_ = 0, gross_profit_ = 0, gross_loss_ = 0, trade_profit_ = 0;
    std::int64_t units_ = 0, session_ = -1;
    int entries_today_ = 0, wins_ = 0, entry_wait_ = 0, exit_wait_ = 0;
    bool pending_entry_ = false, pending_exit_ = false, halted_ = false, session_halted_ = false,
         bought_ = false, finished_ = false;
    std::int64_t liquidity_cap_ = 0;
};
SimResult backtest(const std::vector<Bar> &bars, const SimConfig &config, std::size_t start = 0);
SimResult buy_hold(const std::vector<Bar> &bars, const SimConfig &config);
struct Fold {
    std::size_t train_begin = 0, test_begin = 0, test_end = 0;
    int fast = 0, slow = 0;
    double train_return = 0, test_return = 0;
};
struct WalkForward {
    std::vector<Fold> folds;
    double compounded_return = 0;
};
/** Rolling train windows, bounded 3x3 parameter grid, frozen parameters in each next test window. */
WalkForward walk_forward(const std::vector<Bar> &bars, const SimConfig &config, int train = 120,
                         int test = 40);
} // namespace quant
