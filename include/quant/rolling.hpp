#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <stdexcept>
namespace quant {
/** @brief Bounded rolling price/return statistics, O(1) work per price.
 * Standard deviation is per observed update, never annualized.
 * Received updates need not be evenly spaced in time.
 */
class RollingSeries {
public:
    /** @param capacity Number of prices retained, at least 3. */
    explicit RollingSeries(std::size_t capacity = 20) : capacity_(capacity) {
        if (capacity < 3) throw std::invalid_argument("Capacity must be at least three");
    }
    /** Add a positive finite price; reject invalid and overflowing returns. */
    void push(double price) {
        if (!std::isfinite(price) || price <= 0) throw std::invalid_argument("Invalid price");
        double r = 0;
        if (!prices_.empty()) {
            r = price / prices_.back() - 1.0;
            if (!std::isfinite(r)) throw std::overflow_error("Return overflow");
        }
        const bool has_return = !prices_.empty();
        if (prices_.size() == capacity_) {
            price_sum_ -= prices_.front();
            prices_.pop_front();
            remove_return(returns_.front());
            returns_.pop_front();
        }
        prices_.push_back(price);
        price_sum_ += price;
        if (has_return) { add_return(r); returns_.push_back(r); }
    }
    /** Prices currently retained. */
    std::size_t size() const { return prices_.size(); }
    /** Mean price in the active window. Requires at least one observation. */
    double mean_price() const {
        if (prices_.empty()) throw std::logic_error("Empty series");
        return static_cast<double>(price_sum_ / prices_.size());
    }
    /** Last relative price change. Requires at least two observations. */
    double last_return() const {
        if (returns_.empty()) throw std::logic_error("No return yet");
        return returns_.back();
    }
    /** Sample standard deviation of returns. Requires at least two returns. */
    double return_stddev() const {
        if (returns_.size() < 2) throw std::logic_error("Need at least two returns");
        return static_cast<double>(std::sqrt(std::max(0.0L, m2_) / (returns_.size() - 1)));
    }
private:
    void add_return(double x) {
        const auto n = returns_.size() + 1;
        const long double delta = static_cast<long double>(x) - mean_;
        mean_ += delta / n;
        m2_ += delta * (static_cast<long double>(x) - mean_);
    }
    void remove_return(double x) {
        const auto n = returns_.size();
        if (n <= 1) { mean_ = 0; m2_ = 0; return; }
        const long double new_mean = (n * mean_ - x) / (n - 1);
        m2_ -= (static_cast<long double>(x) - mean_) * (static_cast<long double>(x) - new_mean);
        mean_ = new_mean;
        m2_ = std::max(0.0L, m2_);
    }
    std::size_t capacity_;
    std::deque<double> prices_, returns_;
    long double price_sum_ = 0, mean_ = 0, m2_ = 0;
};
}
