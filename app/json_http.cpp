#include "json_http.hpp"
#include <QDateTime>
#include <QJsonParseError>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <memory>
namespace live {
QNetworkReply* request_json(QNetworkAccessManager& network, QNetworkRequest request,
    QObject* context, std::function<void(HttpResult)> completed,
    const QByteArray& body, HttpLimits limits) {
    limits.bytes = std::clamp<qint64>(limits.bytes, 32, 2 * 1024 * 1024);
    limits.deadline_ms = std::clamp(limits.deadline_ms, 20, 30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "QuantPriceLive/4 educational");
    request.setTransferTimeout(0); // One explicit absolute deadline owns timeout classification.
    if (!body.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = body.isEmpty() ? network.get(request) : network.post(request, body);
    reply->setReadBufferSize(limits.bytes + 1);
    struct State { QByteArray bytes; HttpResult::Error error = HttpResult::Error::None; };
    auto state = std::make_shared<State>();
    auto consume = [reply, state, limits] {
        if (state->bytes.size() + reply->bytesAvailable() > limits.bytes) {
            state->error = HttpResult::Error::TooLarge;
            reply->abort();
            return;
        }
        state->bytes.append(reply->readAll());
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, state, limits] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limits.bytes) {
            state->error = HttpResult::Error::TooLarge;
            reply->abort();
        }
    });
    QObject::connect(reply, &QIODevice::readyRead, reply, consume);
    // An absolute deadline also stops a peer that sends one byte forever.
    QTimer::singleShot(limits.deadline_ms, reply, [reply, state] {
        if (!reply->isFinished()) { state->error = HttpResult::Error::Timeout; reply->abort(); }
    });
    QPointer<QObject> owner(context);
    const auto cancelled = QObject::connect(context, &QObject::destroyed, reply, [reply] { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, reply,
        [reply, state, consume, owner, completed = std::move(completed), cancelled] {
        QObject::disconnect(cancelled);
        HttpResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool numeric = false;
        int retry = reply->rawHeader("Retry-After").toInt(&numeric);
        if (!numeric) {
            const auto date = QDateTime::fromString(QString::fromLatin1(reply->rawHeader("Retry-After")), Qt::RFC2822Date);
            retry = date.isValid() ? static_cast<int>(QDateTime::currentDateTimeUtc().secsTo(date)) : 0;
        }
        result.retry_after_seconds = std::clamp(retry, 0, 300);
        result.error = state->error;
        if (result.error == HttpResult::Error::None) {
            if (result.status >= 300 && result.status < 400) result.error = HttpResult::Error::Redirect;
            else if (result.status && (result.status < 200 || result.status >= 300)) result.error = HttpResult::Error::Http;
            else if (reply->error() == QNetworkReply::OperationCanceledError) result.error = HttpResult::Error::Cancelled;
            else if (reply->error() != QNetworkReply::NoError) result.error = HttpResult::Error::Network;
            else {
                consume(); result.error = state->error;
                if (result.error == HttpResult::Error::None) {
                    QJsonParseError parse;
                    result.document = QJsonDocument::fromJson(state->bytes, &parse);
                    if (parse.error != QJsonParseError::NoError || result.document.isNull()) result.error = HttpResult::Error::InvalidJson;
                }
            }
        }
        reply->deleteLater();
        if (owner) completed(std::move(result));
    });
    return reply;
}
QString http_error_text(const HttpResult& r) {
    if (r.status == 429) return QStringLiteral("Лимит запросов источника; повтор будет позже.");
    if (r.status == 401 || r.status == 403) return QStringLiteral("Источник ограничил публичный доступ. Вводить пароль не нужно.");
    switch (r.error) {
    case HttpResult::Error::Timeout: return QStringLiteral("Источник не ответил вовремя. Можно повторить запрос.");
    case HttpResult::Error::TooLarge: return QStringLiteral("Ответ превышает безопасный размер и отклонён.");
    case HttpResult::Error::InvalidJson: return QStringLiteral("Источник вернул некорректный JSON.");
    case HttpResult::Error::Redirect: return QStringLiteral("Перенаправление отклонено. Проверьте доступность официального API.");
    case HttpResult::Error::Cancelled: return QStringLiteral("Запрос отменён.");
    default: return QStringLiteral("Ошибка сети или источника. Проверьте интернет и повторите позже.");
    }
}
}
