#include "market_feed.hpp"
#include "json_http.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
namespace live {
MarketFeed::MarketFeed(QObject* parent, const QUrl& public_endpoint) : QObject(parent), public_endpoint_(public_endpoint) {
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, &MarketFeed::authorize);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (config_.mode == Mode::Moex) poll_moex(); else demo_tick();
    });
    connect(&renew_, &QTimer::timeout, this, [this] {
        // Reauthenticate and create fresh subscriptions before the 30-minute expiry.
        if (socket_) { socket_->disconnect(this); socket_->abort(); socket_->deleteLater(); socket_ = nullptr; }
        heartbeat_.stop(); authorize();
    });
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        if (!socket_) return;
        if (QDateTime::currentMSecsSinceEpoch() - last_rx_ms_ > 45000) {
            socket_->abort(); retry();
        } else socket_->ping();
    });
}
void MarketFeed::stop() {
    active_ = false; ++generation_;
    poll_.stop(); reconnect_.stop(); renew_.stop(); heartbeat_.stop();
    if (socket_) { socket_->disconnect(this); socket_->abort(); socket_->deleteLater(); socket_ = nullptr; }
    for (auto* reply : network_.findChildren<QNetworkReply*>()) reply->abort();
    pending_.clear(); subscriptions_.clear(); config_.refresh_token.clear(); auth_pending_ = false;
    emit running_changed(false);
    emit status_changed(QStringLiteral("Отключено"));
}
void MarketFeed::start(const Config& config) {
    stop(); config_ = config;
    try {
        const auto normalized = parse_symbols(config_.symbols.join(','));
        if (normalized != config_.symbols || config_.refresh_token.size() > 8192) throw std::invalid_argument("Config");
    } catch (...) { emit status_changed(QStringLiteral("Некорректные параметры подключения.")); return; }
    if (config_.mode != Mode::Alor) config_.refresh_token.clear();
    active_ = true; attempt_ = 0; moex_failures_ = 0; moex_resume_ms_ = 0;
    config_.interval_ms = std::clamp(config_.interval_ms,100,5000);
    config_.poll_interval_ms = std::clamp(config_.poll_interval_ms,5000,60000);
    emit running_changed(true);
    if (config_.mode == Mode::Alor) {
        if (config_.refresh_token.trimmed().isEmpty()) {
            active_ = false; emit running_changed(false); emit status_changed(QStringLiteral("Нужен Refresh token АЛОР")); return;
        }
        authorize();
    } else if (config_.mode == Mode::Moex) {
        emit status_changed(QStringLiteral("MOEX ISS • данные с задержкой • один пакетный запрос раз в %1 с").arg(config_.poll_interval_ms/1000));
        poll_.start(config_.poll_interval_ms); poll_moex();
    } else {
        demo_prices_.clear();
        for (int i = 0; i < config_.symbols.size(); ++i) demo_prices_[config_.symbols[i]] = 100.0 + 80.0 * i;
        emit status_changed(QStringLiteral("ДЕМО • синтетические цены, не биржевые котировки"));
        poll_.start(250); demo_tick();
    }
}
void MarketFeed::authorize() {
    if (!active_ || config_.mode != Mode::Alor || auth_pending_) return;
    auth_pending_ = true;
    emit status_changed(QStringLiteral("АЛОР • авторизация…"));
    QNetworkRequest request{QUrl("https://oauth.alor.ru/refresh")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(15000);
    // Token goes in the HTTPS body, never the URL, log or persistent settings.
    const int generation = generation_;
    const auto body = QJsonDocument(QJsonObject{{"token", config_.refresh_token}}).toJson(QJsonDocument::Compact);
    request_json(network_, request, this, [this, generation](const HttpResult& result) {
        if (!active_ || generation != generation_) return;
        auth_pending_ = false;
        if (result.status == 401 || result.status == 403 || result.status == 400) {
            stop(); emit status_changed(QStringLiteral("АЛОР • доступ отклонён: проверьте Refresh token и права")); return;
        }
        if (!result.ok()) { retry(result.retry_after_seconds); return; }
        const auto token = result.document.object().value("AccessToken").toString();
        if (token.isEmpty() || token.size() > 8192) { stop(); emit status_changed(QStringLiteral("АЛОР • неожиданный ответ авторизации")); return; }
        open_stream(token);
    }, body, {64 * 1024, 15000});
}
void MarketFeed::open_stream(const QString& access_token) {
    if (!active_) return;
    if (socket_) { socket_->disconnect(this); socket_->abort(); socket_->deleteLater(); }
    auto* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = ws; subscriptions_.clear();
    ws->setMaxAllowedIncomingMessageSize(64 * 1024);
    ws->setMaxAllowedIncomingFrameSize(64 * 1024);
    ws->setReadBufferSize(128 * 1024);
    const int generation = generation_;
    connect(ws, &QWebSocket::connected, this, [this, ws, access_token, generation] {
        if (!active_ || generation != generation_) return;
        if(ws->requestUrl()!=QUrl("wss://api.alor.ru/ws")){stop();emit status_changed(QStringLiteral("АЛОР • неожиданный адрес соединения; подписка отменена"));return;}
        last_rx_ms_ = QDateTime::currentMSecsSinceEpoch();
        for (const auto& symbol : config_.symbols) {
            const auto guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            subscriptions_[guid] = symbol;
            QJsonObject sub{{"opcode","QuotesSubscribe"},{"exchange","MOEX"},{"code",symbol},
                {"instrumentGroup","TQBR"},{"format","Simple"},{"frequency",config_.interval_ms},
                {"guid",guid},{"token",access_token}};
            ws->sendTextMessage(QString::fromUtf8(QJsonDocument(sub).toJson(QJsonDocument::Compact)));
        }
        emit status_changed(QStringLiteral("АЛОР • WebSocket открыт, ожидаем подтверждения подписок"));
        renew_.start(25 * 60 * 1000); heartbeat_.start(15000);
    });
    connect(ws, &QWebSocket::pong, this, [this, ws, generation](quint64, const QByteArray&) { if(active_&&generation==generation_&&socket_==ws)last_rx_ms_ = QDateTime::currentMSecsSinceEpoch(); });
    connect(ws, &QWebSocket::textMessageReceived, this, [this, generation](const QString& message) {
        if (!active_ || generation != generation_) return;
        last_rx_ms_ = QDateTime::currentMSecsSinceEpoch();
        const auto object = QJsonDocument::fromJson(message.toUtf8()).object();
        const int code = object.value("httpCode").toInt(0);
        if (code >= 400) {
            // Do not display a raw server message: it may contain sensitive request data.
            stop(); emit status_changed(QStringLiteral("АЛОР • ошибка подписки HTTP %1. Проверьте токен и тикеры").arg(code)); return;
        }
        const auto response_guid = object.value(code == 200 ? "requestGuid" : "guid").toString();
        if (!subscriptions_.contains(response_guid)) return;
        if (code == 200) {
            attempt_ = 0;
            emit status_changed(QStringLiteral("АЛОР • подписки подтверждаются • поток котировок"));
        }
        const auto quote = parse_alor(message.toUtf8());
        if (quote && config_.symbols.contains(quote->symbol)) {
            attempt_ = 0;
            emit quote_received(*quote);
        }
    });
    connect(ws, &QWebSocket::disconnected, this, [this] { heartbeat_.stop(); renew_.stop(); retry(); });
    connect(ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { retry(); });
    // A handshake deadline also covers a server which never finishes upgrading.
    QTimer::singleShot(15000, ws, [this, ws] {
        if (active_ && socket_ == ws && ws->state() != QAbstractSocket::ConnectedState) { ws->abort(); retry(); }
    });
    QNetworkRequest stream_request(QUrl("wss://api.alor.ru/ws"));
    stream_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    stream_request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
    stream_request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
    stream_request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,QNetworkRequest::Manual);
    ws->open(stream_request);
}
void MarketFeed::retry(int minimum_seconds) {
    if (!active_ || config_.mode != Mode::Alor || reconnect_.isActive()) return;
    heartbeat_.stop(); renew_.stop();
    const int delay = std::max(std::clamp(minimum_seconds,0,300)*1000,std::min(30000,1000*(1<<attempt_)));
    attempt_=std::min(attempt_+1,5);
    emit status_changed(QStringLiteral("АЛОР • соединение потеряно; повтор через %1 с").arg(delay/1000));
    reconnect_.start(delay);
}

