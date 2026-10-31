#include "quant/research.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
namespace quant {
namespace {
constexpr std::int64_t day_ms = 86400000;
std::size_t checked_window(int n) {
    if (n < 2 || n > 500)
        throw std::invalid_argument("Feature window");
    return static_cast<std::size_t>(n);
}
void validate_values(const std::vector<double> &x, std::size_t minimum = 2) {
    if (x.size() < minimum || x.size() > 5000)
        throw std::invalid_argument("Need bounded finite observations");
    for (double v : x)
        if (!std::isfinite(v) || std::abs(v) > 1e12)
            throw std::invalid_argument("Invalid numeric observation");
}
double average(const std::vector<double> &x) {
    long double sum = 0;
    for (double v : x)
        sum += v;
    return static_cast<double>(sum / x.size());
}
double quantile(const std::vector<double> &sorted, double p) {
    const double position = (sorted.size() - 1) * p;
    const auto lo = static_cast<std::size_t>(position);
    return sorted[lo] + (sorted[std::min(lo + 1, sorted.size() - 1)] - sorted[lo]) * (position - lo);
}
std::size_t idx(Feature f) { return static_cast<std::size_t>(f); }
void validate_config(const SimConfig &c) {
    for (double v :
         {c.initial_cash, c.commission_rate, c.fixed_fee, c.spread_bps, c.slippage_bps, c.allocation,
          c.stop_fraction, c.target_fraction, c.risk_per_trade, c.max_drawdown, c.daily_loss, c.participation,
          c.tick_size, c.partial_target_fraction, c.partial_exit_fraction})
        if (!std::isfinite(v) || v < 0)
            throw std::invalid_argument("Invalid simulation setting");
    if (c.initial_cash < 1 || c.initial_cash > 1e12 || c.commission_rate > .1 ||
        c.fixed_fee > c.initial_cash || c.spread_bps + c.slippage_bps > 2000 || c.allocation <= 0 ||
        c.allocation > 1 || c.stop_fraction >= 1 || c.target_fraction > 10 || c.risk_per_trade <= 0 ||
        c.risk_per_trade > 1 || c.max_drawdown <= 0 || c.max_drawdown > 1 || c.daily_loss <= 0 ||
        c.daily_loss > 1 || c.participation > 1 || c.lot < 1 || c.lot > 1000000 ||
        c.max_entries_per_day < 1 || c.max_entries_per_day > 1000 || c.annual_periods < 0 ||
        c.annual_periods > 100000 || c.tick_size > 1e9 || c.signal_delay < 0 || c.signal_delay > 10 ||
        c.entry_confirmation < 1 || c.entry_confirmation > 50 || c.exit_confirmation < 1 ||
        c.exit_confirmation > 50 || c.cooldown_bars < 0 || c.cooldown_bars > 500 || c.max_holding_bars < 0 ||
        c.max_holding_bars > 5000 || c.partial_target_fraction > 10 || c.partial_exit_fraction <= 0 ||
        c.partial_exit_fraction >= 1)
        throw std::invalid_argument("Simulation setting out of range");
    for (const auto &group : {c.strategy.entry, c.strategy.exit}) {
        if (!valid_rule_expression(group))
            throw std::invalid_argument("Invalid bounded rule expression");
        if (group.rules.size() > 4)
            throw std::invalid_argument("Too many rules");
        for (const auto &r : group.rules)
            if (idx(r.lhs) >= idx(Feature::Count) || idx(r.rhs) >= idx(Feature::Count) ||
                static_cast<int>(r.op) < 0 || static_cast<int>(r.op) > 3 || !std::isfinite(r.value) ||
                std::abs(r.value) > 1e12)
                throw std::invalid_argument("Invalid rule");
    }
}
} // namespace
void validate_bars(const std::vector<Bar> &bars) {
    if (bars.empty() || bars.size() > 5000)
        throw std::invalid_argument("Need 1..5000 bars");
    std::int64_t last = 0, previous_end = 0;
    for (const auto &b : bars) {
        for (double p : {b.open, b.high, b.low, b.close})
            if (!std::isfinite(p) || p < 1e-9 || p > 1e12)
                throw std::invalid_argument("Invalid OHLC price");
        if (!std::isfinite(b.volume) || b.volume < 0 || b.volume > 1e18 || b.utc_offset_seconds < -50400 ||
            b.utc_offset_seconds > 50400 || b.low > std::min(b.open, b.close) ||
            b.high < std::max(b.open, b.close) || b.time <= last || b.time > 32503680000000LL ||
            b.time < previous_end || b.end <= b.time || b.end - b.time > 2 * day_ms)
            throw std::invalid_argument("Invalid bar chronology or range");
        last = b.time;
        previous_end = b.end;
    }
}
Distribution distribution(const std::vector<double> &prices, int bins) {
    validate_values(prices);
    if (bins < 2 || bins > 100)
        throw std::invalid_argument("Histogram bins");
    Distribution out;
    for (std::size_t i = 1; i < prices.size(); ++i) {
        if (prices[i] <= 0 || prices[i - 1] <= 0)
            throw std::invalid_argument("Positive prices required");
        const double r = prices[i] / prices[i - 1] - 1;
        if (!std::isfinite(r))
            throw std::invalid_argument("Return overflow");
        out.simple.push_back(r);
        out.log_returns.push_back(std::log(prices[i]) - std::log(prices[i - 1]));
    }
    const double m = average(out.simple);
    long double s2 = 0, s3 = 0, s4 = 0;
    for (double v : out.simple) {
        const long double d = v - m;
        s2 += d * d;
        s3 += d * d * d;
        s4 += d * d * d * d;
    }
    const double n = static_cast<double>(out.simple.size());
    out.mean = m;
    if (n > 1)
        out.sd = std::sqrt(static_cast<double>(s2 / (n - 1)));
    if (s2 > 0) {
        const auto variance = s2 / n;
        out.skew = static_cast<double>((s3 / n) / std::pow(variance, 1.5L));
        out.excess_kurtosis = static_cast<double>((s4 / n) / (variance * variance) - 3);
    }
    auto sorted = out.simple;
    std::sort(sorted.begin(), sorted.end());
    out.q05 = quantile(sorted, .05);
    out.median = quantile(sorted, .5);
    out.q95 = quantile(sorted, .95);
    double lo = sorted.front(), hi = sorted.back();
    if (hi == lo) {
        lo -= .0001;
        hi += .0001;
    }
    const double width = (hi - lo) / bins;
    out.bin_counts.assign(bins, 0);
    for (int i = 0; i < bins; ++i)
        out.bin_centers.push_back(lo + (i + .5) * width);
    for (double v : out.simple) {
        const int bin = std::clamp(static_cast<int>((v - lo) / width), 0, bins - 1);
        ++out.bin_counts[bin];
    }
    return out;
}
std::vector<double> acf(const std::vector<double> &values, int lags) {
    validate_values(values, 3);
    if (lags < 1 || lags > 100 || static_cast<std::size_t>(lags) >= values.size())
        throw std::invalid_argument("ACF lags");
    const double mean = average(values);
    long double denominator = 0;
    for (double v : values)
        denominator += (v - mean) * (v - mean);
    std::vector<double> result(lags + 1, unavailable());
    if (denominator <= 0)
        return result;
    result[0] = 1;
    for (int k = 1; k <= lags; ++k) {
        long double sum = 0;
        for (std::size_t i = k; i < values.size(); ++i)
            sum += (values[i] - mean) * (values[i - k] - mean);
        result[k] = static_cast<double>(sum / denominator);
    }
    return result;
}
std::vector<double> pacf(const std::vector<double> &values, int lags) {
    const auto r = acf(values, lags);
    std::vector<double> result(lags + 1, unavailable()), phi(lags + 1, 0);
    if (!std::isfinite(r[0]))
        return result;
    result[0] = 1;
    double variance = 1;
    for (int k = 1; k <= lags; ++k) {
        double sum = 0;
        for (int j = 1; j < k; ++j)
            sum += phi[j] * r[k - j];
        if (variance < 1e-14)
            break;
        const double reflection = (r[k] - sum) / variance;
        if (!std::isfinite(reflection) || std::abs(reflection) > 1 + 1e-8)
            break;
        auto next = phi;
        for (int j = 1; j < k; ++j)
            next[j] = phi[j] - reflection * phi[k - j];
        next[k] = reflection;
        phi = next;
        result[k] = reflection;
        variance *= std::max(0.0, 1 - reflection * reflection);
    }
    return result;
}
Stationarity stationarity(const std::vector<double> &values, int adf_lags, int kpss_lags) {
    validate_values(values, 40);
    if (adf_lags < 0 || adf_lags > 10 || kpss_lags < 0 || kpss_lags > 50 ||
        static_cast<std::size_t>(kpss_lags) >= values.size() / 2)
        throw std::invalid_argument("Test lags");
    Stationarity result;
    result.adf_lags = adf_lags;
    result.kpss_lags = kpss_lags;
    // Center and scale before QR. The ADF t statistic is affine-invariant with a constant.
    const double mean = average(values);
    long double total = 0;
    for (double v : values)
        total += (v - mean) * (v - mean);
    if (total < 1e-24)
        return result;
    const double scale = std::sqrt(static_cast<double>(total / values.size()));
    std::vector<double> y;
    for (double v : values)
        y.push_back((v - mean) / scale);
    const int n = static_cast<int>(y.size()) - 1 - adf_lags, p = 2 + adf_lags;
    result.observations = n;
    std::vector<std::vector<long double>> q(p, std::vector<long double>(n)),
        r(p, std::vector<long double>(p));
    std::vector<long double> target(n), qty(p), beta(p);
    for (int i = 0; i < n; ++i) {
        const int t = i + adf_lags + 1;
        target[i] = y[t] - y[t - 1];
        q[0][i] = 1;
        q[1][i] = y[t - 1];
        for (int k = 1; k <= adf_lags; ++k)
            q[1 + k][i] = y[t - k] - y[t - k - 1];
    }
    bool rank = true;
    for (int k = 0; k < p; ++k) {
        for (int j = 0; j < k; ++j) {
            long double d = 0;
            for (int i = 0; i < n; ++i)
                d += q[j][i] * q[k][i];
            r[j][k] += d;
            for (int i = 0; i < n; ++i)
                q[k][i] -= d * q[j][i];
        }
        // Reorthogonalize for nearly dependent regressors.
        for (int j = 0; j < k; ++j) {
            long double d = 0;
            for (int i = 0; i < n; ++i)
                d += q[j][i] * q[k][i];
            r[j][k] += d;
            for (int i = 0; i < n; ++i)
                q[k][i] -= d * q[j][i];
        }
        long double norm = 0;
        for (auto v : q[k])
            norm += v * v;
        r[k][k] = std::sqrt(norm);
        if (r[k][k] < 1e-12) {
            rank = false;
            break;
        }
        for (int i = 0; i < n; ++i) {
            q[k][i] /= r[k][k];
            qty[k] += q[k][i] * target[i];
        }
    }
    if (rank) {
        for (int k = p - 1; k >= 0; --k) {
            long double s = qty[k];
            for (int j = k + 1; j < p; ++j)
                s -= r[k][j] * beta[j];
            beta[k] = s / r[k][k];
        }
        long double sse = 0;
        for (int i = 0; i < n; ++i) {
            long double e = target[i];
            for (int j = 0; j < p; ++j)
                e -= q[j][i] * qty[j];
            sse += e * e;
        }
        long double diag = 0;
        for (int column = 0; column < p; ++column) {
            std::vector<long double> v(p);
            for (int k = p - 1; k >= 0; --k) {
                long double z = k == column ? 1 : 0;
                for (int j = k + 1; j < p; ++j)
                    z -= r[k][j] * v[j];
                v[k] = z / r[k][k];
            }
            diag += v[1] * v[1];
        }
        const long double se = std::sqrt(sse / (n - p) * diag);
        if (se > 1e-15)
            result.adf = static_cast<double>(beta[1] / se);
    }
    // MacKinnon 2010 constant-only, N=1, finite-sample 5% response surface.
    const double t = n;
    result.adf_critical5 = -2.86154 - 2.8903 / t - 4.234 / (t * t) - 40.040 / (t * t * t);
    long double partial = 0, eta = 0, variance = 0;
    for (double v : y) {
        partial += v;
        eta += partial * partial;
        variance += v * v;
    }
    variance /= y.size();
    for (int lag = 1; lag <= kpss_lags; ++lag) {
        long double cov = 0;
        for (std::size_t i = lag; i < y.size(); ++i)
            cov += y[i] * y[i - lag];
        variance += 2 * (1 - static_cast<long double>(lag) / (kpss_lags + 1)) * cov / y.size();
    }
    if (variance > 1e-16)
        result.kpss = static_cast<double>(eta / (y.size() * static_cast<long double>(y.size())) / variance);
    return result;
}
Correlation aligned_correlation(const std::vector<std::vector<Bar>> &series) {
    if (series.empty() || series.size() > 6)
        throw std::invalid_argument("Need up to six series");
    for (const auto &s : series)
        validate_bars(s);
    Correlation out;
    out.matrix.assign(series.size(), std::vector<double>(series.size(), unavailable()));
    std::map<std::int64_t, int> common;
    for (const auto &s : series)
        for (const auto &b : s)
            ++common[b.time];
    for (const auto &pair : common)
        if (pair.second == static_cast<int>(series.size()))
            ++out.common_bars;
    using Key = std::pair<std::int64_t, std::int64_t>;
    std::vector<std::map<Key, double>> returns;
    for (const auto &s : series) {
        std::map<Key, double> r;
        for (std::size_t i = 1; i < s.size(); ++i)
            r[{s[i - 1].time, s[i].time}] = std::log(s[i].close) - std::log(s[i - 1].close);
        returns.push_back(std::move(r));
    }
    std::vector<std::vector<double>> aligned(series.size());
    for (const auto &kv : returns.front()) {
        bool exists = true;
        for (const auto &r : returns)
            if (!r.count(kv.first))
                exists = false;
        if (exists)
            for (std::size_t k = 0; k < returns.size(); ++k)
                aligned[k].push_back(returns[k].at(kv.first));
    }
    out.common_returns = aligned[0].size();
    if (out.common_returns < 3)
        return out;
    for (std::size_t a = 0; a < series.size(); ++a)
        for (std::size_t b = 0; b < series.size(); ++b) {
            const double ma = average(aligned[a]), mb = average(aligned[b]);
            long double cov = 0, va = 0, vb = 0;
            for (std::size_t i = 0; i < out.common_returns; ++i) {
                const long double x = aligned[a][i] - ma, y = aligned[b][i] - mb;
                cov += x * y;
                va += x * x;
                vb += y * y;
            }
            if (va > 1e-24 && vb > 1e-24)
                out.matrix[a][b] = std::clamp(static_cast<double>(cov / std::sqrt(va * vb)), -1.0, 1.0);
        }
    return out;
}
CandleFeatures::CandleFeatures(int fast, int slow, int window)
    : indicators_(checked_window(window)), fast_(checked_window(fast)), slow_(checked_window(slow)),
      z_(checked_window(window)) {
    if (fast < 2 || slow < 2 || fast > 500 || slow > 500 || window < 3 || window > 500)
        throw std::invalid_argument("Feature windows");
}
Features CandleFeatures::push(const Bar &b) {
    Features out;
    out.fill(unavailable());
    const auto v = indicators_.push(b.close);
    fast_.push(b.close);
    slow_.push(b.close);
    z_.push(b.close);
    out[idx(Feature::Close)] = b.close;
    out[idx(Feature::Volume)] = b.volume;
    if (fast_.ready())
        out[idx(Feature::FastSma)] = fast_.mean();
    if (slow_.ready())
        out[idx(Feature::SlowSma)] = slow_.mean();
    out[idx(Feature::Ema)] = v.ema;
    out[idx(Feature::Rsi)] = v.rsi;
    out[idx(Feature::Macd)] = v.macd;
    out[idx(Feature::MacdSignal)] = v.signal;
    out[idx(Feature::Upper)] = v.upper;
    out[idx(Feature::Lower)] = v.lower;
    const double tr =
        std::isfinite(previous_)
            ? std::max({b.high - b.low, std::abs(b.high - previous_), std::abs(b.low - previous_)})
            : b.high - b.low;
    if (std::isfinite(previous_)) {
        if (tr_count_ < 14) {
            tr_sum_ += tr;
            if (++tr_count_ == 14)
                atr_ = tr_sum_ / 14;
        } else
            atr_ = (atr_ * 13 + tr) / 14;
    }
    out[idx(Feature::Atr)] = atr_;
    out[idx(Feature::Natr)] = atr_ / b.close * 100;
    if (z_.ready() && z_.stddev() > 1e-12)
        out[idx(Feature::Zscore)] = (b.close - z_.mean()) / z_.stddev();
    previous_ = b.close;
    return out;
}
std::vector<Features> candle_features(const std::vector<Bar> &bars, int fast, int slow, int window) {
    validate_bars(bars);
    CandleFeatures f(fast, slow, window);
    std::vector<Features> result;
    result.reserve(bars.size());
    for (const auto &b : bars)
        result.push_back(f.push(b));
    return result;
}
bool valid_rule_expression(const RuleGroup &g) {
    if (g.rules.size() > 4 || g.expression.size() > 7)
        return false;
    if (g.expression.empty())
        return true;
    unsigned seen = 0;
    int depth = 0;
    for (int token : g.expression) {
        if (token >= 0) {
            if (static_cast<std::size_t>(token) >= g.rules.size() || (seen & (1U << token)))
                return false;
            seen |= 1U << token;
            ++depth;
        } else {
            if ((token != -1 && token != -2) || depth < 2)
                return false;
            --depth;
        }
    }
    return depth == 1 && seen == ((1U << g.rules.size()) - 1);
}
std::vector<int> compile_rule_expression(const std::array<int, 3> &joins, const std::array<int, 4> &opens,
                                         const std::array<int, 4> &closes) {
    std::vector<int> output;
    std::array<int, 16> operators{};
    int count = 0, nesting = 0;
    const auto emit_operator = [&] { output.push_back(operators[--count]); };
    for (int i = 0; i < 4; ++i) {
        if (opens[i] < 0 || opens[i] > 3 || closes[i] < 0 || closes[i] > 3)
            throw std::invalid_argument("Bracket count out of range");
        if (i) {
            if (joins[i - 1] < 0 || joins[i - 1] > 1)
                throw std::invalid_argument("Unknown logical join");
            const int op = joins[i - 1] ? -2 : -1;
            while (count && operators[count - 1] != 0 && (operators[count - 1] == -1 || op == -2))
                emit_operator();
            operators[count++] = op;
        }
        for (int j = 0; j < opens[i]; ++j) {
            if (++nesting > 3)
                throw std::invalid_argument("Rule nesting limit 3");
            operators[count++] = 0;
        }
        output.push_back(i);
        for (int j = 0; j < closes[i]; ++j) {
            if (nesting-- <= 0)
                throw std::invalid_argument("Unmatched closing bracket");
            while (count && operators[count - 1] != 0)
                emit_operator();
            if (!count)
                throw std::invalid_argument("Unmatched closing bracket");
            --count;
        }
    }
    if (nesting)
        throw std::invalid_argument("Unclosed bracket");
    while (count)
        emit_operator();
    return output;
}
std::string rule_expression_text(const RuleGroup &g) {
    if (!valid_rule_expression(g))
        return "INVALID";
    if (g.expression.empty()) {
        std::string out;
        for (std::size_t i = 0; i < g.rules.size(); ++i)
            if (g.rules[i].enabled) {
                if (!out.empty())
                    out += g.all ? " AND " : " OR ";
                out += std::to_string(i + 1);
            }
        return out.empty() ? "EMPTY" : out;
    }
    std::array<std::string, 4> stack;
    int count = 0;
    for (int token : g.expression) {
        if (token >= 0)
            stack[count++] = g.rules[token].enabled ? std::to_string(token + 1) : "";
        else {
            auto right = std::move(stack[--count]);
            auto &left = stack[count - 1];
            if (left.empty())
                left = std::move(right);
            else if (!right.empty())
                left = "(" + left + (token == -1 ? " AND " : " OR ") + right + ")";
        }
    }
    return stack[0].empty() ? "EMPTY" : stack[0];
}
bool evaluate(const RuleGroup &g, const Features &p, const Features &c) {
    if (!valid_rule_expression(g))
        return false;
    bool any = false, result = g.all;
    std::array<bool, 4> values{};
    for (std::size_t index = 0; index < g.rules.size(); ++index) {
        const auto &rule = g.rules[index];
        if (!rule.enabled)
            continue;
        any = true;
        const auto lhs = idx(rule.lhs), rhs = idx(rule.rhs);
        if (lhs >= c.size() || rhs >= c.size())
            return false;
        const double a = c[lhs], b = rule.rhs == Feature::Constant ? rule.value : c[rhs], pa = p[lhs],
                     pb = rule.rhs == Feature::Constant ? rule.value : p[rhs];
        bool valid = std::isfinite(a) && std::isfinite(b), match = false;
        if (valid) {
            if (rule.op == Compare::Above)
                match = a > b;
            else if (rule.op == Compare::Below)
                match = a < b;
            else if (std::isfinite(pa) && std::isfinite(pb))
                match = rule.op == Compare::CrossAbove ? (a > b && pa <= pb) : (a < b && pa >= pb);
        }
        values[index] = match;
        if (g.all)
            result &= match;
        else
            result |= match;
    }
    if (g.expression.empty())
        return any && result;
    struct Value {
        bool present = false, match = false;
    };
    std::array<Value, 4> stack{};
    int count = 0;
    for (int token : g.expression) {
        if (token >= 0)
            stack[count++] = {g.rules[token].enabled, values[token]};
        else {
            const auto right = stack[--count];
            auto &left = stack[count - 1];
            if (!left.present)
                left = right;
            else if (right.present)
                left.match = token == -1 ? left.match && right.match : left.match || right.match;
        }
    }
    return stack[0].present && stack[0].match;
}
PaperEngine::PaperEngine(const SimConfig &c, bool capture_trace)
    : config_(c), features_(c.strategy.fast, c.strategy.slow, c.strategy.indicator_window),
      cash_(c.initial_cash), peak_(c.initial_cash), session_equity_(c.initial_cash),
      capture_trace_(capture_trace) {
    validate_config(c);
    current_.fill(unavailable());
    previous_ = current_;
    result_.equity.push_back(cash_);
    result_.drawdowns.push_back(0);
    pending_entry_ = c.strategy.buy_hold;
}
void PaperEngine::warmup(const Bar &b) {
    validate_bars({b});
    if (last_.time && (b.time <= last_.time || b.time < last_.end))
        throw std::invalid_argument("Warmup chronology");
    if (!result_.times.empty())
        throw std::logic_error("Warmup after trading");
    previous_ = current_;
    current_ = features_.push(b);
    entry_streak_ =
        evaluate(config_.strategy.entry, previous_, current_) ? std::min(50, entry_streak_ + 1) : 0;
    last_ = b;
}
void PaperEngine::arm_from_warmup() {
    if (!config_.strategy.buy_hold)
        pending_entry_ = entry_streak_ >= config_.entry_confirmation;
    entry_wait_ = config_.signal_delay;
}
void PaperEngine::log(const Bar &b, std::int64_t qty, double price, const std::string &reason) {
    if (result_.journal.size() < 10000)
        result_.journal.push_back({b.time, qty, price, cash_, cash_ + units_ * price, reason});
}
void PaperEngine::entry(const Bar &b) {
    pending_entry_ = false;
    if (units_ || halted_ || session_halted_ || cooldown_remaining_ > 0 ||
        entries_today_ >= config_.max_entries_per_day)
        return;
    const double raw_price = b.open * (1 + (config_.spread_bps / 2 + config_.slippage_bps) / 10000);
    const double price = config_.tick_size > 0
                             ? std::ceil(raw_price / config_.tick_size - 1e-10) * config_.tick_size
                             : raw_price;
    const double equity = cash_, budget = std::min(cash_, equity * config_.allocation);
    long double lots = std::floor(std::max(0.0, budget - config_.fixed_fee) /
                                  (price * config_.lot * (1 + config_.commission_rate)));
    if (config_.stop_fraction > 0) {
        const double raw_stop = price * (1 - config_.stop_fraction) *
                                (1 - (config_.spread_bps / 2 + config_.slippage_bps) / 10000);
        const double stop_fill =
            config_.tick_size > 0
                ? std::max(0.0, std::floor(raw_stop / config_.tick_size + 1e-10) * config_.tick_size)
                : raw_stop;
        const double unit_risk = price - stop_fill + config_.commission_rate * (price + stop_fill);
        const double risk_budget = std::max(0.0, equity * config_.risk_per_trade - 2 * config_.fixed_fee);
        lots = std::min(lots, std::floor(static_cast<long double>(risk_budget) / (unit_risk * config_.lot)));
    }
    lots = std::min(lots, static_cast<long double>(1000000000 / config_.lot));
    std::int64_t qty = static_cast<std::int64_t>(lots) * config_.lot;
    const auto requested = qty;
    if (config_.participation > 0)
        qty = std::min(qty, liquidity_cap_);
    if (qty <= 0) {
        log(b, 0, b.open, "entry rejected: cash/risk/previous-volume lot limit");
        trace_fill(requested, 0, b.open, b.open, 0, "entry rejected: cash/risk/previous-volume lot limit");
        return;
    }
    const double notional = qty * price, fee = notional * config_.commission_rate + config_.fixed_fee;
    if (notional + fee > cash_ + 1e-7) {
        log(b, 0, price, "entry rejected: cash including fees");
        trace_fill(requested, 0, b.open, price, 0, "entry rejected: cash including fees");
        return;
    }
    if (config_.participation > 0)
        liquidity_cap_ -= qty;
    result_.metrics.fees += fee;
    result_.metrics.execution_cost += qty * (price - b.open);
    cash_ -= notional + fee;
    units_ = qty;
    partial_remaining_ = 0;
    partial_wait_ = 0;
    partial_complete_ = false;
    held_bars_ = 0;
    entry_streak_ = exit_streak_ = 0;
    entry_price_ = price;
    cost_basis_ = notional + fee;
    trade_profit_ = 0;
    traded_ += notional;
    ++result_.metrics.fills;
    ++entries_today_;
    bought_ = true;
    log(b, qty, price, "BUY: previous closed-bar rule; next open; unfilled remainder cancelled");
    trace_fill(requested, qty, b.open, price, fee,
               "BUY: previous closed-bar rule; next open; unfilled remainder cancelled");
}
void PaperEngine::exit(const Bar &b, double raw, const std::string &reason, bool unlimited,
                       std::int64_t planned_slice) {
    const bool partial = planned_slice > 0;
    if (!partial) {
        partial_remaining_ = 0;
        partial_wait_ = 0;
        if (config_.partial_target_fraction > 0)
            partial_complete_ = true;
    }
    if (!units_) {
        pending_exit_ = false;
        return;
    }
    const auto requested_units =
        partial ? std::min(planned_slice, std::max<std::int64_t>(0, units_ - config_.lot)) : units_;
    std::int64_t qty = requested_units;
    if (config_.participation > 0 && !unlimited)
        qty = std::min(qty, liquidity_cap_);
    if (qty <= 0) {
        if (partial) {
            partial_wait_ = 0;
            log(b, 0, raw, "partial exit pending: previous-volume cap");
            trace_fill(-requested_units, 0, raw, raw, 0, "partial exit pending: previous-volume cap");
            return;
        }
        pending_exit_ = true;
        exit_wait_ = 0;
        log(b, 0, raw, "exit pending: previous-volume cap");
        trace_fill(-units_, 0, raw, raw, 0, "exit pending: previous-volume cap");
        return;
    }
    const double raw_price = raw * (1 - (config_.spread_bps / 2 + config_.slippage_bps) / 10000);
    const double price =
        config_.tick_size > 0
            ? std::max(0.0, std::floor(raw_price / config_.tick_size + 1e-10) * config_.tick_size)
            : raw_price;
    const auto requested = -requested_units;
    const double notional = qty * price, fee = notional * config_.commission_rate + config_.fixed_fee;
    const double allocated = cost_basis_ * static_cast<double>(qty) / units_;
    const double pnl = notional - fee - allocated;
    trade_profit_ += pnl;
    if (config_.participation > 0 && !unlimited)
        liquidity_cap_ -= qty;
    result_.metrics.fees += fee;
    result_.metrics.execution_cost += qty * (raw - price);
    cash_ += notional - fee;
    cost_basis_ -= allocated;
    units_ -= qty;
    traded_ += notional;
    ++result_.metrics.fills;
    if (partial)
        ++result_.metrics.partial_fills;
    log(b, -qty, price, reason);
    trace_fill(requested, -qty, raw, price, fee, reason);
    if (partial) {
        partial_remaining_ -= qty;
        partial_wait_ = 0;
        if (partial_remaining_ == 0)
            partial_complete_ = true;
    } else
        pending_exit_ = units_ > 0;
    if (!partial && pending_exit_)
        exit_wait_ = 0;
    if (!units_) {
        held_bars_ = entry_streak_ = exit_streak_ = 0;
        cooldown_remaining_ = config_.cooldown_bars;
        closed_this_bar_ = true;
        time_exit_ = false;
        ++result_.metrics.closed_trades;
        if (trade_profit_ > 0) {
            ++wins_;
            gross_profit_ += trade_profit_;
        } else
            gross_loss_ -= trade_profit_;
        cost_basis_ = 0;
    }
}
void PaperEngine::trace_fill(std::int64_t requested, std::int64_t quantity, double trigger, double price,
                             double fee, const std::string &reason) {
    if (!capture_trace_)
        return;
    auto *trace = tracing_step_ ? &trace_ : (result_.trace.empty() ? nullptr : &result_.trace.back());
    if (!trace)
        return;
    if (trace->fill_count >= static_cast<int>(trace->fills.size()))
        throw std::logic_error("Execution trace bound");
    auto &fill = trace->fills[trace->fill_count++];
    fill = {requested,
            quantity,
            units_,
            trigger,
            price,
            fee,
            quantity
                ? std::abs(static_cast<double>(quantity)) * (quantity > 0 ? price - trigger : trigger - price)
                : 0,
            cash_,
            reason};
}
void PaperEngine::request_stop(const std::string &reason) {
    halted_ = true;
    pending_entry_ = false;
    partial_remaining_ = 0;
    pending_exit_ = units_ > 0;
    result_.halt_reason = reason;
}
void PaperEngine::mark(const Bar &b) {
    const double equity = cash_ + units_ * b.close;
    peak_ = std::max(peak_, equity);
    const double dd = peak_ > 0 ? std::max(0.0, 1 - equity / peak_) : 0;
    result_.metrics.max_drawdown = std::max(result_.metrics.max_drawdown, dd);
    result_.equity.push_back(equity);
    result_.drawdowns.push_back(dd);
    if (result_.times.empty())
        result_.start_time = b.time;
    result_.times.push_back(b.end);
}
void PaperEngine::step(const Bar &b) {
    if (finished_)
        throw std::logic_error("Finished engine");
    if (result_.times.size() >= 5000)
        throw std::length_error("Virtual session limit 5000 bars");
    validate_bars({b});
    if (last_.time && (b.time <= last_.time || b.time < last_.end))
        throw std::invalid_argument("Non-increasing engine time");
    closed_this_bar_ = false;
    if (capture_trace_) {
        trace_ = {};
        trace_.time = b.time;
        trace_.units_before = units_;
        trace_.cash_before = cash_;
        tracing_step_ = true;
    }
    const auto day = (b.time + static_cast<std::int64_t>(b.utc_offset_seconds) * 1000) / day_ms;
    if (day != session_) {
        session_ = day;
        entries_today_ = 0;
        session_halted_ = false;
        session_equity_ = result_.equity.back();
    }
    const long double volume_cap = last_.volume * config_.participation / config_.lot;
    liquidity_cap_ = static_cast<std::int64_t>(std::min(1000000000.0L, std::floor(volume_cap))) * config_.lot;
    if (halted_ || (pending_exit_ && exit_wait_-- <= 0))
        exit(b, b.open,
             halted_      ? "risk/manual halt: next open"
             : time_exit_ ? "time exit: next open"
                          : "SELL: previous closed-bar exit; next open");
    if (pending_entry_ && !units_ && entry_wait_-- <= 0)
        entry(b);
    if (units_ && partial_remaining_ > 0 && !pending_exit_ && !halted_ && !session_halted_) {
        const bool stop_gap =
            config_.stop_fraction > 0 && b.open <= entry_price_ * (1 - config_.stop_fraction);
        const bool target_gap =
            config_.target_fraction > 0 && b.open >= entry_price_ * (1 + config_.target_fraction);
        if (!stop_gap && !target_gap && partial_wait_-- <= 0)
            exit(b, b.open, "PARTIAL SELL: close-confirmed profit stage; next open", false,
                 partial_remaining_);
    }
    if (units_) {
        const double stop = entry_price_ * (1 - config_.stop_fraction),
                     target = entry_price_ * (1 + config_.target_fraction);
        const bool sl = config_.stop_fraction > 0 && b.low <= stop,
                   tp = config_.target_fraction > 0 && b.high >= target;
        if (config_.stop_fraction > 0 && b.open <= stop)
            exit(b, b.open, "stop gap: opening price");
        else if (config_.target_fraction > 0 && b.open >= target)
            exit(b, b.open, "target gap: market trigger at open");
        else if (sl && tp)
            exit(b, config_.pessimistic ? stop : target,
                 config_.pessimistic ? "both touched: stop-first assumption"
                                     : "both touched: target-first assumption");
        else if (sl)
            exit(b, stop, "stop touched: market-trigger model");
        else if (tp)
            exit(b, target, "target touched: market-trigger model");
    }
    previous_ = current_;
    current_ = features_.push(b);
    last_ = b;
    mark(b);
    const double equity = result_.equity.back();
    const bool entry_condition = evaluate(config_.strategy.entry, previous_, current_),
               exit_condition = evaluate(config_.strategy.exit, previous_, current_);
    if (units_) {
        ++held_bars_;
        exit_streak_ = exit_condition ? std::min(50, exit_streak_ + 1) : 0;
        entry_streak_ = 0;
    } else {
        entry_streak_ = !cooldown_remaining_ && entry_condition ? std::min(50, entry_streak_ + 1) : 0;
        exit_streak_ = 0;
    }
    if (equity <= 0 || result_.drawdowns.back() >= config_.max_drawdown) {
        if (!halted_)
            log(b, 0, b.close, "RISK: drawdown/equity limit; exit next available open");
        request_stop("drawdown/equity limit");
    }
    if (session_equity_ > 0 && 1 - equity / session_equity_ >= config_.daily_loss) {
        session_halted_ = true;
        pending_exit_ = units_ > 0;
        exit_wait_ = 0;
        log(b, 0, b.close, "RISK: session loss limit; exit next available open");
    }
    if (!halted_ && !session_halted_) {
        if (units_) {
            if (!time_exit_ && config_.max_holding_bars > 0 && held_bars_ >= config_.max_holding_bars) {
                exit_wait_ = 0;
                time_exit_ = pending_exit_ = true;
                log(b, 0, b.close, "SIGNAL: time exit, pending next open");
            } else if (!pending_exit_ && exit_streak_ >= config_.exit_confirmation) {
                exit_wait_ = config_.signal_delay;
                pending_exit_ = true;
                log(b, 0, b.close, "SIGNAL: exit, pending next open");
            }
        } else if (!pending_entry_ && !config_.strategy.buy_hold &&
                   entries_today_ < config_.max_entries_per_day && !cooldown_remaining_ &&
                   entry_streak_ >= config_.entry_confirmation) {
            pending_entry_ = true;
            entry_wait_ = config_.signal_delay;
            log(b, 0, b.close, "SIGNAL: entry, pending next open");
        }
    }
    if (pending_exit_ || halted_ || session_halted_) {
        partial_remaining_ = 0;
        partial_wait_ = 0;
        if (config_.partial_target_fraction > 0)
            partial_complete_ = true;
    } else if (units_ && !partial_complete_ && partial_remaining_ == 0 &&
               config_.partial_target_fraction > 0 &&
               b.close >= entry_price_ * (1 + config_.partial_target_fraction)) {
        partial_complete_ = true;
        const auto lots = static_cast<std::int64_t>(
            std::floor((units_ / config_.lot) * config_.partial_exit_fraction + 1e-12));
        partial_remaining_ = std::min(lots * config_.lot, units_ - config_.lot);
        if (partial_remaining_ > 0) {
            partial_wait_ = config_.signal_delay;
            partial_complete_ = false;
            log(b, 0, b.close, "SIGNAL: partial profit stage, pending next open");
        } else
            log(b, 0, b.close, "partial exit skipped: less than one lot or no remaining lot");
    }
    if (capture_trace_) {
        trace_.units_after = units_;
        trace_.cash_after = cash_;
        trace_.equity = equity;
        trace_.entry_condition = entry_condition;
        trace_.exit_condition = exit_condition;
        trace_.entry_streak = entry_streak_;
        trace_.exit_streak = exit_streak_;
        trace_.held_bars = held_bars_;
        trace_.cooldown_remaining = cooldown_remaining_;
        trace_.pending_entry = pending_entry_;
        trace_.pending_exit = pending_exit_;
        trace_.entry_wait = std::max(0, entry_wait_);
        trace_.exit_wait = std::max(0, exit_wait_);
        trace_.partial_remaining = partial_remaining_;
        trace_.partial_wait = std::max(0, partial_wait_);
        trace_.partial_complete = partial_complete_;
        trace_.gate = halted_                                         ? "halt"
                      : session_halted_                               ? "session halt"
                      : pending_exit_                                 ? "exit pending"
                      : pending_entry_                                ? "entry pending"
                      : partial_remaining_ > 0                        ? "partial pending"
                      : units_                                        ? "holding"
                      : cooldown_remaining_                           ? "cooldown"
                      : entries_today_ >= config_.max_entries_per_day ? "entry limit"
                      : entry_condition                               ? "confirmation"
                                                                      : "no entry condition";
        result_.trace.push_back(std::move(trace_));
        tracing_step_ = false;
    }
    if (cooldown_remaining_ > 0 && !closed_this_bar_)
        --cooldown_remaining_;
}
void PaperEngine::finish() {
    if (finished_)
        return;
    if (config_.liquidate_end && !result_.times.empty() && units_) {
        exit(last_, last_.close, "end of predefined test: close liquidation, volume cap waived", true);
        result_.equity.back() = cash_;
        peak_ = std::max(peak_, cash_);
        result_.drawdowns.back() = std::max(0.0, 1 - cash_ / peak_);
        result_.metrics.max_drawdown = std::max(result_.metrics.max_drawdown, result_.drawdowns.back());
        if (capture_trace_ && !result_.trace.empty()) {
            auto &trace = result_.trace.back();
            trace.units_after = units_;
            trace.cash_after = cash_;
            trace.equity = cash_;
            trace.pending_entry = trace.pending_exit = false;
            trace.partial_remaining = 0;
            trace.partial_wait = 0;
            trace.partial_complete = partial_complete_;
            trace.gate = "predefined end";
        }
    }
    finished_ = true;
}
SimResult PaperEngine::result() const {
    auto out = result_;
    out.cash = cash_;
    out.units = units_;
    out.partial_remaining = partial_remaining_;
    out.unrealized = units_ ? units_ * last_.close - cost_basis_ : 0;
    out.halted = halted_;
    auto &m = out.metrics;
    m.total_return = out.equity.back() / config_.initial_cash - 1;
    m.turnover = traded_ / config_.initial_cash;
    if (m.closed_trades)
        m.win_rate = static_cast<double>(wins_) / m.closed_trades;
    if (gross_loss_ > 0)
        m.profit_factor = gross_profit_ / gross_loss_;
    std::vector<double> r;
    for (std::size_t i = 1; i < out.equity.size(); ++i)
        if (out.equity[i - 1] > 0)
            r.push_back(out.equity[i] / out.equity[i - 1] - 1);
    if (r.size() >= 2) {
        const double mean = average(r);
        long double sum = 0, down = 0;
        for (double v : r) {
            sum += (v - mean) * (v - mean);
            down += std::min(v, 0.0) * std::min(v, 0.0);
        }
        const double sd = std::sqrt(static_cast<double>(sum / (r.size() - 1))),
                     ds = std::sqrt(static_cast<double>(down / r.size()));
        const double factor = config_.annual_periods ? std::sqrt(config_.annual_periods) : 1;
        if (sd > 1e-15)
            m.sharpe = mean / sd * factor;
        if (ds > 1e-15)
            m.sortino = mean / ds * factor;
    }
    if (out.times.size() >= 2) {
        const double elapsed = (out.times.back() - out.start_time) / static_cast<double>(day_ms);
        if (elapsed >= 30 && out.equity.back() > 0) {
            m.cagr = std::expm1(std::log1p(m.total_return) * 365.25 / elapsed);
            if (!std::isfinite(m.cagr))
                m.cagr = unavailable();
            if (m.max_drawdown > 0)
                m.calmar = m.cagr / m.max_drawdown;
        }
    }
    return out;
}
SimResult backtest(const std::vector<Bar> &bars, const SimConfig &config, std::size_t start,
                   bool capture_trace) {
    validate_bars(bars);
    if (start >= bars.size())
        throw std::invalid_argument("Test start");
    PaperEngine engine(config, capture_trace);
    for (std::size_t i = 0; i < start; ++i)
        engine.warmup(bars[i]);
    if (start)
        engine.arm_from_warmup();
    for (std::size_t i = start; i < bars.size(); ++i)
        engine.step(bars[i]);
    engine.finish();
    return engine.result();
}
SimResult buy_hold(const std::vector<Bar> &bars, const SimConfig &config) {
    auto baseline = config;
    baseline.strategy.buy_hold = true;
    baseline.entry_confirmation = baseline.exit_confirmation = 1;
    baseline.cooldown_bars = baseline.max_holding_bars = 0;
    baseline.partial_target_fraction = 0;
    baseline.stop_fraction = baseline.target_fraction = 0;
    baseline.max_drawdown = baseline.daily_loss = 1;
    baseline.participation = 0;
    return backtest(bars, baseline);
}
WalkForward walk_forward(const std::vector<Bar> &bars, const SimConfig &config, int train, int test) {
    validate_bars(bars);
    if (train < 60 || train > 1000 || test < 10 || test > 500)
        throw std::invalid_argument("Walk-forward windows");
    WalkForward result;
    double capital = config.initial_cash;
    for (std::size_t boundary = train; boundary + test <= bars.size() && result.folds.size() < 20;
         boundary += test) {
        if (capital < 1)
            break;
        std::vector<Bar> training(bars.begin() + boundary - train, bars.begin() + boundary);
        double best = -INFINITY;
        int bf = 0, bs = 0;
        for (int fast : {5, 10, 20})
            for (int slow : {20, 40, 60})
                if (fast < slow) {
                    auto candidate = config;
                    candidate.strategy.fast = fast;
                    candidate.strategy.slow = slow;
                    const auto score = backtest(training, candidate).metrics.total_return;
                    if (score > best) {
                        best = score;
                        bf = fast;
                        bs = slow;
                    }
                }
        auto selected = config;
        selected.initial_cash = capital;
        selected.strategy.fast = bf;
        selected.strategy.slow = bs;
        std::vector<Bar> evaluation(bars.begin() + boundary - train, bars.begin() + boundary + test);
        const auto evaluated = backtest(evaluation, selected, train);
        result.folds.push_back(
            {boundary - train, boundary, boundary + test, bf, bs, best, evaluated.metrics.total_return});
        capital *= 1 + evaluated.metrics.total_return;
    }
    result.compounded_return = capital / config.initial_cash - 1;
    return result;
}
} // namespace quant
