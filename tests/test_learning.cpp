#include "quant/learning.hpp"
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
TEST_CASE("Teaching commission loss equals paid fees on a flat market") {
    for (double rate : {.0001, .001, .01}) {
        const auto r = quant::commission_lesson(rate);
        CHECK(r.first.metrics.total_return == doctest::Approx(0));
        CHECK(r.first.metrics.closed_trades == 20);
        CHECK(r.second.metrics.closed_trades == 20);
        CHECK(r.second.equity.back() < 100000);
        CHECK(100000 - r.second.equity.back() == doctest::Approx(r.second.metrics.fees));
        CHECK(r.second.metrics.execution_cost == 0);
    }
    CHECK_THROWS(quant::commission_lesson(0));
}
TEST_CASE("Teaching stop gap matches an independently calculated loss") {
    const auto r = quant::gap_lesson(.01);
    REQUIRE(!r.first.journal.empty());
    CHECK(r.first.journal.front().quantity == 500);
    CHECK(r.second.journal.front().quantity == 500);
    CHECK(r.first.equity.back() == doctest::Approx(100000 - 500 * (100 - 98)));
    CHECK(r.second.equity.back() == doctest::Approx(100000 - 500 * (100 - 90)));
    CHECK(quant::gap_lesson(.05).first.journal.front().quantity == 1000);
    CHECK_THROWS(quant::gap_lesson(.051));
}
TEST_CASE("Teaching SMA choice cannot inspect the next price segment") {
    auto bars = quant::learning_bars();
    const auto original = quant::selection_lesson(bars, 16);
    for (std::size_t i = original.split; i < bars.size(); ++i) {
        bars[i].open *= .75;
        bars[i].high *= .75;
        bars[i].low *= .75;
        bars[i].close *= .75;
    }
    const auto changed = quant::selection_lesson(bars, 16);
    CHECK(original.fast == changed.fast);
    CHECK(original.slow == changed.slow);
    CHECK(original.train_scores == changed.train_scores);
    CHECK(original.first.equity == changed.first.equity);
    CHECK(original.second.equity != changed.second.equity);
    CHECK(original.first.metrics.total_return ==
          doctest::Approx(*std::max_element(original.train_scores.begin(), original.train_scores.end())));
    const auto small = quant::selection_lesson(quant::learning_bars(), 1);
    CHECK(small.first.metrics.total_return <= original.first.metrics.total_return + 1e-12);
    CHECK_THROWS(quant::selection_lesson(bars, 17));
}
