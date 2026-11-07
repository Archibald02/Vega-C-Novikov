#include "history_disk_cache.hpp"
#include <doctest/doctest.h>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <limits>

namespace {
live::HistoryDataset sample(QString symbol = "SBER", int count = 60, int interval = 24) {
    live::HistoryDataset set;
    auto &i = set.info;
    i.symbol = symbol; i.from = QDate(2020, 1, 1); i.till = QDate(2035, 1, 1);
    i.interval = interval; i.preset = interval == 60 ? 2 : 1;
    i.key = symbol + ":2020-01-01:2035-01-01:" + QString::number(interval);
    i.started_ms = 1700000000000LL; i.fetched_ms = i.started_ms + 1000; i.revision = 3;
    i.limited = count == 5000;
    const auto start = QDateTime(i.from, QTime(0, 0), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
    for (int k = 0; k < count; ++k) {
        const double p = 100.123456789123 + k * .001234567891;
        set.bars.push_back({start + k * (interval == 60 ? 3600000LL : 86400000LL), p, p + 2, p - 2, p + 1,
                            123456.123456 + k * .123456789});
    }
    i.bars = set.bars.size(); i.sha256 = live::history_content_hash(set.bars);
    return set;
}
QByteArray read_bytes(const QString &path) {
    QFile f(path); REQUIRE(f.open(QIODevice::ReadOnly)); return f.readAll();
}
QByteArray resign(QJsonObject o) {
    o.remove("integrity");
    o["integrity"] = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
void write_bytes(const QString &path, const QByteArray &bytes) {
    QFile f(path); REQUIRE(f.open(QIODevice::WriteOnly)); REQUIRE(f.write(bytes) == bytes.size());
}
}
TEST_CASE("History disk cache remains absent until explicit write and preserves offline provenance") {
    QTemporaryDir dir; REQUIRE(dir.isValid());
    const auto root = dir.filePath("cache");
    std::atomic<bool> cancel{false};
    CHECK(live::HistoryDiskCache::catalog(root, cancel).items.isEmpty());
    live::HistoryDiskCache::clear(root, cancel);
    CHECK_FALSE(QFileInfo::exists(root));
    const auto set = sample();
    live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel);
    const auto catalog = live::HistoryDiskCache::catalog(root, cancel);
    REQUIRE(catalog.items.size() == 1);
    CHECK(catalog.invalid_files == 0);
    const auto copy = live::HistoryDiskCache::read(root, catalog.items[0].file, cancel);
    CHECK(copy.info.from_disk);
    CHECK(copy.info.sha256 == set.info.sha256);
    CHECK(copy.info.fetched_ms == set.info.fetched_ms);
    CHECK(copy.info.started_ms == set.info.started_ms);
    CHECK(copy.info.revision == set.info.revision);
    CHECK(copy.bars[25].close == set.bars[25].close);
    live::MarketHistory history;
    int loads = 0;
    QObject::connect(&history, &live::MarketHistory::loaded, &history,
        [&](const QString &, int, const QVector<live::Candle> &bars, bool cached) {
            ++loads; CHECK(cached); CHECK(bars.size() == set.bars.size());
        });
    REQUIRE(history.restore_disk(copy));
    CHECK(loads == 1);
    CHECK(history.current_info().from_disk);
    CHECK(history.datasets().isEmpty());
    CHECK_FALSE(history.busy());
    CHECK_FALSE(history.checkpoint().available);
    CHECK_FALSE(history.dataset(set.info.key));
}
TEST_CASE("History disk cache rejects tampering schema and malicious paths without changing RAM data") {
    QTemporaryDir dir; const auto root = dir.filePath("cache"); std::atomic<bool> cancel{false};
    const auto set = sample();
    live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel);
    const auto name = live::HistoryDiskCache::file_name(set.info.key);
    const auto path = QDir(root).filePath(name);
    const auto original = read_bytes(path);
    SUBCASE("content modification") {
        auto o = QJsonDocument::fromJson(original).object();
        auto bars = o["candles"].toArray(); auto row = bars[0].toArray(); row[4] = 100.25;
        bars[0] = row; o["candles"] = bars;
        write_bytes(path, QJsonDocument(o).toJson());
    }
    SUBCASE("unknown schema") {
        auto o = QJsonDocument::fromJson(original).object(); o["schema"] = 99;
        write_bytes(path, resign(o));
    }
    SUBCASE("unknown fields with otherwise valid integrity") {
        auto o = QJsonDocument::fromJson(original).object(); o["token"] = "not-a-credential";
        write_bytes(path, resign(o));
    }
    SUBCASE("invalid candle number type with valid integrity") {
        auto o = QJsonDocument::fromJson(original).object();
        auto bars = o["candles"].toArray(); auto row = bars[0].toArray(); row[4] = "100";
        bars[0] = row; o["candles"] = bars; write_bytes(path, resign(o));
    }
    SUBCASE("malformed file") { write_bytes(path, "not json"); }
    SUBCASE("oversized file") { write_bytes(path, QByteArray(live::HistoryDiskCache::file_limit + 1, 'x')); }
    CHECK_THROWS(live::HistoryDiskCache::read(root, "../outside.json", cancel));
    if (read_bytes(path) != original) {
        CHECK_THROWS(live::HistoryDiskCache::read(root, name, cancel));
        const auto catalog = live::HistoryDiskCache::catalog(root, cancel);
        CHECK(catalog.items.isEmpty()); CHECK(catalog.invalid_files == 1);
    }
    auto invalid = set; invalid.bars[0].low = 1000;
    CHECK_THROWS(live::HistoryDiskCache::write(root, invalid, live::HistoryDiskCache::default_quota, cancel));
    live::MarketHistory history;
    CHECK_FALSE(history.restore_disk(invalid));
    CHECK(history.current_info().key.isEmpty());
    invalid = set; invalid.bars[1].time_ms = invalid.bars[0].time_ms;
    CHECK_THROWS(live::HistoryDiskCache::validate(invalid));
    invalid = set; invalid.info.key = "../../data";
    CHECK_THROWS(live::HistoryDiskCache::validate(invalid));
    invalid = set; invalid.info.from = QDate(2034, 1, 1);
    CHECK_THROWS(live::HistoryDiskCache::validate(invalid));
    invalid = set; invalid.bars[0].close = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS(live::HistoryDiskCache::validate(invalid));
    invalid = sample("SBER", 5001);
    CHECK_THROWS(live::HistoryDiskCache::validate(invalid));
}
TEST_CASE("History disk cache obeys entry and byte quotas and clear only removes owned files") {
    QTemporaryDir dir; const auto root = dir.filePath("cache"); std::atomic<bool> cancel{false};
    for (int k = 0; k < 10; ++k)
        live::HistoryDiskCache::write(root, sample("T" + QString::number(k)),
                                    live::HistoryDiskCache::default_quota, cancel);
    auto catalog = live::HistoryDiskCache::catalog(root, cancel);
    REQUIRE(catalog.items.size() == live::HistoryDiskCache::entry_limit);
    auto large = sample("LARGE", 5000);
    live::HistoryDiskCache::write(root, large, live::HistoryDiskCache::default_quota, cancel);
    large = sample("OTHER", 5000);
    live::HistoryDiskCache::write(root, large, live::HistoryDiskCache::default_quota, cancel);
    live::HistoryDiskCache::trim(root, live::HistoryDiskCache::file_limit, cancel);
    catalog = live::HistoryDiskCache::catalog(root, cancel);
    CHECK(catalog.bytes <= live::HistoryDiskCache::file_limit);
    CHECK(catalog.items.size() <= live::HistoryDiskCache::entry_limit);
    CHECK_THROWS(live::HistoryDiskCache::trim(root, 1, cancel));
    write_bytes(QDir(root).filePath("my-notes.json"), "keep me");
    live::HistoryDiskCache::clear(root, cancel);
    CHECK(live::HistoryDiskCache::catalog(root, cancel).items.isEmpty());
    CHECK(read_bytes(QDir(root).filePath("my-notes.json")) == "keep me");
}
TEST_CASE("History disk cache replaces a key atomically and handles lock cancellation and symlinks") {
    QTemporaryDir dir; const auto root = dir.filePath("cache"); std::atomic<bool> cancel{false};
    auto set = sample("SBER", 60, 60);
    live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel);
    const auto name = live::HistoryDiskCache::file_name(set.info.key);
    const auto original = read_bytes(QDir(root).filePath(name));
    {
        QLockFile guard(QDir(root).filePath("history-cache.lock")); REQUIRE(guard.tryLock(0));
        CHECK_THROWS(live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel));
    }
    cancel = true;
    CHECK_THROWS(live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel));
    CHECK_THROWS(live::HistoryDiskCache::clear(root, cancel));
    CHECK(read_bytes(QDir(root).filePath(name)) == original);
    cancel = false;
    set.bars[0].close = set.bars[0].open;
    set.info.sha256 = live::history_content_hash(set.bars); ++set.info.revision;
    set.info.extended = true; set.info.base_revision = 3; set.info.reused_bars = 10;
    set.info.checked_from = set.info.from; set.info.checked_till = set.info.till;
    live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel);
    const auto copy = live::HistoryDiskCache::read(root, name, cancel);
    CHECK(copy.info.sha256 != sample("SBER", 60, 60).info.sha256);
    CHECK(copy.info.extended); CHECK(copy.info.base_revision == 3); CHECK(copy.info.reused_bars == 10);
    CHECK(live::HistoryDiskCache::catalog(root, cancel).items.size() == 1);
#ifndef Q_OS_WIN
    QFile::remove(QDir(root).filePath(name));
    const auto outside = dir.filePath("outside.json"); write_bytes(outside, original);
    REQUIRE(QFile::link(outside, QDir(root).filePath(name)));
    CHECK_THROWS(live::HistoryDiskCache::read(root, name, cancel));
    CHECK_THROWS(live::HistoryDiskCache::write(root, set, live::HistoryDiskCache::default_quota, cancel));
    live::HistoryDiskCache::clear(root, cancel);
    CHECK(read_bytes(outside) == original);
#endif
}
