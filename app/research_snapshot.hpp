#pragma once
#include "quant/research.hpp"
#include <QByteArray>
#include <QString>
#include <atomic>
namespace live {
/** Immutable inputs and reference output for the original single-instrument backtest only. */
struct ReplaySnapshot {
    std::vector<quant::Bar> bars;
    quant::SimConfig config;
    quant::SimResult reference;
};
/** Bounded local serialization. Explicit export includes prices and virtual capital. */
QByteArray encode_snapshot(const ReplaySnapshot &snapshot, const QString &source, const QString &name,
                           const QString &hypothesis);
/** Offline causal replay; validates schema, OHLCV hash and every stored output before comparison.
 * No network, widgets or current UI settings are used. Throws on malformed input. */
QString verify_snapshot(const QByteArray &bytes, const std::atomic<bool> &cancelled);
constexpr qint64 snapshot_byte_limit = 8 * 1024 * 1024;
} // namespace live
