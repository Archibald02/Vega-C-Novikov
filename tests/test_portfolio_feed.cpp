#include "portfolio_live_feed.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include <QUrlQuery>
#include <doctest/doctest.h>
namespace {
const QTimeZone zone("Europe/Moscow");
qint64 epoch() { return QDateTime(QDate(2023, 11, 15), QTime(10, 0), zone).toMSecsSinceEpoch(); }
quant::Bar candle(int i) {
    return {epoch() + i * 3600000LL, epoch() + (i + 1) * 3600000LL, 100, 100, 100, 100, 10000, 10800};
}
std::unique_ptr<quant::PortfolioStreamBuffer> seed() {
    std::vector<quant::PortfolioAsset> a(2);
    a[0].symbol = "A";
    a[1].symbol = "B";
    for (auto &v : a)
        for (int i = 0; i < 40; ++i)
            v.bars.push_back(candle(i));
    return std::make_unique<quant::PortfolioStreamBuffer>(a, candle(39).time, candle(39).end, epoch(), 60);
}
QByteArray payload(QDate from, QDate till, bool revision, bool malformed) {
    QJsonArray rows;
    for (int i = 0; i < 43; ++i) {
        auto t = QDateTime::fromMSecsSinceEpoch(candle(i).time, zone);
        if (t.date() < from || t.date() > till)
            continue;
        const double p = revision && i == 39 ? 101 : 100;
        rows.append(
            QJsonArray{t.toString("yyyy-MM-dd HH:mm:ss"), p, p, p, p, malformed && i == 40 ? -1. : 10000.});
    }
    return QJsonDocument(
               QJsonObject{{"candles", QJsonObject{{"columns", QJsonArray{"begin", "open", "high", "low",
                                                                          "close", "volume"}},
                                                   {"data", rows}}}})
        .toJson(QJsonDocument::Compact);
}
struct Server {
    QTcpServer listener;
    int requests = 0;
    bool fail_b = false, revise_b = false, malformed_b = false, hold = false;
    QVector<QByteArray> headers;
    Server() {
        if (!listener.listen(QHostAddress::LocalHost))
            throw std::runtime_error("fixture");
        QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
            while (auto *s = listener.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                auto replied = std::make_shared<bool>(false);
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QTcpSocket::readyRead, s, [=] {
                    bytes->append(s->readAll());
                    if (*replied || !bytes->contains("\r\n\r\n"))
                        return;
                    *replied = true;
                    ++requests;
                    headers.push_back(*bytes);
                    if (hold)
                        return;
                    bool b = bytes->contains("/B/");
                    QUrlQuery q(QUrl::fromEncoded(bytes->split(' ').value(1)));
                    auto body = fail_b && b
                                    ? QByteArray("{}")
                                    : payload(QDate::fromString(q.queryItemValue("from"), Qt::ISODate),
                                              QDate::fromString(q.queryItemValue("till"), Qt::ISODate),
                                              revise_b && b, malformed_b && b);
                    s->write(QByteArray("HTTP/1.1 ") + (fail_b && b ? "503 Unavailable" : "200 OK") +
                             "\r\nContent-Type: application/json\r\nContent-Length: " +
                             QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/").arg(listener.serverPort())); }
};
bool wait_for(const std::function<bool()> &p) {
    QElapsedTimer t;
    t.start();
    while (!p() && t.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return p();
}
void events() {
    QEventLoop l;
    QTimer::singleShot(35, &l, &QEventLoop::quit);
    l.exec();
}
qint64 now() { return candle(41).end + 20 * 60000; }
} // namespace
TEST_CASE("Portfolio feed complete cycle, closed cutoff, pause cooldown and request privacy") {
    Server s;
    live::PortfolioLiveFeed f(nullptr, s.url(), now);
    int published = 0, fatal = 0, status = 0;
    std::vector<std::vector<quant::Bar>> data;
    f.on_frames = [&](const auto &v) {
        ++published;
        data = v;
    };
    f.on_fatal = [&](const auto &) { ++fatal; };
    f.on_status = [&](const auto &) { ++status; };
    f.start(seed(), 60);
    REQUIRE(wait_for([&] { return published || fatal; }));
    CHECK(published == 1);
    CHECK(fatal == 0);
    CHECK(status == 1);
    REQUIRE(data.size() == 2);
    CHECK(data.front()[0].time == candle(40).time);
    CHECK(data.back()[0].end == candle(41).end);
    CHECK(s.requests == 2);
    f.refresh();
    f.pause(true);
    CHECK(f.paused());
    f.pause(false);
    events();
    CHECK(s.requests == 2);
    for (const auto &h : s.headers) {
        CHECK_FALSE(h.toLower().contains("authorization:"));
        CHECK_FALSE(h.toLower().contains("cookie:"));
        CHECK_FALSE(h.toLower().contains("token"));
    }
    f.stop();
}
TEST_CASE("Portfolio feed second-symbol HTTP failure never commits partial data") {
    Server s;
    s.fail_b = true;
    live::PortfolioLiveFeed f(nullptr, s.url(), now);
    int published = 0, status = 0;
    f.on_frames = [&](const auto &) { ++published; };
    f.on_status = [&](const auto &) { ++status; };
    f.start(seed(), 60);
    REQUIRE(wait_for([&] { return status; }));
    CHECK(published == 0);
    CHECK(s.requests == 2);
    CHECK(f.active());
    f.refresh();
    events();
    CHECK(s.requests == 2);
    f.stop();
}
TEST_CASE("Portfolio feed rejects revised or malformed closed rows") {
    Server s;
    SUBCASE("revision") { s.revise_b = true; }
    SUBCASE("rejected") { s.malformed_b = true; }
    live::PortfolioLiveFeed f(nullptr, s.url(), now);
    int published = 0, fatal = 0;
    f.on_frames = [&](const auto &) { ++published; };
    f.on_fatal = [&](const auto &) { ++fatal; };
    f.start(seed(), 60);
    REQUIRE(wait_for([&] { return fatal; }));
    CHECK(published == 0);
    CHECK_FALSE(f.active());
    events();
    CHECK(fatal == 1);
}
TEST_CASE("Portfolio feed cancellation and stale source suppress callbacks and requests") {
    Server s;
    s.hold = true;
    live::PortfolioLiveFeed f(nullptr, s.url(), now);
    int callbacks = 0;
    f.on_frames = [&](const auto &) { ++callbacks; };
    f.on_fatal = [&](const auto &) { ++callbacks; };
    f.on_status = [&](const auto &) { ++callbacks; };
    f.start(seed(), 60);
    REQUIRE(wait_for([&] { return s.requests; }));
    f.stop();
    events();
    CHECK(callbacks == 0);
    CHECK(s.requests == 1);
    live::PortfolioLiveFeed stale(nullptr, s.url(), [] { return now() + 8 * 86400000LL; });
    int fatal = 0;
    stale.on_fatal = [&](const auto &) { ++fatal; };
    stale.start(seed(), 60);
    CHECK(fatal == 1);
    events();
    CHECK(s.requests == 1);
}
