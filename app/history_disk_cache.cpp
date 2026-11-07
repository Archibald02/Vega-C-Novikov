#include "history_disk_cache.hpp"
#include "quote_parser.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace live {
namespace {
void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void checkpoint(const std::atomic<bool> &cancel) { check(!cancel.load(), "Cache operation cancelled"); }
bool managed_name(const QString &name) {
    static const QRegularExpression pattern("^qpl-history-[0-9a-f]{64}\\.json$");
    return pattern.match(name).hasMatch();
}
QJsonObject metadata(const HistorySetInfo &i) {
    return {{"key", i.key}, {"symbol", i.symbol}, {"sha256", i.sha256},
            {"from", i.from.toString(Qt::ISODate)}, {"till", i.till.toString(Qt::ISODate)},
            {"interval", i.interval}, {"preset", i.preset}, {"bars", i.bars},
            {"added", i.added}, {"changed", i.changed}, {"removed", i.removed},
            {"revision", QString::number(i.revision)}, {"base_revision", QString::number(i.base_revision)},
            {"fetched_ms", QString::number(i.fetched_ms)}, {"started_ms", QString::number(i.started_ms)},
            {"comparable", i.comparable}, {"limited", i.limited}, {"resumed", i.resumed},
            {"extended", i.extended}, {"prepended", i.prepended}, {"rechecked", i.rechecked},
            {"checked_from", i.checked_from.toString(Qt::ISODate)},
            {"checked_till", i.checked_till.toString(Qt::ISODate)}, {"reused_bars", i.reused_bars}};
}
QByteArray digest(const QJsonObject &o) {
    return QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256).toHex();
}
QByteArray encode(const HistoryDataset &set) {
    HistoryDiskCache::validate(set);
    QJsonArray rows;
    for (const auto &b : set.bars)
        rows.append(QJsonArray{QString::number(b.time_ms), b.open, b.high, b.low, b.close, b.volume});
    QJsonObject o{{"format", "QuantPriceLiveHistory"}, {"schema", 1},
                  {"source", "MOEX ISS TQBR"}, {"metadata", metadata(set.info)}, {"candles", rows}};
    o["integrity"] = QString::fromLatin1(digest(o));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
HistoryDataset decode(const QByteArray &bytes) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    check(error.error == QJsonParseError::NoError && doc.isObject(), "Cache JSON");
    auto o = doc.object();
    const auto integrity = o.take("integrity");
    check(integrity.isString() && integrity.toString().toLatin1() == digest(o), "Cache integrity");
    check(o.size() == 5 && o["format"] == "QuantPriceLiveHistory" && o["schema"] == 1 &&
          o["source"] == "MOEX ISS TQBR" && o["metadata"].isObject() && o["candles"].isArray(),
          "Cache schema");
    const auto m = o["metadata"].toObject();
    HistoryDataset set;
    auto &i = set.info;
    i.key = m["key"].toString(); i.symbol = m["symbol"].toString(); i.sha256 = m["sha256"].toString();
    i.from = QDate::fromString(m["from"].toString(), Qt::ISODate);
    i.till = QDate::fromString(m["till"].toString(), Qt::ISODate);
    i.interval = m["interval"].toInt(); i.preset = m["preset"].toInt(); i.bars = m["bars"].toInt();
    i.added = m["added"].toInt(); i.changed = m["changed"].toInt(); i.removed = m["removed"].toInt();
    i.revision = m["revision"].toString().toULongLong();
    i.base_revision = m["base_revision"].toString().toULongLong();
    i.fetched_ms = m["fetched_ms"].toString().toLongLong();
    i.started_ms = m["started_ms"].toString().toLongLong();
    i.comparable = m["comparable"].toBool(); i.limited = m["limited"].toBool();
    i.resumed = m["resumed"].toBool(); i.extended = m["extended"].toBool();
    i.prepended = m["prepended"].toBool(); i.rechecked = m["rechecked"].toBool();
    i.checked_from = QDate::fromString(m["checked_from"].toString(), Qt::ISODate);
    i.checked_till = QDate::fromString(m["checked_till"].toString(), Qt::ISODate);
    i.reused_bars = m["reused_bars"].toInt();
    check(metadata(i) == m, "Cache metadata types or unknown fields");
    const auto rows = o["candles"].toArray();
    check(!rows.isEmpty() && rows.size() <= 5000, "Cache bar limit");
    for (const auto &value : rows) {
        check(value.isArray(), "Cache candle");
        const auto r = value.toArray();
        check(r.size() == 6 && r[0].isString(), "Cache candle schema");
        const auto time = r[0].toString().toLongLong();
        check(QString::number(time) == r[0].toString(), "Cache candle time");
        for (int k = 1; k < 6; ++k) check(r[k].isDouble(), "Cache candle number");
        set.bars.push_back({time, r[1].toDouble(), r[2].toDouble(), r[3].toDouble(),
                            r[4].toDouble(), r[5].toDouble()});
    }
    HistoryDiskCache::validate(set);
    set.info.from_disk = true;
    return set;
}
QVector<QFileInfo> files(const QString &directory, const std::atomic<bool> &cancel) {
    check(!QFileInfo(directory).isSymLink(), "Cache directory must not be a symlink");
    QVector<QFileInfo> result;
    QDirIterator it(directory, {"qpl-history-*.json"}, QDir::Files | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        checkpoint(cancel);
        it.next();
        if (!managed_name(it.fileName())) continue;
        check(result.size() < 64, "Too many cache files; clean the dedicated cache directory");
        result.push_back(it.fileInfo());
    }
    std::sort(result.begin(), result.end(), [](const QFileInfo &a, const QFileInfo &b) {
        if (a.lastModified() == b.lastModified()) return a.fileName() < b.fileName();
        return a.lastModified() < b.lastModified();
    });
    return result;
}
HistoryDataset read_file(const QString &directory, const QString &name, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    check(managed_name(name) && !QFileInfo(directory).isSymLink(), "Cache file name");
    const QFileInfo info(QDir(directory).filePath(name));
    check(!info.isSymLink() && info.isFile() && info.size() > 0 && info.size() <= HistoryDiskCache::file_limit,
          "Cache file size or type");
    QFile f(info.filePath());
    check(f.open(QIODevice::ReadOnly), "Cannot read cache file");
    const auto bytes = f.read(HistoryDiskCache::file_limit + 1);
    check(bytes.size() <= HistoryDiskCache::file_limit && f.error() == QFileDevice::NoError, "Cache read");
    auto set = decode(bytes);
    check(HistoryDiskCache::file_name(set.info.key) == name, "Cache filename does not match dataset");
    checkpoint(cancel);
    return set;
}
void quota_check(qint64 quota) {
    check(quota >= HistoryDiskCache::file_limit && quota <= HistoryDiskCache::default_quota, "Cache quota");
}
void prune(const QString &directory, qint64 quota, const QString &replacement, qint64 incoming,
           const std::atomic<bool> &cancel) {
    quota_check(quota);
    auto entries = files(directory, cancel);
    qint64 bytes = incoming;
    int count = incoming > 0 ? 1 : 0;
    for (const auto &f : entries) if (f.fileName() != replacement) { bytes += f.size(); ++count; }
    for (const auto &f : entries) {
        if (bytes <= quota && count <= HistoryDiskCache::entry_limit) break;
        if (f.fileName() == replacement) continue;
        checkpoint(cancel);
        check(QFile::remove(f.filePath()), "Cannot evict cache file");
        bytes -= f.size(); --count;
    }
}
void lock(QLockFile &guard, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    guard.setStaleLockTime(30000);
    check(guard.tryLock(0), "Cache is busy in another operation");
}
} // namespace
QString HistoryDiskCache::file_name(const QString &key) {
    return "qpl-history-" + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),
        QCryptographicHash::Sha256).toHex()) + ".json";
}
void HistoryDiskCache::validate(const HistoryDataset &set) {
    const auto &i = set.info;
    check(parse_symbols(i.symbol) == QStringList{i.symbol}, "Cache symbol");
    check(i.from.isValid() && i.till.isValid() && i.from >= QDate(1990, 1, 1) &&
          i.till <= QDate(3000, 12, 31) && i.from <= i.till && i.from.daysTo(i.till) <= 7305,
          "Cache date range");
    check((i.interval == 24 && (i.preset == 0 || i.preset == 1)) ||
          (i.interval == 60 && i.preset == 2), "Cache interval");
    check(i.key == i.symbol + ":" + i.from.toString(Qt::ISODate) + ":" + i.till.toString(Qt::ISODate) +
          ":" + QString::number(i.interval), "Cache key");
    check(!set.bars.isEmpty() && set.bars.size() <= 5000 && i.bars == set.bars.size(), "Cache bars");
    check(i.started_ms > 0 && i.fetched_ms >= i.started_ms && i.fetched_ms <= 32503680000000LL &&
          i.revision > 0, "Cache provenance time/revision");
    for (const auto n : {i.added, i.changed, i.removed, i.reused_bars})
        check(n >= 0 && n <= 5000, "Cache counters");
    if (i.extended || i.rechecked)
        check(i.checked_from.isValid() && i.checked_till.isValid() && i.checked_from >= i.from &&
              i.checked_till <= i.till && i.checked_from <= i.checked_till, "Cache checked range");
    qint64 previous = 0;
    for (const auto &b : set.bars) {
        check(b.time_ms > previous && b.time_ms <= 32503680000000LL, "Cache chronology");
        previous = b.time_ms;
        const auto date = QDateTime::fromMSecsSinceEpoch(b.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).date();
        check(date >= i.from && date <= i.till, "Cache candle outside requested dates");
        for (const auto n : {b.open, b.high, b.low, b.close})
            check(std::isfinite(n) && n > 0 && n <= 1e12, "Cache OHLC");
        check(std::isfinite(b.volume) && b.volume >= 0 && b.volume <= 1e18 &&
              b.low <= std::min(b.open, b.close) && b.high >= std::max(b.open, b.close) && b.low <= b.high,
              "Cache OHLCV range");
    }
    check(i.sha256 == history_content_hash(set.bars), "Cache OHLCV fingerprint");
}
HistoryDiskCatalog HistoryDiskCache::catalog(const QString &directory, const std::atomic<bool> &cancel) {
    HistoryDiskCatalog out;
    if (!QFileInfo::exists(directory)) { checkpoint(cancel); return out; }
    check(!QFileInfo(directory).isSymLink(), "Cache directory must not be a symlink");
    QLockFile guard(QDir(directory).filePath("history-cache.lock")); lock(guard, cancel);
    const auto entries = files(directory, cancel);
    for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
        out.bytes += it->size();
        try {
            const auto set = read_file(directory, it->fileName(), cancel);
            check(out.items.size() < entry_limit, "Cache entry limit");
            out.items.push_back({it->fileName(), set.info, it->size()});
        } catch (...) { checkpoint(cancel); ++out.invalid_files; }
    }
    return out;
}
HistoryDataset HistoryDiskCache::read(const QString &directory, const QString &file,
                                     const std::atomic<bool> &cancel) {
    check(QFileInfo(directory).isDir() && !QFileInfo(directory).isSymLink(), "Cache directory");
    QLockFile guard(QDir(directory).filePath("history-cache.lock")); lock(guard, cancel);
    return read_file(directory, file, cancel);
}
void HistoryDiskCache::write(const QString &directory, const HistoryDataset &set, qint64 quota,
                             const std::atomic<bool> &cancel) {
    quota_check(quota); checkpoint(cancel);
    const auto bytes = encode(set);
    check(bytes.size() <= file_limit && bytes.size() <= quota, "Cache file too large");
    check(!QFileInfo(directory).isSymLink() && QDir().mkpath(directory), "Cannot create cache directory");
    QLockFile guard(QDir(directory).filePath("history-cache.lock")); lock(guard, cancel);
    const auto name = file_name(set.info.key);
    check(!QFileInfo(QDir(directory).filePath(name)).isSymLink(), "Cache target is a symlink");
    QSaveFile file(QDir(directory).filePath(name));
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot stage cache file");
    prune(directory, quota, name, bytes.size(), cancel);
    checkpoint(cancel);
    check(file.commit(), "Cannot commit cache file");
}
void HistoryDiskCache::trim(const QString &directory, qint64 quota, const std::atomic<bool> &cancel) {
    quota_check(quota); checkpoint(cancel);
    if (!QFileInfo::exists(directory)) return;
    check(!QFileInfo(directory).isSymLink(), "Cache directory must not be a symlink");
    QLockFile guard(QDir(directory).filePath("history-cache.lock")); lock(guard, cancel);
    prune(directory, quota, {}, 0, cancel);
}
void HistoryDiskCache::clear(const QString &directory, const std::atomic<bool> &cancel) {
    checkpoint(cancel);
    if (!QFileInfo::exists(directory)) return;
    check(!QFileInfo(directory).isSymLink(), "Cache directory must not be a symlink");
    QLockFile guard(QDir(directory).filePath("history-cache.lock")); lock(guard, cancel);
    for (const auto &file : files(directory, cancel)) {
        checkpoint(cancel);
        check(QFile::remove(file.filePath()), "Cannot remove cache file");
    }
}
} // namespace live
