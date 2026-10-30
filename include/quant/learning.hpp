#pragma once
#include "research.hpp"
namespace quant {
/** Synthetic educational comparisons using the production causal execution engine. */
struct LessonResult {
    SimResult first, second;
    std::vector<double> train_scores;
    int fast = 0, slow = 0;
    std::size_t split = 0;
};
/** Flat price, alternating entry/exit. Compare zero commission with rate per fill (0.0001..0.01). */
LessonResult commission_lesson(double rate);
/** Same entry and sizing; compare a stop touch with a gap through that stop. Risk 0.001..0.05. */
LessonResult gap_lesson(double risk);
/** Deterministic invented series with a regime change, never downloaded market observations. */
std::vector<Bar> learning_bars();
/** Choose among the first 1..16 fixed SMA pairs using ONLY the first half, freeze, then evaluate
 * the second half with causal warmup. Training/testing start with separate equal cash balances. */
LessonResult selection_lesson(const std::vector<Bar> &bars, int candidates);
} // namespace quant
