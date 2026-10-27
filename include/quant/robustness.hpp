#pragma once
#include "research.hpp"
#include <atomic>
namespace quant {
struct Trial {
    std::string name;
    int fast = 0, slow = 0;
    Metrics metrics;
};
struct Robustness {
    std::vector<Trial> stability, stress;
};
/** Bounded nine-neighbour grid and six deterministic adverse scenarios; no optimisation on holdout. */
Robustness robustness(const std::vector<Bar> &bars, const SimConfig &config,
                      const std::atomic<bool> *cancel = nullptr);
struct HoldoutResult {
    SimConfig frozen;
    SimResult test;
    std::size_t split = 0, gap = 0;
};
/** Choose SMA windows using training only, skip a temporal embargo, then evaluate once on the suffix. */
HoldoutResult final_holdout(const std::vector<Bar> &bars, const SimConfig &config, std::size_t split,
                            std::size_t gap = 0);
} // namespace quant