void MarketFeed::poll_moex() {
    if(!active_ || !pending_.isEmpty() || QDateTime::currentMSecsSinceEpoch() < moex_resume_ms_) return;
    pending_.insert("batch");const int generation=generation_;
    QUrl url(public_endpoint_);QUrlQuery query;
    query.addQueryItem("iss.meta","off");query.addQueryItem("iss.only","marketdata");
    query.addQueryItem("securities",config_.symbols.join(','));
    query.addQueryItem("marketdata.columns","SECID,LAST,BID,OFFER,LASTTOPREVPRICE,SYSTIME");url.setQuery(query);
    QNetworkRequest request(url);
    request_json(network_, request, this, [this, generation](const HttpResult& result) {
        if (!active_ || generation != generation_) return;
        pending_.clear();
        if (!result.ok()) {
            const int delay = std::max(result.retry_after_seconds, std::min(60, 5 * (1 << std::min(moex_failures_, 3))));
            moex_failures_ = std::min(moex_failures_ + 1, 4);
            moex_resume_ms_ = QDateTime::currentMSecsSinceEpoch() + delay * 1000;
            emit status_changed(QStringLiteral("MOEX ISS • %1 Повтор не раньше %2 с. Данные задержаны.").arg(http_error_text(result)).arg(delay)); return;
        }
        const auto quotes = parse_moex(result.document.toJson(QJsonDocument::Compact)); QSet<QString> received;
        for (const auto& q : quotes) if (config_.symbols.contains(q.symbol)) { received.insert(q.symbol); emit quote_received(q); }
        if (received.isEmpty()) {
            moex_failures_ = std::min(moex_failures_ + 1, 4);
            moex_resume_ms_ = QDateTime::currentMSecsSinceEpoch() + std::min(60, 5 * (1 << moex_failures_)) * 1000;
        } else moex_failures_ = 0;
        if (received.size() != config_.symbols.size()) emit status_changed(QStringLiteral("MOEX ISS • цены %1/%2 тикеров; нет LAST или инструмент недоступен TQBR").arg(received.size()).arg(config_.symbols.size()));
        else emit status_changed(QStringLiteral("MOEX ISS • пакет %1 тикеров • данные с задержкой").arg(received.size()));
    }, {}, {256 * 1024, 10000});
}
void MarketFeed::demo_tick() {
    for (const auto& symbol : config_.symbols) {
        auto& price = demo_prices_[symbol];
        price *= 1.0 + (QRandomGenerator::global()->generateDouble() - 0.5) * 0.002;
        Quote q; q.symbol = symbol; q.last = price; q.bid = price-0.01; q.ask = price+0.01;
        q.change_percent = std::numeric_limits<double>::quiet_NaN();
        q.market_time_ms = QDateTime::currentMSecsSinceEpoch();
        emit quote_received(q);
    }
}
}
