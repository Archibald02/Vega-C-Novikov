#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <functional>
namespace live {
/** Bounded, deadline-limited JSON transport. No cookies, redirects, disk cache or auth reuse. */
struct HttpResult {
    enum class Error { None, Network, Http, Timeout, TooLarge, InvalidJson, Redirect, Cancelled };
    Error error = Error::None;
    int status = 0, retry_after_seconds = 0;
    QJsonDocument document;
    bool ok() const { return error == Error::None; }
};
struct HttpLimits { qint64 bytes = 1024 * 1024; int deadline_ms = 15000; };
/** Context destruction cancels the request and suppresses its callback. Empty body means GET. */
QNetworkReply* request_json(QNetworkAccessManager& network, QNetworkRequest request,
    QObject* context, std::function<void(HttpResult)> completed,
    const QByteArray& body = {}, HttpLimits limits = {});
QString http_error_text(const HttpResult& result);
}
