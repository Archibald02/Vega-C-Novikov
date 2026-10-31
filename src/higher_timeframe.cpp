#include "quant/research.hpp"
#include <limits>
namespace quant {
namespace {
constexpr std::int64_t hour = 3600000, day = 24 * hour;
int higher_window(int n) {
    if (n < 2 || n > 100)
        throw std::invalid_argument("Higher SMA window 2..100");
    return n;
}
} // namespace
ClosedHourTrend::ClosedHourTrend(int factor, int fast, int slow)
    : factor_(factor), fast_window_(higher_window(fast)), slow_window_(higher_window(slow)),
      fast_(fast_window_), slow_(slow_window_) {
    if ((factor != 2 && factor != 3 && factor != 4 && factor != 6 && factor != 12 && factor != 24) ||
        fast >= slow)
        throw std::invalid_argument("Higher hourly factor/window order");
}
void ClosedHourTrend::validate_next(const Bar &b) const {
    validate_bars({b});
    if (b.end - b.time != hour || (previous_end_ && b.time < previous_end_))
        throw std::invalid_argument("Higher filter needs chronological closed one-hour candles");
    auto local = (b.time % day + static_cast<std::int64_t>(b.utc_offset_seconds) * 1000) % day;
    if (local < 0)
        local += day;
    if (local % hour)
        throw std::invalid_argument("Hourly candles must align to local whole hours");
    const auto width = factor_ * hour, start = b.time - local % width;
    if (start > std::numeric_limits<std::int64_t>::max() - width)
        throw std::invalid_argument("Higher bucket time overflow");
}
HigherState ClosedHourTrend::push(const Bar &b) {
    validate_next(b);
    auto local = (b.time % day + static_cast<std::int64_t>(b.utc_offset_seconds) * 1000) % day;
    if (local < 0)
        local += day;
    const auto width = factor_ * hour, start = b.time - local % width;
    const int slot = static_cast<int>((b.time - start) / hour);
    const bool offset_changed = previous_end_ && b.utc_offset_seconds != previous_offset_;
    const bool gap = previous_end_ && b.time != previous_end_;
    if (offset_changed) {
        fast_ = PriceMoments(fast_window_);
        slow_ = PriceMoments(slow_window_);
        state_.completed = 0;
        state_.last_end = 0;
        state_.fast = state_.slow = state_.close = unavailable();
    }
    if (gap || offset_changed) {
        ++state_.breaks;
        valid_ = false;
        if (state_.progress)
            ++state_.discarded;
        state_.progress = 0;
    }
    if (start != bucket_start_) {
        if (state_.progress) {
            ++state_.discarded;
            valid_ = false;
        }
        state_.progress = 0;
        bucket_start_ = start;
        if (slot != 0) {
            ++state_.discarded;
            valid_ = false;
        }
    }
    if (slot == state_.progress) {
        if (slot == 0)
            bucket_ = b;
        else {
            bucket_.high = std::max(bucket_.high, b.high);
            bucket_.low = std::min(bucket_.low, b.low);
            bucket_.close = b.close;
            bucket_.end = b.end;
            bucket_.volume += b.volume;
        }
        ++state_.progress;
        if (state_.progress == factor_) {
            if (bucket_.time != start || bucket_.end != start + width)
                throw std::logic_error("Incomplete higher candle emitted");
            fast_.push(bucket_.close);
            slow_.push(bucket_.close);
            ++state_.completed;
            state_.last_end = bucket_.end;
            state_.close = bucket_.close;
            state_.fast = fast_.ready() ? fast_.mean() : unavailable();
            state_.slow = slow_.ready() ? slow_.mean() : unavailable();
            state_.progress = 0;
            valid_ = true;
        }
    } else if (state_.progress) {
        ++state_.discarded;
        state_.progress = 0;
        valid_ = false;
    }
    previous_end_ = b.end;
    previous_offset_ = b.utc_offset_seconds;
    state_.ready = valid_ && fast_.ready() && slow_.ready();
    return state_;
}
} // namespace quant
