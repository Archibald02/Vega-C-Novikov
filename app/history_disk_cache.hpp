#pragma once
#include "market_history.hpp"
#include <atomic>

namespace live {
struct HistoryDiskItem {
    QString file;
    HistorySetInfo info;
    qint64 bytes = 0;
};
struct HistoryDiskCatalog {
    QVector<HistoryDiskItem> items;
    qint64 bytes = 0;
    int invalid_files = 0;
};
/** Opt-in public-candle storage. No network, credentials, settings or automatic startup reads.
 * Call from a worker thread. Operations are serialized with a nonblocking directory lock.
 * Individual writes are atomic; quota eviction is not a multi-file transaction.
 */
class HistoryDiskCache {
  public:
    static constexpr qint64 file_limit = 1024 * 1024;
    static constexpr qint64 default_quota = 8 * file_limit;
    static constexpr int entry_limit = 8;
    static void validate(const HistoryDataset &set);
    static QString file_name(const QString &key);
    static HistoryDiskCatalog catalog(const QString &directory, const std::atomic<bool> &cancel);
    static HistoryDataset read(const QString &directory, const QString &file,
                               const std::atomic<bool> &cancel);
    static void write(const QString &directory, const HistoryDataset &set, qint64 quota,
                      const std::atomic<bool> &cancel);
    static void trim(const QString &directory, qint64 quota, const std::atomic<bool> &cancel);
    static void clear(const QString &directory, const std::atomic<bool> &cancel);
};
} // namespace live
