#pragma once
#include <cstddef>
#include <string>
#include <vector>
/** @file analytics.hpp
 * @brief Price-series analytics. Returns are fractions, not percentages.
 */
namespace quant {
/** Simple returns: prices[i] / prices[i-1] - 1.
 * @throws std::invalid_argument For fewer than two or invalid prices.
 */
std::vector<double> simple_returns(const std::vector<double>& prices);
/** Rolling mean, one value per complete window. O(n) time.
 * @throws std::invalid_argument For invalid prices or window outside [1,n].
 */
std::vector<double> moving_average(const std::vector<double>& prices, std::size_t window);
/** Sample standard deviation of returns multiplied by sqrt(periods_per_year).
 * Uses Welford's algorithm. Default assumes 252 trading days per year.
 * @throws std::invalid_argument For fewer than two returns, nonfinite returns,
 * or periods_per_year <= 0 or nonfinite.
 */
double annualized_volatility(const std::vector<double>& returns, double periods_per_year = 252.0);
}
