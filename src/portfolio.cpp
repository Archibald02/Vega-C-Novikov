#include "quant/portfolio.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
namespace quant {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument("Portfolio inputs/limits");
}
std::int64_t lots(double units, std::int64_t lot) {
    return static_cast<std::int64_t>(std::floor(std::max(0., std::min(1e9, units)) / lot)) * lot;
}
double price(double raw, double tick, double bps, bool buy) {
    double p = raw * (1 + (buy ? 1 : -1) * bps / 10000);
    if (tick > 0) {
        const double rounded = (buy ? std::ceil(p / tick) : std::floor(p / tick)) * tick;
        p = buy ? std::max(p, rounded) : std::min(p, rounded);
    }
    return std::max(0., p);
}
bool ready(const RuleGroup &g, const Features &previous, const Features &current) {
    bool any = false, result = g.all;
    for (const auto &r : g.rules)
        if (r.enabled) {
            any = true;
            const auto a = static_cast<std::size_t>(r.lhs), b = static_cast<std::size_t>(r.rhs);
            bool value =
                std::isfinite(current[a]) && (r.rhs == Feature::Constant || std::isfinite(current[b]));
            if (r.op == Compare::CrossAbove || r.op == Compare::CrossBelow)
                value = value && std::isfinite(previous[a]) &&
                        (r.rhs == Feature::Constant || std::isfinite(previous[b]));
            if (g.all)
                result = result && value;
            else
                result = result || value;
        }
    return any && result;
}
void validate_inputs(std::vector<PortfolioAsset> &assets, const PortfolioConfig &c) {
    require(!assets.empty() && assets.size() <= 6);
    for (double x : {c.initial_cash, c.commission_rate, c.fixed_fee, c.spread_bps, c.slippage_bps,
                     c.max_weight, c.max_invested, c.group_limit, c.max_drawdown, c.daily_loss})
        require(std::isfinite(x) && x >= 0);
    require(c.initial_cash >= 1 && c.initial_cash <= 1e12 && c.commission_rate <= .1 &&
            c.fixed_fee <= c.initial_cash && c.spread_bps + c.slippage_bps <= 2000 && c.max_weight > 0 &&
            c.max_weight <= 1 && c.max_invested > 0 && c.max_invested <= 1 && c.group_limit <= 1 &&
            c.max_drawdown > 0 && c.max_drawdown <= 1 && c.daily_loss > 0 && c.daily_loss <= 1 &&
            c.rebalance_every >= 0 && c.rebalance_every <= 5000 && c.manual_stop_after >= 0 &&
            c.manual_stop_after <= 5000);
    std::sort(assets.begin(), assets.end(), [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
    double total_weight = 0;
    std::map<std::string, double> group_weights;
    for (std::size_t j = 0; j < assets.size(); ++j) {
        const auto &a = assets[j];
        require(!a.symbol.empty() && a.symbol.size() <= 32 && (!j || a.symbol != assets[j - 1].symbol) &&
                a.group.size() <= 160);
        require(std::isfinite(a.weight) && a.weight >= 0 && a.weight <= c.max_weight &&
                std::isfinite(a.tick_size) && a.tick_size >= 0 && a.tick_size <= 1e9 &&
                (a.tick_size == 0 || a.tick_size >= 1e-9) && a.lot >= 1 && a.lot <= 1000000);
        require(!c.group_limit || !a.group.empty());

        require(a.strategy.fast >= 2 && a.strategy.fast < a.strategy.slow && a.strategy.slow <= 500 &&
                a.strategy.indicator_window >= 2 && a.strategy.indicator_window <= 500);
        SimConfig validate;
        validate.strategy = a.strategy;
        PaperEngine checked(validate);
        total_weight += a.weight;
        group_weights[a.group] += a.weight;
    }
    require(total_weight <= c.max_invested + 1e-12);
    if (c.group_limit)
        for (const auto &g : group_weights)
            require(g.second <= c.group_limit + 1e-12);
}
} // namespace
PortfolioSchedule portfolio_schedule(std::vector<PortfolioAsset> assets) {
    require(!assets.empty() && assets.size() <= 6);
    std::sort(assets.begin(), assets.end(), [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
    for (std::size_t j = 0; j < assets.size(); ++j) {
        require(!assets[j].symbol.empty() && (!j || assets[j].symbol != assets[j - 1].symbol));
        validate_bars(assets[j].bars);
    }
    PortfolioSchedule s;
    std::vector<std::size_t> index(assets.size());
    for (const auto &first : assets[0].bars) {
        std::vector<Bar> frame{first};
        for (std::size_t j = 1; j < assets.size(); ++j) {
            const auto &bars = assets[j].bars;
            while (index[j] < bars.size() && bars[index[j]].time < first.time)
                ++index[j];
            if (index[j] == bars.size() || bars[index[j]].time != first.time ||
                bars[index[j]].end != first.end)
                break;
            frame.push_back(bars[index[j]]);
        }
        if (frame.size() == assets.size()) {
            for (const auto &b : frame)
                require(b.utc_offset_seconds == first.utc_offset_seconds);
            s.frames.push_back(std::move(frame));
        }
    }
    require(s.frames.size() >= 2);
    for (const auto &a : assets)
        s.excluded.push_back(a.bars.size() - s.frames.size());
    return s;
}
struct PortfolioAccount::State {
    std::vector<PortfolioAsset> assets;
    PortfolioConfig c;
    PortfolioResult r;
    std::vector<CandleFeatures> features;
    std::vector<Features> prev, current;
    std::vector<bool> active;
    std::vector<std::int64_t> target;
    bool pending = false, armed = false;
    double peak = 0, session_base = 0, traded = 0;
    std::int64_t session = -1, last_end = 0;
    std::size_t warmed = 0;
    std::vector<Bar> last_warmup;
    State(std::vector<PortfolioAsset> input, const PortfolioConfig &config)
        : assets(std::move(input)), c(config) {
        validate_inputs(assets, c);
        r.cash = peak = session_base = c.initial_cash;
        r.equity.push_back(r.cash);
        r.drawdowns.push_back(0);
        for (auto &a : assets) {
            a.bars.clear();
            a.bars.shrink_to_fit();
            r.positions.push_back({a.symbol});
            features.emplace_back(a.strategy.fast, a.strategy.slow, a.strategy.indicator_window);
            active.push_back(!a.use_rules || a.strategy.buy_hold);
        }
        prev.resize(assets.size());
        current.resize(assets.size());
        target.resize(assets.size());
        for (auto &p : current)
            p.fill(unavailable());
    }
    void validate_frame(const std::vector<Bar> &bars) const {
        require(bars.size() == assets.size());
        for (const auto &b : bars) {
            validate_bars({b});
            require(b.time >= last_end && b.time == bars[0].time && b.end == bars[0].end &&
                    b.utc_offset_seconds == bars[0].utc_offset_seconds);
        }
    }
    void log(std::int64_t t, const std::string &symbol, std::int64_t q, double p, double fee,
             const std::string &why) {
        if (r.journal.size() < 10000)
            r.journal.push_back({t, q, symbol, why, p, fee, r.cash});
        else
            ++r.dropped_events;
    }
    void plan(const std::vector<Bar> &bars, double equity) {
        for (std::size_t j = 0; j < assets.size(); ++j)
            target[j] =
                r.halted || !active[j] ? 0 : lots(equity * assets[j].weight / bars[j].close, assets[j].lot);
        pending = true;
    }
    void step(const std::vector<Bar> &bars) {
        require(r.common_bars < 5000 && (!warmed || armed));
        validate_frame(bars);
        const auto i = r.common_bars;
        if (!i && !warmed)
            r.start_time = bars[0].time;
        const double bps = c.spread_bps / 2 + c.slippage_bps;
        const auto date = (bars[0].time + bars[0].utc_offset_seconds * 1000LL) / 86400000LL;
        if (date != session) {
            session = date;
            session_base = r.equity.back();
        }
        if (pending) {
            for (std::size_t j = 0; j < assets.size(); ++j) {
                auto &p = r.positions[j];
                if (p.units <= target[j])
                    continue;
                const auto q = p.units - target[j];
                const double fill = price(bars[j].open, assets[j].tick_size, bps, false), notional = q * fill,
                             fee = notional * c.commission_rate + c.fixed_fee;
                if (fill <= 0) {
                    log(bars[j].time, assets[j].symbol, 0, fill, 0, "SELL_BLOCKED_PRICE");
                    continue;
                }
                if (r.cash + notional < fee) {
                    log(bars[j].time, assets[j].symbol, 0, fill, 0, "SELL_BLOCKED_FEE");
                    continue;
                }
                r.cash += notional - fee;
                p.units -= q;
                p.pnl += notional - fee;
                p.fees += fee;
                r.fees += fee;
                r.execution_cost += q * (bars[j].open - fill);
                traded += notional;
                ++r.fills;
                log(bars[j].time, assets[j].symbol, -q, fill, fee, r.halted ? "RISK_EXIT" : "SELL_REBALANCE");
            }
            double open_equity = r.cash, estimated_cost = 0, invested = 0;
            std::map<std::string, double> group_value;
            for (std::size_t j = 0; j < assets.size(); ++j) {
                double value = r.positions[j].units * bars[j].open;
                open_equity += value;
                invested += value;
                group_value[assets[j].group] += value;
                if (target[j] > r.positions[j].units) {
                    const double fill = price(bars[j].open, assets[j].tick_size, bps, true),
                                 q = static_cast<double>(target[j] - r.positions[j].units);
                    estimated_cost += q * (fill - bars[j].open + fill * c.commission_rate) + c.fixed_fee;
                }
            }
            const double cap_equity = std::max(0., open_equity - estimated_cost);
            for (std::size_t j = 0; j < assets.size(); ++j) {
                auto &p = r.positions[j];
                if (p.units >= target[j] || r.halted)
                    continue;
                const double raw = bars[j].open, fill = price(raw, assets[j].tick_size, bps, true),
                             cash_units = (r.cash - c.fixed_fee) / (fill * (1 + c.commission_rate));
                double allowed = std::min({static_cast<double>(target[j] - p.units), cash_units,
                                           (cap_equity * c.max_weight - p.units * raw) / raw,
                                           (cap_equity * c.max_invested - invested) / raw});
                if (c.group_limit)
                    allowed =
                        std::min(allowed, (cap_equity * c.group_limit - group_value[assets[j].group]) / raw);
                auto q = lots(allowed, assets[j].lot);
                double cost = q * fill + (q * fill * c.commission_rate + c.fixed_fee);
                while (q > 0 && cost > r.cash) {
                    q -= assets[j].lot;
                    cost = q * fill + (q * fill * c.commission_rate + c.fixed_fee);
                }
                if (!q) {
                    log(bars[j].time, assets[j].symbol, 0, fill, 0, "BUY_BLOCKED_CASH_LIMIT");
                    continue;
                }
                const double notional = q * fill, fee = notional * c.commission_rate + c.fixed_fee;
                r.cash -= cost;
                require(r.cash >= 0);
                p.units += q;
                p.pnl -= cost;
                p.fees += fee;
                r.fees += fee;
                r.execution_cost += q * (fill - raw);
                invested += q * raw;
                group_value[assets[j].group] += q * raw;
                traded += notional;
                ++r.fills;
                log(bars[j].time, assets[j].symbol, q, fill, fee, "BUY_REBALANCE");
            }
            pending = false;
        }
        double equity = r.cash;
        for (std::size_t j = 0; j < assets.size(); ++j) {
            r.positions[j].value = r.positions[j].units * bars[j].close;
            equity += r.positions[j].value;
            prev[j] = current[j];
            current[j] = features[j].push(bars[j]);
        }
        peak = std::max(peak, equity);
        const double dd = peak > 0 ? 1 - equity / peak : 0;
        r.max_drawdown = std::max(r.max_drawdown, dd);
        r.equity.push_back(equity);
        r.drawdowns.push_back(dd);
        r.times.push_back(bars[0].end);
        if (!r.halted && (equity <= 0 || dd >= c.max_drawdown ||
                          (session_base > 0 && 1 - equity / session_base >= c.daily_loss) ||
                          (c.manual_stop_after && i + 1 >= static_cast<std::size_t>(c.manual_stop_after)))) {
            r.halted = true;
            r.halt_reason = c.manual_stop_after && i + 1 >= static_cast<std::size_t>(c.manual_stop_after)
                                ? "MANUAL_STOP"
                            : dd >= c.max_drawdown ? "DRAWDOWN"
                                                   : "DAILY_LOSS";
            log(bars[0].end, "", 0, 0, 0, r.halt_reason);
        }
        bool changed = false;
        for (std::size_t j = 0; j < assets.size(); ++j)
            if (!r.halted && assets[j].use_rules && !assets[j].strategy.buy_hold) {
                const bool old = active[j];
                if (active[j]) {
                    if (evaluate(assets[j].strategy.exit, prev[j], current[j]))
                        active[j] = false;
                } else if (evaluate(assets[j].strategy.entry, prev[j], current[j]))
                    active[j] = true;
                if (old != active[j])
                    log(bars[j].end, assets[j].symbol, 0, 0, 0, active[j] ? "RULE_ON" : "RULE_OFF");
                changed |= old != active[j];
            }
        const bool periodic =
            (!warmed && i == 0) ||
            (c.rebalance_every && (i + (warmed ? 1 : 0)) % static_cast<std::size_t>(c.rebalance_every) == 0);
        if (r.halted || periodic || changed) {
            plan(bars, equity);
            if (!r.halted)
                log(bars[0].end, "", 0, 0, 0, "TARGET_NEXT_OPEN");
        }
        last_end = bars[0].end;
        ++r.common_bars;
        r.total_return = r.equity.back() / c.initial_cash - 1;
        r.turnover = traded / c.initial_cash;
    }
};
PortfolioAccount::PortfolioAccount(std::vector<PortfolioAsset> assets, const PortfolioConfig &c)
    : state_(std::make_unique<State>(std::move(assets), c)) {}
PortfolioAccount::~PortfolioAccount() = default;
PortfolioAccount::PortfolioAccount(PortfolioAccount &&) noexcept = default;
PortfolioAccount &PortfolioAccount::operator=(PortfolioAccount &&) noexcept = default;
void PortfolioAccount::step(const std::vector<Bar> &frame) { state_->step(frame); }
void PortfolioAccount::warmup(const std::vector<Bar> &frame) {
    auto &s = *state_;
    require(!s.armed && !s.r.common_bars && !s.r.halted && s.warmed < 5000);
    s.validate_frame(frame);
    for (std::size_t j = 0; j < s.assets.size(); ++j) {
        s.prev[j] = s.current[j];
        s.current[j] = s.features[j].push(frame[j]);
    }
    s.last_warmup = frame;
    s.last_end = frame[0].end;
    ++s.warmed;
}
void PortfolioAccount::arm_from_warmup() {
    auto &s = *state_;
    require(s.warmed && !s.armed && !s.r.halted);
    for (std::size_t j = 0; j < s.assets.size(); ++j)
        if (s.assets[j].use_rules && !s.assets[j].strategy.buy_hold)
            s.active[j] = evaluate(s.assets[j].strategy.entry, s.prev[j], s.current[j]);
    s.plan(s.last_warmup, s.c.initial_cash);
    s.r.start_time = s.last_end;
    s.log(s.last_end, "", 0, 0, 0, "TARGET_NEXT_OPEN");
    s.armed = true;
}
void PortfolioAccount::request_stop() {
    auto &s = *state_;
    if (s.r.halted)
        return;
    s.r.halted = true;
    s.r.halt_reason = "MANUAL_STOP";
    std::fill(s.target.begin(), s.target.end(), 0);
    s.pending = true;
    s.log(s.last_end, "", 0, 0, 0, s.r.halt_reason);
}
bool PortfolioAccount::halted() const { return state_->r.halted; }
PortfolioResult PortfolioAccount::result() const {
    auto r = state_->r;
    const auto &assets = state_->assets;
    const auto &prev = state_->prev;
    const auto &current = state_->current;
    for (std::size_t j = 0; j < r.positions.size(); ++j) {
        auto &p = r.positions[j];
        p.pnl += p.value;
        p.rules_ready = !assets[j].use_rules || assets[j].strategy.buy_hold ||
                        (ready(assets[j].strategy.entry, prev[j], current[j]) &&
                         ready(assets[j].strategy.exit, prev[j], current[j]));
    }
    return r;
}
PortfolioResult portfolio_backtest(std::vector<PortfolioAsset> assets, const PortfolioConfig &c,
                                   const std::atomic<bool> *cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Portfolio cancelled");
    auto schedule = portfolio_schedule(assets);
    PortfolioAccount account(std::move(assets), c);
    for (const auto &frame : schedule.frames) {
        if (cancel && cancel->load())
            throw std::runtime_error("Portfolio cancelled");
        account.step(frame);
    }
    auto r = account.result();
    r.excluded = std::move(schedule.excluded);
    return r;
}
PortfolioAccount portfolio_baseline_account(std::vector<PortfolioAsset> assets, PortfolioConfig c) {
    require(!assets.empty() && assets.size() <= 6);
    double total = 0;
    for (const auto &a : assets)
        total += a.weight;
    std::map<std::string, int> counts;
    int largest = 1;
    for (const auto &a : assets) {
        require(std::isfinite(a.weight) && a.weight >= 0 && a.weight <= c.max_weight);
        largest = std::max(largest, ++counts[a.group]);
    }
    require(total <= c.max_invested + 1e-12);
    const double w =
        std::min({total / assets.size(), c.max_weight, c.group_limit > 0 ? c.group_limit / largest : 1.});
    for (auto &a : assets) {
        a.weight = w;
        a.use_rules = false;
    }
    c.rebalance_every = 0;
    c.manual_stop_after = 0;
    c.max_drawdown = c.daily_loss = 1;
    return PortfolioAccount(std::move(assets), c);
}
PortfolioResult portfolio_baseline(std::vector<PortfolioAsset> assets, PortfolioConfig c,
                                   const std::atomic<bool> *cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Portfolio cancelled");
    auto schedule = portfolio_schedule(assets);
    auto account = portfolio_baseline_account(std::move(assets), c);
    for (const auto &frame : schedule.frames) {
        if (cancel && cancel->load())
            throw std::runtime_error("Portfolio cancelled");
        account.step(frame);
    }
    auto r = account.result();
    r.excluded = std::move(schedule.excluded);
    return r;
}
} // namespace quant
