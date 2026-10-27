#include "quant/analytics.hpp"
#include <cmath>
#include <stdexcept>
namespace quant {
namespace {
void validate_prices(const std::vector<double>& prices) {
    if (prices.empty()) throw std::invalid_argument("Price series is empty");
    for (double x : prices) {
        if (!std::isfinite(x) || x <= 0) throw std::invalid_argument("Prices must be finite and positive");
    }
}
}
std::vector<double> simple_returns(const std::vector<double>& prices) {
    validate_prices(prices);
    if (prices.size() < 2) throw std::invalid_argument("At least two prices required");
    std::vector<double> result;
    result.reserve(prices.size()-1);
    for (std::size_t i=1; i<prices.size(); ++i) {
        const double r = prices[i]/prices[i-1]-1.0;
        if (!std::isfinite(r)) throw std::overflow_error("Return overflow");
        result.push_back(r);
    }
    return result;
}
std::vector<double> moving_average(const std::vector<double>& prices, std::size_t window) {
    validate_prices(prices);
    if (window==0 || window>prices.size()) throw std::invalid_argument("Window must be between 1 and price count");
    std::vector<double> result;
    result.reserve(prices.size()-window+1);
    long double sum=0;
    for (std::size_t i=0; i<prices.size(); ++i) {
        sum += static_cast<long double>(prices[i])/window;
        if (i>=window) sum -= static_cast<long double>(prices[i-window])/window;
        if (i+1>=window) result.push_back(static_cast<double>(sum));
    }
    return result;
}
double annualized_volatility(const std::vector<double>& returns, double periods_per_year) {
    if (returns.size()<2 || !std::isfinite(periods_per_year) || periods_per_year<=0)
        throw std::invalid_argument("Need two returns and positive finite periods per year");
    long double mean=0, m2=0;
    std::size_t n=0;
    for (double x : returns) {
        if (!std::isfinite(x)) throw std::invalid_argument("Returns must be finite");
        ++n;
        const long double delta=static_cast<long double>(x)-mean;
        mean += delta/n;
        m2 += delta*(static_cast<long double>(x)-mean);
    }
    const double result=static_cast<double>(std::sqrt(m2/(n-1))*std::sqrt(static_cast<long double>(periods_per_year)));
    if (!std::isfinite(result)) throw std::overflow_error("Volatility overflow");
    return result;
}
}
