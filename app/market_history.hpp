#pragma once
#include "json_http.hpp"
#include <QDateTime>
#include <QMap>
#include <QPointer>
#include <QVector>
#include <functional>
#include <memory>
namespace live {
struct Candle {
    qint64 time_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};
/** Rejects malformed OHLC, nonfinite numbers, duplicate/reversed dates; max 500
 * bars. */
QVector<Candle> parse_candles(const QJsonDocument &document);
struct HistorySetInfo {
    QString key, symbol, sha256;
    QDate from, till;
    int interval = 24, preset = 1, bars = 0, added = 0, changed = 0, removed = 0;
    quint64 revision = 0;
    qint64 fetched_ms = 0, started_ms = 0;
    bool comparable = false, limited = false, resumed = false, extended = false;
    QDate checked_from, checked_till;
    bool prepended = false, rechecked = false, archived = false;
    quint64 base_revision = 0;
    int reused_bars = 0;
};
struct HistoryCheckpointInfo {
    bool available = false;
    QString symbol;
    QDate from, till;
    int interval = 24, preset = 1, bars = 0, next_offset = 0;
    qint64 saved_ms = 0;
    bool extending = false;
};
/** Only public MOEX candles, user-triggered, paginated requests, 8-entry RAM
 * cache, no token. */
class MarketHistory : public QObject {
    Q_OBJECT
  public:
    explicit MarketHistory(QObject *parent = nullptr,
                           const QUrl &public_base = QUrl("https://iss.moex.com/iss/engines/stock/"
                                                          "markets/shares/boards/TQBR/securities/"),
                           std::function<qint64()> clock = {})
        : QObject(parent), public_base_(public_base), clock_(std::move(clock)) {}
    ~MarketHistory() override { cancel(); }
    void load(const QString &symbol, int preset, bool force = false);
    void load_range(const QString &symbol, const QDate &from, const QDate &till, int interval,
                    bool force = false, int result_preset = -1);
    void cancel(bool preserve_checkpoint = true);
    /** Explicit RAM reuse, including expired entries; never performs HTTP. */
    bool restore(const QString &key);
    /** Revalidates the last successful page before continuing a RAM-only
     * checkpoint. */
    bool resume();
    /** Extend only the right edge of a complete RAM dataset, verifying an
     * overlapping tail. */
    bool extend(const QString &key, const QDate &till);
    /** Extend only the left edge, verifying the earliest overlapping Moscow
     * calendar days. */
    bool prepend(const QString &key, const QDate &from);
    /** Re-fetch an interior date window, verifying unchanged anchors on both sides. */
    bool recheck(const QString &key, const QDate &from, const QDate &till);
    void discard_checkpoint();
    QVector<HistorySetInfo> datasets() const;
    /** Superseded RAM contents: newest first, at most two per key and eight globally. */
    QVector<HistorySetInfo> archived_datasets(const QString &key) const;
    /** Open an archived revision without HTTP or replacing the current cache entry. */
    bool restore_revision(const QString &key, quint64 revision);
    /** Remove only the stored archive entry; already displayed study results remain. */
    bool remove_revision(const QString &key, quint64 revision);
    /** Remove all archived entries, leaving current cache entries and displayed results. */
    void clear_archive();
    HistoryCheckpointInfo checkpoint() const;
    HistorySetInfo current_info() const { return current_info_; }
    void clear();
    bool busy() const { return active_; }
  signals:
    void history_changed();
    void state_message(const QString &text);
    void progress(const QString &symbol, int bars, int limit);
    void truncated(const QString &symbol);
    void loaded(const QString &symbol, int preset, const QVector<live::Candle> &bars, bool cached);
    void failed(const QString &text);
    void quality_report(const QString &symbol, int received, int rejected);

  private:
    struct Entry {
        QVector<Candle> bars;
        qint64 fetched_ms = 0;
        int received = 0, rejected = 0;
        HistorySetInfo info;
    };
    struct Extension {
        Entry base;
        QDate target_from, target_till, checked_from, checked_till;
        bool prepended = false, rechecked = false;
        QString target_key;
        QDate replace_from, replace_till;
    };
    bool begin_extension(const QString &key, const QDate &date, bool before);
    void complete_extension();
    void restart_extension(const QString &reason);
    std::shared_ptr<const Extension> extension_;
    void page(const QString &symbol, int preset, const QString &key, QDate from, QDate till, int interval,
              quint64 generation, int offset);
    void complete(const QString &symbol, int preset, const QString &key, const Entry *comparison = nullptr);
    struct Request {
        QString key, symbol;
        QDate from, till;
        int interval = 24, preset = 1;
        qint64 started_ms = 0;
        bool limited = false, resumed = false, extended = false;
        QDate checked_from, checked_till;
        bool prepended = false, rechecked = false;
        quint64 base_revision = 0;
        int reused_bars = 0;
    } request_;
    struct Checkpoint {
        Request request;
        QVector<Candle> bars;
        QByteArray page_hash;
        int received = 0, rejected = 0, last_offset = 0, next_offset = 0;
        qint64 saved_ms = 0;
        std::shared_ptr<const Extension> extension;
    } checkpoint_;
    void fail(const QString &message, bool retain);
    qint64 now_ms() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }
    bool verifying_checkpoint_ = false;
    HistorySetInfo current_info_;
    quint64 revision_ = 0;
    QVector<Candle> accumulated_;
    int received_ = 0, rejected_ = 0;
    bool active_ = false;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QMap<QString, Entry> cache_;
    QVector<Entry> archive_;
    void archive_entry(const Entry &entry);
    quint64 generation_ = 0;
    qint64 resume_ms_ = 0;
    QUrl public_base_;
    std::function<qint64()> clock_;
};
} // namespace live
