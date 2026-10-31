#pragma once
#include "portfolio_project.hpp"
#include "quant/portfolio_stream.hpp"
#include <optional>
namespace live {
/** Explicit private export of inputs and accepted frames. No credentials, paths or auto-connect consent. */
struct PortfolioAccountSnapshot {
    PortfolioProject project;
    bool live = false, failed = false;
    int interval = 0, stop_after = -1;
    std::string stop_reason;
    std::int64_t warmup_cutoff = 0, warmup_end = 0;
    std::size_t index = 0;
    std::vector<std::vector<quant::Bar>> frames;
    std::optional<quant::PortfolioStreamState> audit;
    std::vector<std::size_t> excluded;
    quant::PortfolioResult reference, baseline_reference;
};
struct PortfolioAccountRestore {
    PortfolioAccountSnapshot snapshot;
    quant::PortfolioSchedule schedule;
    std::unique_ptr<quant::PortfolioAccount> account, baseline;
};
constexpr qint64 portfolio_account_byte_limit = 32 * 1024 * 1024;
/** Pure causal reconstruction, ignoring reference outputs. Used also before writing. */
PortfolioAccountRestore replay_portfolio_account(const PortfolioAccountSnapshot &snapshot,
                                                 const std::atomic<bool> &cancel);
QByteArray encode_portfolio_account(const PortfolioAccountSnapshot &snapshot,
                                    const std::atomic<bool> &cancel);
/** Verifies complete reference outputs with numerical tolerance before returning a paused account. */
PortfolioAccountRestore decode_portfolio_account(const QByteArray &bytes, const std::atomic<bool> &cancel);
void write_portfolio_account(const QString &path, const PortfolioAccountSnapshot &snapshot,
                             const std::atomic<bool> &cancel);
PortfolioAccountRestore read_portfolio_account(const QString &path, const std::atomic<bool> &cancel);
} // namespace live
