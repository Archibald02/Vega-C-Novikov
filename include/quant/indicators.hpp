#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>
namespace quant {
inline double unavailable() { return std::numeric_limits<double>::quiet_NaN(); }
/** Preallocated chronological ring. Allocation does not grow after construction. */
template<class T> class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : values_(capacity) {
        if (!capacity) throw std::invalid_argument("Zero capacity");
    }
    void push(const T& value) {
        if (size_ < values_.size()) values_[(head_ + size_++) % values_.size()] = value;
        else { values_[head_] = value; head_ = (head_ + 1) % values_.size(); }
    }
    const T& operator[](std::size_t index) const { return values_[(head_ + index) % values_.size()]; }
    const T& back() const { return (*this)[size_ - 1]; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return values_.size(); }
    std::size_t allocated_bytes() const { return values_.size() * sizeof(T); }
private:
    std::vector<T> values_;
    std::size_t head_ = 0, size_ = 0;
};
/** Rolling population moments with stable removal and addition. */
class PriceMoments {
public:
    explicit PriceMoments(std::size_t period) : values_(period) {}
    void push(double value) {
        if (values_.size() == values_.capacity()) {
            const auto n = values_.size();
            const auto old = values_[0];
            if (n <= 1) { mean_ = m2_ = 0; }
            else {
                const long double next = (n * mean_ - old) / (n - 1);
                m2_ = std::max(0.0L, m2_ - (old - mean_) * (old - next)); mean_ = next;
            }
        }
        const auto n = std::min(values_.size() + 1, values_.capacity());
        const long double delta = value - mean_;
        mean_ += delta / n; m2_ += delta * (value - mean_); values_.push(value);
    }
    double mean() const { return values_.size() ? static_cast<double>(mean_) : unavailable(); }
    double stddev() const { return values_.size() ? std::sqrt(static_cast<double>(std::max(0.0L,m2_) / values_.size())) : unavailable(); }
    bool ready() const { return values_.size() == values_.capacity(); }
private:
    RingBuffer<double> values_;
    long double mean_ = 0, m2_ = 0;
};
/** EMA seeded by the first period's SMA; NaN during warmup. */
class Ema {
public:
    explicit Ema(std::size_t period) : period_(period), alpha_(2.0 / (period + 1)) {
        if (!period) throw std::invalid_argument("Zero EMA period");
    }
    double push(double x) {
        if (count_ < period_) { sum_ += x; if (++count_ == period_) value_ = static_cast<double>(sum_ / period_); }
        else value_ += alpha_ * (x - value_);
        return value_;
    }
private:
    std::size_t period_, count_ = 0;
    double alpha_, value_ = unavailable();
    long double sum_ = 0;
};
/** Indicators computed per received observation, not on exchange candles. */
struct IndicatorValues {
    double sma = unavailable(), ema = unavailable(), upper = unavailable(), lower = unavailable();
    double rsi = unavailable(), macd = unavailable(), signal = unavailable();
};
/** O(1) SMA/EMA, Bollinger(20,2), Wilder RSI(14), MACD(12,26,9). */
class StreamingIndicators {
public:
    explicit StreamingIndicators(std::size_t period = 20) : mean_(period), bands_(20), ema_(period), fast_(12), slow_(26), signal_(9) {}
    IndicatorValues push(double price) {
        if (!std::isfinite(price) || price <= 0) throw std::invalid_argument("Invalid indicator price");
        mean_.push(price); bands_.push(price);
        IndicatorValues out; out.sma = mean_.mean(); out.ema = ema_.push(price);
        if (bands_.ready()) { out.upper = bands_.mean() + 2 * bands_.stddev(); out.lower = bands_.mean() - 2 * bands_.stddev(); }
        if (has_previous_) {
            const double diff = price - previous_, gain = std::max(0.0,diff), loss = std::max(0.0,-diff);
            if (changes_ < 14) { gain_ += gain; loss_ += loss; if (++changes_ == 14) { gain_ /= 14; loss_ /= 14; } }
            else { gain_ = (gain_ * 13 + gain) / 14; loss_ = (loss_ * 13 + loss) / 14; }
            if (changes_ >= 14) out.rsi = gain_ + loss_ == 0 ? 50 : 100 * gain_ / (gain_ + loss_);
        }
        previous_ = price; has_previous_ = true;
        const double fast = fast_.push(price), slow = slow_.push(price);
        if (std::isfinite(fast) && std::isfinite(slow)) { out.macd = fast - slow; out.signal = signal_.push(out.macd); }
        return out;
    }
private:
    PriceMoments mean_, bands_;
    Ema ema_, fast_, slow_, signal_;
    bool has_previous_ = false;
    std::size_t changes_ = 0;
    double previous_ = 0, gain_ = 0, loss_ = 0;
};
/** Lightweight experiment with rolling-origin validation at the SAME horizon. */
struct ForecastResult {
    double trend = unavailable(), naive = unavailable();
    double trend_mae = unavailable(), naive_mae = unavailable(), trend_rmse = unavailable();
    std::size_t evaluations = 0;
};
/** Fit on past windows only. Horizon is number of observations, never minutes.
 * Bounded by caller's input history; O(n) time, O(1) additional memory.
 */
ForecastResult experimental_forecast(const std::vector<double>& prices, std::size_t lookback, std::size_t horizon);
/** Hypothetical cash-funded long-position sizing; not an order. */
struct PositionSize { std::size_t lots = 0, units = 0; double cost = 0, risk = 0, reward = 0, reward_risk = 0; };
PositionSize size_long_position(double capital, double risk_percent, double entry, double stop, double target, std::size_t lot_size);
}
