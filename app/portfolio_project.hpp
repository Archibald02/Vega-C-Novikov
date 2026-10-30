#pragma once
#include "portfolio_input.hpp"
#include <QByteArray>
namespace live {
/** Explicit local project, not an account checkpoint or an authenticated data
 * source. Modes: 0 weights, 1 common SMA windows, 2 SMA 5/20, 3 SMA 20/60, 4
 * RSI 14 30/70. Consent to manual metadata is deliberately not persisted.
 * Results are recalculated. */
struct PortfolioProject {
    PortfolioInput input;
    quant::PortfolioConfig config;
    std::vector<bool> enabled;
    std::vector<int> modes;
    int fast = 10, slow = 30;
};
constexpr qint64 portfolio_project_byte_limit = 16 * 1024 * 1024;
QByteArray encode_portfolio_project(const PortfolioProject &project, const std::atomic<bool> &cancel);
PortfolioProject decode_portfolio_project(const QByteArray &bytes, const std::atomic<bool> &cancel);
/** Regular files only, bounded read, atomic explicit write; no network or
 * implicit persistence. */
PortfolioProject read_portfolio_project(const QString &path, const std::atomic<bool> &cancel);
void write_portfolio_project(const QString &path, const PortfolioProject &project,
                             const std::atomic<bool> &cancel);
} // namespace live
