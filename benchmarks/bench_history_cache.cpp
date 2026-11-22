#include "history_disk_cache.hpp"
#include <QDateTime>
#include <QTemporaryDir>
#include <QTimeZone>
#include <benchmark/benchmark.h>

namespace {
live::HistoryDataset dataset(int index) {
    live::HistoryDataset set;
    auto &i = set.info;
    i.symbol = QString("ASSET%1").arg(index);
    i.from = QDate(2000, 1, 1); i.till = QDate(2015, 1, 1);
    i.interval = 24; i.preset = 1; i.revision = 1;
    i.started_ms = i.fetched_ms = 1700000000000LL;
    i.key = i.symbol + ":2000-01-01:2015-01-01:24";
    const auto start = QDateTime(i.from, QTime(0, 0), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
    for (int n = 0; n < 5000; ++n)
        set.bars.push_back({start + n * 86400000LL, 100., 101., 99., 100., 10000.});
    i.bars = set.bars.size();
    i.sha256 = live::history_content_hash(set.bars);
    return set;
}
void BM_HistoryDisk5000(benchmark::State &state) {
    QTemporaryDir root;
    if (!root.isValid()) { state.SkipWithError("Temporary directory unavailable"); return; }
    std::atomic<bool> cancel{false};
    const auto input = dataset(0);
    for (int n = 0; n < 8; ++n)
        live::HistoryDiskCache::write(root.path(), dataset(n), live::HistoryDiskCache::default_quota, cancel);
    for (auto _ : state) {
        if (state.range(0) == 0)
            live::HistoryDiskCache::write(root.path(), input, live::HistoryDiskCache::default_quota, cancel);
        else if (state.range(0) == 1) {
            const auto r = live::HistoryDiskCache::read(root.path(), live::HistoryDiskCache::file_name(input.info.key), cancel);
            benchmark::DoNotOptimize(r.bars.data());
        } else {
            const auto r = live::HistoryDiskCache::catalog(root.path(), cancel);
            benchmark::DoNotOptimize(r.items.size());
        }
    }
}
}
// Isolated temporary files only; never uses the application's data/cache directory.
BENCHMARK(BM_HistoryDisk5000)->ArgName("0=write,1=read,2=catalog8")->Arg(0)->Arg(1)->Arg(2);
