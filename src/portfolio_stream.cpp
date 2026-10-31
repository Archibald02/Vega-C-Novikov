#include "quant/portfolio_stream.hpp"
#include <algorithm>
#include <stdexcept>
namespace quant {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument("Portfolio stream closed history/limits");
}
bool same(const Bar &a, const Bar &b) {
    return a.time == b.time && a.end == b.end && a.open == b.open && a.high == b.high && a.low == b.low &&
           a.close == b.close && a.volume == b.volume && a.utc_offset_seconds == b.utc_offset_seconds;
}
} // namespace
PortfolioStreamBuffer::PortfolioStreamBuffer(std::vector<PortfolioAsset> seed, std::int64_t watermark,
                                             std::int64_t end, std::int64_t begin, int interval)
    : last_time_(watermark), last_end_(end), coverage_(begin), interval_(interval) {
    require(!seed.empty() && seed.size() <= 6 && (interval == 60 || interval == 24) && begin > 0);
    std::sort(seed.begin(), seed.end(), [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
    for (const auto &a : seed) {
        validate_bars(a.bars);
        coverage_ = std::max(coverage_, a.bars.front().time);
    }
    require(coverage_ <= watermark && end > watermark);
    for (const auto &a : seed) {
        require(!a.symbol.empty() && (symbols_.empty() || symbols_.back() != a.symbol));
        symbols_.push_back(a.symbol);
        std::map<std::int64_t, Bar> bars;
        for (const auto &b : a.bars) {
            require(b.end - b.time == (interval == 60 ? 3600000LL : 86400000LL));
            if (b.time >= coverage_)
                bars.emplace(b.time, b);
        }
        require(bars.count(watermark) && bars.at(watermark).end == end && bars.size() <= 512);
        known_.push_back(std::move(bars));
    }
}
std::vector<std::vector<Bar>> PortfolioStreamBuffer::merge(const std::vector<std::vector<Bar>> &cycle,
                                                           std::int64_t begin, std::int64_t cutoff) {
    require(cycle.size() == known_.size() && begin > 0 && begin <= last_time_ && cutoff >= last_end_);
    begin = std::max(begin, coverage_);
    auto next = known_;
    for (std::size_t j = 0; j < cycle.size(); ++j) {
        const auto &bars = cycle[j];
        require(bars.size() <= 512);
        if (!bars.empty())
            validate_bars(bars);
        std::map<std::int64_t, Bar> received;
        for (const auto &b : bars) {
            require(b.end <= cutoff && b.end - b.time == (interval_ == 60 ? 3600000LL : 86400000LL));
            if (b.time < begin)
                continue;
            received.emplace(b.time, b);
            const auto old = next[j].find(b.time);
            if (old != next[j].end())
                require(same(old->second, b));
            else {
                require(b.time > last_time_ && b.time >= last_end_);
                next[j].emplace(b.time, b);
            }
        }
        for (const auto &old : next[j])
            if (old.first >= begin && old.second.end <= cutoff)
                require(received.count(old.first));
        next[j].erase(next[j].begin(), next[j].lower_bound(begin));
        require(next[j].size() <= 512);
    }
    std::vector<std::vector<Bar>> frames;
    for (auto it = next[0].upper_bound(last_time_); it != next[0].end(); ++it) {
        if (it->second.end > cutoff)
            continue;
        std::vector<Bar> frame{it->second};
        for (std::size_t j = 1; j < next.size(); ++j) {
            const auto other = next[j].find(it->first);
            if (other == next[j].end())
                break;
            require(other->second.end == it->second.end &&
                    other->second.utc_offset_seconds == it->second.utc_offset_seconds);
            frame.push_back(other->second);
        }
        if (frame.size() == next.size())
            frames.push_back(std::move(frame));
    }
    known_ = std::move(next);
    coverage_ = begin;
    if (!frames.empty()) {
        last_time_ = frames.back()[0].time;
        last_end_ = frames.back()[0].end;
    }
    return frames;
}
std::size_t PortfolioStreamBuffer::retained_bars() const {
    std::size_t n = 0;
    for (const auto &bars : known_)
        n += bars.size();
    return n;
}
} // namespace quant
