#pragma once
#include "quote_parser.hpp"
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
namespace live {
/** Asynchronous event-driven provider. No blocking network calls, no disk cache. */
class MarketFeed : public QObject {
    Q_OBJECT
public:
    enum class Mode { Moex, Alor, Demo };
    struct Config { Mode mode = Mode::Moex; QStringList symbols; QString refresh_token; int interval_ms = 250; int poll_interval_ms = 5000; };
    explicit MarketFeed(QObject* parent = nullptr, const QUrl& public_endpoint = QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities.json"));
    ~MarketFeed() override { stop(); }
    void start(const Config& config);
    void stop();
signals:
    void running_changed(bool active);
    void quote_received(const live::Quote& quote);
    void status_changed(const QString& text);
private:
    void authorize();
    void open_stream(const QString& access_token);
    void retry(int minimum_seconds = 0);
    void poll_moex();
    void demo_tick();
    Config config_;
    QNetworkAccessManager network_;
    QPointer<QWebSocket> socket_;
    QTimer poll_, reconnect_, renew_, heartbeat_;
    QSet<QString> pending_;
    QHash<QString, QString> subscriptions_;
    QHash<QString, double> demo_prices_;
    int generation_ = 0, attempt_ = 0;
    bool active_ = false, auth_pending_ = false;
    qint64 last_rx_ms_ = 0, moex_resume_ms_ = 0;
    int moex_failures_ = 0;
    QUrl public_endpoint_;
};
}
