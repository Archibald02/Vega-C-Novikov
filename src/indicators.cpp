#include "quant/indicators.hpp"
namespace quant {
namespace {
struct TrendWindow {
    std::size_t n; long double sum = 0, xy = 0;
    TrendWindow(const std::vector<double>& p, std::size_t count) : n(count) {
        for (std::size_t i=0;i<n;++i) { sum += p[i]; xy += i * static_cast<long double>(p[i]); }
    }
    double predict(std::size_t horizon) const {
        const long double x = (n-1)/2.0L;
        const long double denominator = n * (static_cast<long double>(n)*n - 1) / 12;
        const long double slope = (xy - x * sum) / denominator;
        return static_cast<double>(sum/n + slope * (static_cast<long double>(n-1+horizon)-x));
    }
    void advance(double old, double next) { xy = xy - (sum-old) + (n-1)*static_cast<long double>(next); sum += next-old; }
};
}
ForecastResult experimental_forecast(const std::vector<double>& p, std::size_t lookback, std::size_t horizon) {
    if (lookback < 3 || lookback > 500 || horizon < 1 || horizon > 50) throw std::invalid_argument("Invalid forecast window or horizon");
    for (double price : p) if (!std::isfinite(price) || price<=0) throw std::invalid_argument("Invalid forecast price");
    ForecastResult result;
    if (p.size() < lookback) return result;
    TrendWindow model(p,lookback);
    long double trend_abs = 0, naive_abs = 0, squared = 0;
    for (std::size_t end=lookback-1;;++end) {
        const double prediction = model.predict(horizon);
        if (end+horizon < p.size()) {
            const double actual = p[end+horizon];
            const long double error = prediction-actual;
            trend_abs += std::abs(error); squared += error*error;
            naive_abs += std::abs(p[end]-actual); ++result.evaluations;
        }
        if (end+1 == p.size()) { result.trend = prediction; result.naive = p.back(); break; }
        model.advance(p[end+1-lookback],p[end+1]);
    }
    if (result.evaluations) {
        result.trend_mae = static_cast<double>(trend_abs/result.evaluations);
        result.naive_mae = static_cast<double>(naive_abs/result.evaluations);
        result.trend_rmse = std::sqrt(static_cast<double>(squared/result.evaluations));
    }
    return result;
}
PositionSize size_long_position(double capital, double risk_percent, double entry, double stop, double target, std::size_t lot) {
    for (double x : {capital,risk_percent,entry,stop,target}) if (!std::isfinite(x) || x<=0) throw std::invalid_argument("Positive finite inputs required");
    if (risk_percent>100 || stop>=entry || target<=entry || !lot || lot>1000000) throw std::invalid_argument("Need stop < entry < target and valid lot size");
    const long double cash_limit = std::floor(static_cast<long double>(capital)/entry/lot);
    const long double risk_limit = std::floor(static_cast<long double>(capital)*(risk_percent/100)/(entry-stop)/lot);
    const auto count = std::min(cash_limit,risk_limit);
    if (count > static_cast<long double>(std::numeric_limits<std::size_t>::max()/lot)) throw std::overflow_error("Position too large");
    PositionSize r; r.lots = static_cast<std::size_t>(count); r.units = r.lots*lot;
    r.cost = r.units*entry; r.risk = r.units*(entry-stop); r.reward = r.units*(target-entry); r.reward_risk = (target-entry)/(entry-stop);
    return r;
}
}
