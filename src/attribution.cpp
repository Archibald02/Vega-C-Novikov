#include "quant/attribution.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
namespace quant {
namespace {
constexpr std::int64_t day = 86400000;
int leap_days_before(int year) {
    const int y = year - 1;
    return y / 4 - y / 100 + y / 400;
}
std::int64_t year_start(int year) {
    return 365LL * (year - 1970) + leap_days_before(year) - leap_days_before(1970);
}
bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
double finite_value(long double x) {
    const double value = static_cast<double>(x);
    if (!std::isfinite(value))
        throw std::invalid_argument("Attribution arithmetic overflow");
    return value;
}
bool close(double a, long double b) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(static_cast<long double>(a) - b) <=
               1e-8L * std::max({1.L, std::abs(static_cast<long double>(a)), std::abs(b)});
}
PerformancePeriod period(const std::vector<double> &e, std::size_t begin, std::size_t end, int key) {
    PerformancePeriod p;
    p.key = key;
    p.begin = begin;
    p.end = end;
    p.starting_equity = e[begin];
    p.ending_equity = e[end];
    p.pnl = finite_value(static_cast<long double>(e[end]) - e[begin]);
    if (e[begin] > 0) {
        p.return_fraction = finite_value(static_cast<long double>(e[end]) / e[begin] - 1);
        double peak = e[begin];
        p.max_drawdown = 0;
        for (std::size_t i = begin + 1; i <= end; ++i) {
            peak = std::max(peak, e[i]);
            p.max_drawdown =
                std::max(p.max_drawdown, finite_value(1 - static_cast<long double>(e[i]) / peak));
        }
    }
    return p;
}
} // namespace
namespace {
int month_key(const Bar &b) {
    const auto local = b.time + static_cast<std::int64_t>(b.utc_offset_seconds) * 1000;
    auto days = local / day;
    if (local < 0 && local % day)
        --days;
    int lo = 1968, hi = 3002;
    while (lo + 1 < hi) {
        const int middle = (lo + hi) / 2;
        if (year_start(middle) <= days)
            lo = middle;
        else
            hi = middle;
    }
    days -= year_start(lo);
    const int lengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 0;
    for (; month < 11; ++month) {
        const int length = lengths[month] + (month == 1 && leap(lo));
        if (days < length)
            break;
        days -= length;
    }
    return lo * 100 + month + 1;
}
} // namespace
int opening_month(const Bar &b) {
    validate_bars({b});
    return month_key(b);
}
PerformanceAttribution performance_attribution(const std::vector<Bar> &bars, const SimResult &r,
                                               const std::atomic<bool> *cancel) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    auto cancelled_report = [] {
        PerformanceAttribution out;
        out.cancelled = true;
        return out;
    };
    if (cancelled())
        return cancelled_report();
    if (bars.empty() || bars.size() > 5000 || r.equity.size() != bars.size() + 1 ||
        r.times.size() != bars.size())
        throw std::invalid_argument("Attribution history dimensions");
    validate_bars(bars);
    for (double e : r.equity)
        if (!std::isfinite(e))
            throw std::invalid_argument("Attribution nonfinite equity");
    if (r.equity.front() <= 0)
        throw std::invalid_argument("Attribution initial capital");
    for (std::size_t i = 0; i < bars.size(); ++i)
        if (r.times[i] != bars[i].end)
            throw std::invalid_argument("Attribution clock mismatch");
    PerformanceAttribution out;
    out.initial_capital = r.equity.front();
    out.final_capital = r.equity.back();
    out.total_pnl = finite_value(static_cast<long double>(out.final_capital) - out.initial_capital);
    const auto n = bars.size();
    std::vector<std::size_t> month_index(n), section_index(n);
    std::vector<int> month_keys;
    month_keys.reserve(n);
    for (const auto &b : bars)
        month_keys.push_back(month_key(b));
    std::size_t first = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        if (cancelled())
            return cancelled_report();
        if (i == n || month_keys[i] != month_keys[first]) {
            for (std::size_t j = first; j < i; ++j)
                month_index[j] = out.months.size();
            out.months.push_back(period(r.equity, first, i, month_keys[first]));
            first = i;
        }
    }
    const std::size_t count = std::min<std::size_t>(3, n);
    for (std::size_t k = 0; k < count; ++k) {
        const auto begin = n * k / count, end = n * (k + 1) / count;
        out.sections.push_back(period(r.equity, begin, end, static_cast<int>(k + 1)));
        for (auto i = begin; i < end; ++i)
            section_index[i] = k;
    }
    if (r.trace.size() != n)
        return out;
    auto invalid_trace = [&] {
        out.trace_status = AttributionTrace::Invalid;
        return out;
    };
    std::vector<PositionContribution> positions;
    positions.reserve(n);
    std::vector<double> month_fees(out.months.size()), section_fees(out.sections.size());
    PositionContribution active;
    std::int64_t units = 0;
    double cash = out.initial_capital;
    long double cash_flow = 0, closed_pnl = 0, total_fees = 0;
    int fills = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (cancelled())
            return cancelled_report();
        const auto &t = r.trace[i];
        if (t.time != bars[i].time || t.units_before != units || !close(t.cash_before, cash) ||
            t.fill_count < 0 || t.fill_count > static_cast<int>(t.fills.size()))
            return invalid_trace();
        for (int f = 0; f < t.fill_count; ++f) {
            const auto &x = t.fills[f];
            const auto q = x.quantity;
            if (!std::isfinite(x.price) || x.price < 0 || !std::isfinite(x.fee) || x.fee < 0 ||
                q == std::numeric_limits<std::int64_t>::min())
                return invalid_trace();
            if (q == 0) {
                if (x.fee != 0 || x.units_after != units || !close(x.cash_after, cash))
                    return invalid_trace();
                continue;
            }
            if ((q > 0 && units != 0) || (q < 0 && -q > units))
                return invalid_trace();
            if (q > 0) {
                active = {};
                active.entry = i;
                active.quantity = q;
                active.entry_cost = finite_value(static_cast<long double>(q) * x.price + x.fee);
                if (active.entry_cost <= 0)
                    return invalid_trace();
                cash_flow = 0;
            }
            const auto flow = -static_cast<long double>(q) * x.price - x.fee;
            if (!close(x.cash_after, static_cast<long double>(cash) + flow))
                return invalid_trace();
            units += q;
            cash = x.cash_after;
            if (x.units_after != units)
                return invalid_trace();
            cash_flow += flow;
            active.fees += x.fee;
            ++active.fills;
            ++fills;
            total_fees += x.fee;
            month_fees[month_index[i]] += x.fee;
            section_fees[section_index[i]] += x.fee;
            if (units == 0) {
                active.exit = i;
                active.pnl = finite_value(cash_flow);
                active.return_fraction = finite_value(cash_flow / active.entry_cost);
                active.initial_capital_fraction = finite_value(cash_flow / out.initial_capital);
                if (positions.size() >= n)
                    return invalid_trace();
                closed_pnl += cash_flow;
                positions.push_back(active);
                cash_flow = 0;
            }
        }
        if (t.units_after != units || !close(t.cash_after, cash) || !close(t.equity, r.equity[i + 1]) ||
            !close(t.equity,
                   static_cast<long double>(cash) + static_cast<long double>(units) * bars[i].close))
            return invalid_trace();
    }
    const long double open_pnl = units ? cash_flow + static_cast<long double>(units) * bars.back().close : 0;
    const long double residual = static_cast<long double>(out.total_pnl) - closed_pnl - open_pnl;
    double scale = out.initial_capital;
    for (double e : r.equity)
        scale = std::max(scale, std::abs(e));
    const long double tolerance =
        std::max(1e-8L, 8.L * std::numeric_limits<double>::epsilon() * scale * (fills + 1));
    if (r.units != units || !close(r.cash, cash) || std::abs(residual) > tolerance ||
        !close(r.metrics.fees, total_fees) || r.metrics.closed_trades != static_cast<int>(positions.size()) ||
        r.metrics.fills != fills)
        return invalid_trace();
    out.trace_status = AttributionTrace::Ready;
    out.positions = std::move(positions);
    out.closed_pnl = finite_value(closed_pnl);
    out.open_position_pnl = finite_value(open_pnl);
    out.fees = finite_value(total_fees);
    out.open_units = units;
    out.reconciliation_error = finite_value(static_cast<long double>(out.total_pnl) - closed_pnl - open_pnl);
    out.gross_profit = out.gross_loss = 0;
    std::vector<double> profits;
    for (const auto &p : out.positions) {
        if (p.pnl > 0) {
            out.gross_profit += p.pnl;
            profits.push_back(p.pnl);
        } else
            out.gross_loss -= p.pnl;
    }
    if (out.gross_profit > 0) {
        std::sort(profits.begin(), profits.end(), std::greater<double>());
        out.top_one_profit_share = profits.front() / out.gross_profit;
        long double top = 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, profits.size()); ++i)
            top += profits[i];
        out.top_three_profit_share = finite_value(top / out.gross_profit);
    }
    for (std::size_t i = 0; i < out.months.size(); ++i)
        out.months[i].fees = month_fees[i];
    for (std::size_t i = 0; i < out.sections.size(); ++i)
        out.sections[i].fees = section_fees[i];
    return cancelled() ? cancelled_report() : out;
}
} // namespace quant
