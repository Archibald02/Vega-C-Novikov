#include "quant/bootstrap.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace quant {
namespace {
// SplitMix64 and rejection sampling specify the same block-start stream on Windows and Linux.
class BlockRandom {
  public:
    explicit BlockRandom(std::uint32_t seed) : state_(seed) {}
    std::size_t start(std::size_t bound) {
        const std::uint64_t n = static_cast<std::uint64_t>(bound);
        const std::uint64_t threshold = (std::uint64_t{0} - n) % n;
        std::uint64_t x;
        do {
            x = next();
        } while (x < threshold);
        return static_cast<std::size_t>(x % n);
    }

  private:
    std::uint64_t state_;
    std::uint64_t next() {
        auto z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};
double percentile(const std::vector<double> &v, double p) {
    const double index = p * (v.size() - 1);
    const auto lo = static_cast<std::size_t>(index), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (index - lo);
}
} // namespace
BootstrapResult equity_bootstrap(const std::vector<double> &equity, const BootstrapConfig &c,
                                 const std::atomic<bool> *cancel) {
    if (c.simulations < 100 || c.simulations > 1000 || c.block_length < 1 || c.block_length > 250 ||
        equity.size() > 5001)
        throw std::invalid_argument("Bootstrap limits");
    BootstrapResult out;
    out.config = c;
    auto interrupted = [&] { return cancel && cancel->load(); };
    auto unavailable_result = [&](BootstrapStatus status) {
        BootstrapResult missing;
        missing.config = c;
        missing.observations = out.observations;
        missing.status = status;
        return missing;
    };
    if (interrupted())
        return unavailable_result(BootstrapStatus::Cancelled);
    for (double e : equity)
        if (!std::isfinite(e))
            throw std::invalid_argument("Nonfinite bootstrap equity");
    out.observations = equity.size() < 2 ? 0 : static_cast<int>(equity.size()) - 2;
    if (out.observations < 20 || c.block_length > out.observations)
        return unavailable_result(BootstrapStatus::TooShort);
    // Zero/negative close capital cannot be transformed into multiplicative returns.
    for (std::size_t i = 1; i < equity.size(); ++i)
        if (equity[i] <= 0)
            return unavailable_result(BootstrapStatus::NonPositiveCapital);
    std::vector<double> log_returns;
    log_returns.reserve(out.observations);
    for (std::size_t i = 2; i < equity.size(); ++i)
        log_returns.push_back(std::log(equity[i]) - std::log(equity[i - 1]));
    const int n = out.observations, points = std::min(101, n + 1);
    std::vector<std::vector<double>> sampled(points);
    std::vector<int> indices;
    for (int p = 0; p < points; ++p) {
        indices.push_back(p * n / (points - 1));
        sampled[p].reserve(c.simulations);
    }
    std::vector<double> drawdowns;
    out.terminal_returns.reserve(c.simulations);
    drawdowns.reserve(c.simulations);
    BlockRandom random(c.seed);
    int losses = 0;
    for (int sim = 0; sim < c.simulations; ++sim) {
        if (interrupted())
            return unavailable_result(BootstrapStatus::Cancelled);
        double path = 0, peak = 0, dd = 0;
        int done = 0, point = 1;
        sampled[0].push_back(100);
        while (done < n) {
            const auto start = random.start(static_cast<std::size_t>(n - c.block_length + 1));
            for (int j = 0; j < c.block_length && done < n; ++j) {
                if ((done % 256) == 0 && interrupted())
                    return unavailable_result(BootstrapStatus::Cancelled);
                path += log_returns[start + j];
                ++done;
                if (!std::isfinite(path) || std::abs(path) > 690)
                    return unavailable_result(BootstrapStatus::NumericalLimit);
                peak = std::max(peak, path);
                dd = std::max(dd, -std::expm1(path - peak));
                if (point < points && done == indices[point])
                    sampled[point++].push_back(100 * std::exp(path));
            }
        }
        const double result = std::expm1(path);
        out.terminal_returns.push_back(result);
        drawdowns.push_back(dd);
        losses += result < -1e-12;
    }
    if (interrupted())
        return unavailable_result(BootstrapStatus::Cancelled);
    std::sort(out.terminal_returns.begin(), out.terminal_returns.end());
    std::sort(drawdowns.begin(), drawdowns.end());
    const std::array<double, 3> probabilities{.05, .5, .95};
    for (int i = 0; i < 3; ++i) {
        out.returns[i] = percentile(out.terminal_returns, probabilities[i]);
        out.drawdowns[i] = percentile(drawdowns, probabilities[i]);
    }
    double actual_peak = equity[1];
    out.actual_drawdown = 0;
    for (std::size_t i = 1; i < equity.size(); ++i) {
        actual_peak = std::max(actual_peak, equity[i]);
        out.actual_drawdown = std::max(out.actual_drawdown, 1 - equity[i] / actual_peak);
    }
    out.actual_return = equity.back() / equity[1] - 1;
    if (!std::isfinite(out.actual_return))
        return unavailable_result(BootstrapStatus::NumericalLimit);
    for (int p = 0; p < points; ++p) {
        std::sort(sampled[p].begin(), sampled[p].end());
        const double actual = 100 * (equity[1 + indices[p]] / equity[1]);
        if (!std::isfinite(actual))
            return unavailable_result(BootstrapStatus::NumericalLimit);
        out.band.push_back({indices[p], percentile(sampled[p], .05), percentile(sampled[p], .5),
                            percentile(sampled[p], .95), actual});
    }
    out.loss_frequency = static_cast<double>(losses) / c.simulations;
    out.status = BootstrapStatus::Ready;
    return out;
}
} // namespace quant
