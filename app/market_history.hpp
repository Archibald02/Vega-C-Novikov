#pragma once
#include "json_http.hpp"
#include <QDateTime>
#include <QMap>
#include <QPointer>
#include <QVector>
namespace live {
struct Candle {
    qint64 time_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};
/** Rejects malformed OHLC, nonfinite numbers, duplicate/reversed dates; max 500 bars. */
QVector<Candle> parse_candles(const QJsonDocument &document);
/** Only public MOEX candles, user-triggered, paginated requests, 8-entry RAM cache, no token. */
class MarketHistory : public QObject {
    Q_OBJECT
  public:
    explicit MarketHistory(
        QObject *parent = nullptr,
        const QUrl &public_base =
            QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities/"))
        : QObject(parent), public_base_(public_base) {}
    ~MarketHistory() override { cancel(); }
    void load(const QString &symbol, int preset, bool force = false);
    void load_range(const QString &symbol, const QDate &from, const QDate &till, int interval,
                    bool force = false, int result_preset = -1);
    void cancel();
    void clear();
    bool busy() const { return active_; }
  signals:
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
    };
    void page(const QString &symbol, int preset, const QString &key, QDate from, QDate till, int interval,
              quint64 generation, int offset);
    void complete(const QString &symbol, int preset, const QString &key);
    QVector<Candle> accumulated_;
    int received_ = 0, rejected_ = 0;
    bool active_ = false;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QMap<QString, Entry> cache_;
    quint64 generation_ = 0;
    qint64 resume_ms_ = 0;
    QUrl public_base_;
};
} // namespace live
