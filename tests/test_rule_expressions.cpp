#include "quant/research.hpp"
#include <doctest/doctest.h>
#include <optional>
namespace {
quant::RuleGroup four_rules() {
    quant::RuleGroup g;
    for (auto feature :
         {quant::Feature::Close, quant::Feature::FastSma, quant::Feature::SlowSma, quant::Feature::Ema})
        g.rules.push_back({feature, quant::Feature::Constant, quant::Compare::Above, 0, true});
    return g;
}
std::optional<bool> join(std::optional<bool> left, std::optional<bool> right, bool use_or) {
    if (!left)
        return right;
    if (!right)
        return left;
    return use_or ? *left || *right : *left && *right;
}
} // namespace
TEST_CASE("Visual parentheses compile with AND precedence and bounded bracket depth") {
    auto g = four_rules();
    g.expression = quant::compile_rule_expression({0, 1, 0}, {0, 1, 0, 0}, {0, 0, 1, 0});
    CHECK(g.expression == std::vector<int>{0, 1, 2, -2, -1, 3, -1});
    CHECK(quant::rule_expression_text(g) == "((1 AND (2 OR 3)) AND 4)");
    g.rules[3].enabled = false;
    CHECK(quant::rule_expression_text(g) == "(1 AND (2 OR 3))");
    CHECK(quant::compile_rule_expression({1, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}) ==
          std::vector<int>{0, 1, 2, -1, -2, 3, -2});
    CHECK_NOTHROW(quant::compile_rule_expression({0, 1, 0}, {3, 0, 0, 0}, {0, 0, 0, 3}));
    CHECK_THROWS(quant::compile_rule_expression({0, 1, 0}, {3, 1, 0, 0}, {0, 0, 0, 3}));
    CHECK_THROWS(quant::compile_rule_expression({0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}));
    CHECK_THROWS(quant::compile_rule_expression({0, 1, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}));
    CHECK_THROWS(quant::compile_rule_expression({0, 2, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}));
    CHECK_THROWS(quant::compile_rule_expression({0, 1, 0}, {-1, 0, 0, 0}, {0, 0, 0, 0}));
}
TEST_CASE("All 40 four-leaf Boolean trees agree for every truth and disabled-leaf combination") {
    const std::array<std::array<int, 7>, 5> shapes{{{0, 1, -3, 2, -3, 3, -3},
                                                    {0, 1, 2, -3, -3, 3, -3},
                                                    {0, 1, -3, 2, 3, -3, -3},
                                                    {0, 1, 2, -3, 3, -3, -3},
                                                    {0, 1, 2, 3, -3, -3, -3}}};
    for (int shape = 0; shape < 5; ++shape)
        for (int ops = 0; ops < 8; ++ops) {
            auto g = four_rules();
            int op_index = 0;
            for (int token : shapes[shape])
                g.expression.push_back(token == -3 ? ((ops & (1 << op_index++)) ? -2 : -1) : token);
            REQUIRE(quant::valid_rule_expression(g));
            for (int enabled = 0; enabled < 16; ++enabled)
                for (int truth = 0; truth < 16; ++truth) {
                    quant::Features current{};
                    std::array<std::optional<bool>, 4> v;
                    for (int i = 0; i < 4; ++i) {
                        g.rules[i].enabled = enabled & (1 << i);
                        current[static_cast<std::size_t>(g.rules[i].lhs)] = (truth & (1 << i)) ? 1 : -1;
                        v[i] =
                            g.rules[i].enabled ? std::optional<bool>((truth & (1 << i)) != 0) : std::nullopt;
                    }
                    const bool a = ops & 1, b = ops & 2, c = ops & 4;
                    std::optional<bool> expected;
                    if (shape == 0)
                        expected = join(join(join(v[0], v[1], a), v[2], b), v[3], c);
                    if (shape == 1)
                        expected = join(join(v[0], join(v[1], v[2], a), b), v[3], c);
                    if (shape == 2)
                        expected = join(join(v[0], v[1], a), join(v[2], v[3], b), c);
                    if (shape == 3)
                        expected = join(v[0], join(join(v[1], v[2], a), v[3], b), c);
                    if (shape == 4)
                        expected = join(v[0], join(v[1], join(v[2], v[3], a), b), c);
                    CHECK(quant::evaluate(g, current, current) == expected.value_or(false));
                }
        }
}
TEST_CASE("Disabled subgroups are omitted while enabled unavailable indicators remain false") {
    auto g = four_rules();
    g.expression = {0, 1, 2, -2, -1, 3, -1};
    g.rules[3].enabled = false;
    quant::Features f;
    f.fill(quant::unavailable());
    f[static_cast<std::size_t>(quant::Feature::Close)] = 1;
    CHECK_FALSE(quant::evaluate(g, f, f));
    g.rules[1].enabled = g.rules[2].enabled = false;
    CHECK(quant::evaluate(g, f, f));
    CHECK(quant::rule_expression_text(g) == "1");
    g.rules[0].enabled = false;
    CHECK_FALSE(quant::evaluate(g, f, f));
    CHECK(quant::rule_expression_text(g) == "EMPTY");
}
TEST_CASE("Malformed or excessive expressions fail closed before simulation") {
    for (const auto &tokens : std::vector<std::vector<int>>{{-1},
                                                            {0, 1},
                                                            {0, 0, -1},
                                                            {0, 1, -3},
                                                            {0, 1, -1, 2, -1},
                                                            {0, 1, -1, 2, -1, 999, -1},
                                                            {0, 1, -1, 2, -1, 3, -1, 0},
                                                            std::vector<int>(10000, 0)}) {
        auto g = four_rules();
        g.expression = tokens;
        CHECK_FALSE(quant::valid_rule_expression(g));
        quant::Features f{};
        CHECK_FALSE(quant::evaluate(g, f, f));
        CHECK(quant::rule_expression_text(g) == "INVALID");
        quant::SimConfig c;
        c.strategy.entry = g;
        CHECK_THROWS(quant::PaperEngine(c));
    }
}
TEST_CASE("Nested confirmation executes causally and agrees with incremental virtual account") {
    quant::SimConfig c;
    c.initial_cash = 1000;
    c.allocation = .5;
    c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = 0;
    c.stop_fraction = c.target_fraction = 0;
    c.max_drawdown = c.daily_loss = 1;
    c.liquidate_end = false;
    c.entry_confirmation = 2;
    c.strategy.entry = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 100, true},
                         {quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 103, true},
                         {quant::Feature::Close, quant::Feature::Constant, quant::Compare::Below, 102, true},
                         {quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, false}},
                        true,
                        {0, 1, 2, -2, -1, 3, -1}};
    c.strategy.exit = {{{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Below, 100, true}},
                       true};
    std::vector<quant::Bar> bars;
    for (double price : {101., 101., 102., 104., 104., 99., 99., 101., 101.}) {
        const auto t = 1700000000000LL + static_cast<std::int64_t>(bars.size()) * 86400000;
        bars.push_back({t, t + 86400000, price, price + 1, price - 1, price, 10000});
    }
    const auto batch = quant::backtest(bars, c, 0, true);
    REQUIRE(batch.trace[2].fill_count == 1);
    CHECK(batch.trace[2].fills[0].quantity > 0);
    CHECK_FALSE(batch.trace[2].entry_condition); // prior confirmed signal remains latched
    quant::PaperEngine paper(c, true);
    for (const auto &bar : bars)
        paper.step(bar);
    CHECK(paper.result().equity == batch.equity);
    auto changed = bars;
    for (std::size_t i = 5; i < changed.size(); ++i)
        changed[i].open = changed[i].close = 1, changed[i].low = .5, changed[i].high = 2;
    const auto future = quant::backtest(changed, c, 0, true);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(batch.equity[i + 1] == future.equity[i + 1]);
        CHECK(batch.trace[i].pending_entry == future.trace[i].pending_entry);
        CHECK(batch.trace[i].units_after == future.trace[i].units_after);
    }
}
