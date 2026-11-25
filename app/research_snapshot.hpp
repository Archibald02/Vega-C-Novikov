#pragma once
#include "quant/research.hpp"
#include "quant/robustness.hpp"
#include "quant/bootstrap.hpp"
#include <QByteArray>
#include <QString>
#include <atomic>
#include <memory>
namespace live {
/** Bounded, frozen inputs and outputs of the trading research checks. Hidden holdout bars are
 * never included unless the user explicitly evaluated that holdout. */
struct ResearchChecks {
    int train = 120, test = 40, manual_attempts = 1;
    bool bootstrap_enabled = false;
    quant::BootstrapConfig bootstrap_config;
    std::size_t holdout_split = 0, holdout_gap = 0; // split=0 means not evaluated
    quant::SimResult baseline;
    quant::WalkForward walk;
    quant::Robustness robustness;
    quant::BootstrapResult bootstrap;
    quant::HoldoutResult holdout;
};
/** Replay the same bounded checks used by the GUI. Throws on cancellation; no partial publication. */
ResearchChecks run_research_checks(const std::vector<quant::Bar> &bars, const quant::SimConfig &config,
                                  const quant::SimResult &original, ResearchChecks settings,
                                  const std::atomic<bool> &cancelled);
/** Immutable inputs and reference output; optional research checks extend legacy backtest snapshots. */
struct ReplaySnapshot {
    std::vector<quant::Bar> bars;
    quant::SimConfig config;
    quant::SimResult reference;
    std::shared_ptr<const ResearchChecks> research;
};
/** Bounded local serialization. Explicit export includes prices and virtual capital. */
QByteArray encode_snapshot(const ReplaySnapshot &snapshot, const QString &source, const QString &name,
                           const QString &hypothesis);
/** Offline causal replay; validates schema, OHLCV hash and every stored output before comparison.
 * No network, widgets or current UI settings are used. Throws on malformed input. */
QString verify_snapshot(const QByteArray &bytes, const std::atomic<bool> &cancelled);
constexpr qint64 snapshot_byte_limit = 8 * 1024 * 1024;
} // namespace live
