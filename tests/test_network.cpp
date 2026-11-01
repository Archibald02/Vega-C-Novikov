#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include "json_http.hpp"
#include "market_feed.hpp"
#include "market_history.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <cmath>
using live::HttpResult;
namespace {
struct LocalServer {
    QTcpServer listener;
    QVector<QByteArray> received;
    std::function<void(QTcpSocket*,const QByteArray&)> handler;
    LocalServer() {
        if (!listener.listen(QHostAddress::LocalHost)) throw std::runtime_error("Loopback listen failed");
        QObject::connect(&listener,&QTcpServer::newConnection,&listener,[this] {
            while (auto* socket=listener.nextPendingConnection()) {
                auto bytes=std::make_shared<QByteArray>();auto done=std::make_shared<bool>(false);
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[this,socket,bytes,done] {
                    bytes->append(socket->readAll());
                    if(*done||!bytes->contains("\r\n\r\n"))return;
                    *done=true;received.push_back(*bytes);if(handler)handler(socket,*bytes);
                });
            }
        });
    }
    QUrl url(const QString& path="/") const { return QUrl(QString("http://127.0.0.1:%1%2").arg(listener.serverPort()).arg(path)); }
};
void respond(QTcpSocket* socket,const QByteArray& body="{}",int code=200,const QByteArray& extra={}) {
    socket->write("HTTP/1.1 "+QByteArray::number(code)+" Status\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n"+extra+"\r\n"+body);socket->disconnectFromHost();
}
HttpResult fetch(LocalServer& server,live::HttpLimits limits={1024,500},const QByteArray& body={},QNetworkAccessManager* existing=nullptr) {
    QNetworkAccessManager local;auto& network=existing?*existing:local;QEventLoop loop;std::optional<HttpResult> result;
    auto* reply=live::request_json(network,QNetworkRequest(server.url()),&loop,[&](HttpResult r){result=std::move(r);loop.quit();},body,limits);
    QTimer::singleShot(1500,&loop,[&]{reply->abort();loop.quit();});loop.exec();
    if(!result)throw std::runtime_error("Transport test deadline exceeded");return *result;
}
bool wait_for(const std::function<bool()>& predicate,int timeout=2000) {if(predicate())return true;QEventLoop loop;QTimer timer;QObject::connect(&timer,&QTimer::timeout,&loop,[&]{if(predicate())loop.quit();});timer.start(5);QTimer::singleShot(timeout,&loop,&QEventLoop::quit);loop.exec();return predicate();}
void events(int milliseconds) {QEventLoop loop;QTimer::singleShot(milliseconds,&loop,&QEventLoop::quit);loop.exec();}
QByteArray moex_fixture() {return R"({"marketdata":{"columns":["SECID","LAST","SYSTIME"],"data":[["SBER",100,"2026-10-27 12:00:00"],["GAZP",200,"2026-10-27 12:00:00"]]}})";}
}
TEST_CASE("HTTP JSON success, damaged documents and wrong root") {
    LocalServer server;server.handler=[](auto* s,const auto&){respond(s,R"({"answer":42})");};
    auto r=fetch(server);REQUIRE(r.ok());CHECK(r.document.object().value("answer").toInt()==42);
    for(const QByteArray bad:{QByteArray("<html>gateway</html>"),QByteArray("{\"x\":"),QByteArray("null"),QByteArray()}) {
        server.handler=[bad](auto* s,const auto&){respond(s,bad);};CHECK(fetch(server).error==HttpResult::Error::InvalidJson);
    }
}
TEST_CASE("HTTP 401 403 429 500 and Retry-After do not solicit credentials") {
    LocalServer server;
    for(int code:{401,403,429,500}) {
        server.handler=[code](auto* s,const auto&){respond(s,"{}",code,"Retry-After: 120\r\n");};
        const auto r=fetch(server);CHECK(r.error==HttpResult::Error::Http);CHECK(r.status==code);CHECK(r.retry_after_seconds==120);
    }
    server.handler=[](auto* s,const auto&){respond(s,"{}",429,"Retry-After: 999999\r\n");};CHECK(fetch(server).retry_after_seconds==300);
}
TEST_CASE("Redirects including POST are never followed") {
    LocalServer server;server.handler=[&](auto* s,const auto&){respond(s,"{}",307,"Location: "+server.url("/unexpected-target").toEncoded()+"\r\n");};
    CHECK(fetch(server,{1024,500},R"({"token":"SYNTHETIC_CANARY"})").error==HttpResult::Error::Redirect);
    events(30);CHECK(server.received.size()==1);
}
TEST_CASE("Public requests neither load nor save cookies nor authorization") {
    LocalServer server;QNetworkAccessManager network;auto* jar=network.cookieJar();
    jar->setCookiesFromUrl({QNetworkCookie("personal-session","DO_NOT_SEND")},server.url());
    server.handler=[](auto* s,const auto&){respond(s,"{}",200,"Set-Cookie: tracker=DO_NOT_STORE; Path=/\r\n");};
    REQUIRE(fetch(server,{1024,500},{},&network).ok());REQUIRE(fetch(server,{1024,500},{},&network).ok());
    for(const auto& req:server.received){CHECK_FALSE(req.toLower().contains("cookie:"));CHECK_FALSE(req.toLower().contains("authorization:"));}
    bool saved=false;for(const auto& cookie:jar->cookiesForUrl(server.url()))if(cookie.name()=="tracker")saved=true;CHECK_FALSE(saved);
}
TEST_CASE("Oversize responses rejected with and without Content-Length") {
    LocalServer server;
    server.handler=[](auto* s,const auto&){respond(s,QByteArray(2048,'x'));};CHECK(fetch(server,{64,500}).error==HttpResult::Error::TooLarge);
    server.handler=[](auto* s,const auto&){s->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"+QByteArray(2048,'x'));s->disconnectFromHost();};CHECK(fetch(server,{64,500}).error==HttpResult::Error::TooLarge);
}
TEST_CASE("Total deadline stops silence and a continuously trickling response") {
    LocalServer server;server.handler=[](auto*,const auto&){};CHECK(fetch(server,{1024,80}).error==HttpResult::Error::Timeout);
    server.handler=[](auto* s,const auto&){s->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n");auto* timer=new QTimer(s);QObject::connect(timer,&QTimer::timeout,s,[s]{s->write(" ");});timer->start(10);};
    CHECK(fetch(server,{1024,80}).error==HttpResult::Error::Timeout);
}
TEST_CASE("Connection drop and incomplete HTTP body return controlled errors") {
    LocalServer server;server.handler=[](auto* s,const auto&){s->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{}");s->disconnectFromHost();};CHECK_FALSE(fetch(server).ok());
}
TEST_CASE("Destroying request owner cancels and suppresses late callbacks") {
    LocalServer server;server.handler=[](auto*,const auto&){};QNetworkAccessManager network;bool called=false;auto* owner=new QObject;
    QPointer<QNetworkReply> reply=live::request_json(network,QNetworkRequest(server.url()),owner,[&](auto){called=true;});
    REQUIRE(wait_for([&]{return !server.received.isEmpty();}));delete owner;REQUIRE(wait_for([&]{QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return reply.isNull();}));CHECK_FALSE(called);CHECK(network.findChildren<QNetworkReply*>().isEmpty());
}
TEST_CASE("Restart ignores stale replies and releases cancelled requests") {
    LocalServer server;server.handler=[](auto* s,const QByteArray& request){if(request.contains("GAZP"))respond(s,moex_fixture());};
    live::MarketFeed feed(nullptr,server.url());QStringList seen;
    QObject::connect(&feed,&live::MarketFeed::quote_received,&feed,[&](const live::Quote& q){seen.push_back(q.symbol);});
    live::MarketFeed::Config config;config.symbols={"SBER"};feed.start(config);
    REQUIRE(wait_for([&]{return server.received.size()==1;}));config.symbols={"GAZP"};feed.start(config);REQUIRE(wait_for([&]{return !seen.isEmpty();}));feed.stop();REQUIRE(wait_for([&]{QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return feed.findChildren<QNetworkReply*>().isEmpty();}));
    CHECK(seen==QStringList{"GAZP"});CHECK(feed.findChildren<QNetworkReply*>().isEmpty());
    for(int i=0;i<50;++i){feed.start(config);feed.stop();}REQUIRE(wait_for([&]{QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return feed.findChildren<QNetworkReply*>().isEmpty();}));
}
TEST_CASE("Feed rejects invalid configuration without sending a request") {
    LocalServer server;live::MarketFeed feed(nullptr,server.url());live::MarketFeed::Config config;config.symbols={"https://invalid"};feed.start(config);events(30);CHECK(server.received.isEmpty());
}
TEST_CASE("Candles enforce OHLC invariants chronology types and bounds") {
    const auto input=QJsonDocument::fromJson(R"({"candles":{"columns":["close","volume","begin","low","high","open"],"data":[[101,1000,"2026-10-01 00:00:00",99,102,100],[101,1000,"2026-10-01 00:00:00",99,102,100],[101,1000,"2026-10-02 00:00:00",105,102,100],[101,-1,"2026-10-03 00:00:00",99,102,100],["101",10,"2026-10-04 00:00:00",99,102,100],[102,500,"2026-10-05 00:00:00",100,103,101]]}})");
    const auto bars=live::parse_candles(input);REQUIRE(bars.size()==2);CHECK(bars[0].close==101);CHECK(bars[1].open==101);CHECK(bars[0].time_ms<bars[1].time_ms);
    CHECK(live::parse_candles(QJsonDocument()).isEmpty());
    QJsonArray rows;for(int i=0;i<700;++i)rows.append(QJsonArray{100,100,101,99,10,QDate(2024,1,1).addDays(i).toString("yyyy-MM-dd")+" 00:00:00"});
    const QJsonDocument large(QJsonObject{{"candles",QJsonObject{{"columns",QJsonArray{"open","close","high","low","volume","begin"}},{"data",rows}}}});CHECK(live::parse_candles(large).size()==500);
}

TEST_CASE("Public candles cache is bounded and changing symbol cancels stale work") {
    LocalServer server;
    QByteArray fixture=R"({"candles":{"columns":["begin","open","high","low","close","volume"],"data":[["2026-10-01 00:00:00",100,102,99,101,10]]}})";
    fixture.replace("2026-10-01",QDate::currentDate().addDays(-1).toString(Qt::ISODate).toUtf8());
    server.handler=[fixture](auto* socket,const auto&){QTimer::singleShot(20,socket,[socket,fixture]{respond(socket,fixture);});};
    live::MarketHistory history(nullptr,server.url());QStringList symbols;QVector<bool> caches;int failures=0;
    QObject::connect(&history,&live::MarketHistory::loaded,&history,[&](const QString& symbol,int,const auto& bars,bool cached){CHECK(bars.size()==1);symbols.push_back(symbol);caches.push_back(cached);});
    QObject::connect(&history,&live::MarketHistory::failed,&history,[&](const auto&){++failures;});
    history.load("SBER",0);REQUIRE(wait_for([&]{return server.received.size()==1;}));history.load("GAZP",0);REQUIRE(wait_for([&]{return symbols.size()==1;}));REQUIRE(symbols==QStringList{"GAZP"});CHECK_FALSE(caches.back());
    const int previous=server.received.size();history.load("GAZP",0);CHECK(caches.back());events(10);CHECK(server.received.size()==previous);
    for(int i=0;i<9;++i){const auto n=symbols.size();history.load("T"+QString::number(i),0);REQUIRE(wait_for([&]{return symbols.size()==n+1;}));}
    const int before=server.received.size();history.load("GAZP",0);REQUIRE(wait_for([&]{return !history.busy();}));CHECK(server.received.size()==before+1);CHECK_FALSE(caches.back());CHECK(failures==0);
    history.clear();events(10);CHECK_FALSE(history.busy());
    history.load("bad/../ticker",0);CHECK(failures==1);
}
TEST_CASE("Candles honor source rate limits without opening a login") {
    LocalServer server;server.handler=[](auto* socket,const auto&){respond(socket,"{}",429,"Retry-After: 120\r\n");};
    live::MarketHistory history(nullptr,server.url());int errors=0;QObject::connect(&history,&live::MarketHistory::failed,&history,[&](const auto&){++errors;});
    history.load("SBER",0);REQUIRE(wait_for([&]{return errors==1;}));history.load("GAZP",0,true);events(20);CHECK(errors==2);CHECK(server.received.size()==1);CHECK_FALSE(history.busy());
}

TEST_CASE("History paginates without repeated pages, publishes atomically and caps 5000 bars") {
    LocalServer server;int pages=0;
    server.handler=[&](auto* socket,const QByteArray& request) {
        const auto path=QString::fromLatin1(request.split(' ').value(1));const int start=QUrlQuery(QUrl(path)).queryItemValue("start").toInt();
        ++pages;QJsonArray rows;
        for(int i=start;i<start+500;++i)rows.append(QJsonArray{QDate(2000,1,1).addDays(i).toString("yyyy-MM-dd")+" 00:00:00",100,101,99,100,10});
        respond(socket,QJsonDocument(QJsonObject{{"candles",QJsonObject{{"columns",QJsonArray{"begin","open","high","low","close","volume"}},{"data",rows}}}}).toJson());
    };
    live::MarketHistory history(nullptr,server.url());int total=0,limited=0;
    QObject::connect(&history,&live::MarketHistory::loaded,&history,[&](const QString&,int,const auto& bars,bool){total=bars.size();});
    QObject::connect(&history,&live::MarketHistory::truncated,&history,[&](const QString&){++limited;});
    history.load_range("SBER",QDate(2000,1,1),QDate(2019,12,31),24);
    // Paging is spaced to avoid hammering ISS; ten pages take at least 1.35 s.
    // Scheduling ten spaced requests under sanitizers/contended CI needs slack; all atomicity/page-count checks remain.
    const bool completed=wait_for([&]{return total==5000;},10000);
    INFO("pages=" << pages << ", total=" << total << ", truncated=" << limited);
    REQUIRE(completed);CHECK(pages==10);CHECK(limited==1);CHECK_FALSE(history.busy());
    history.load_range("SBER",QDate(2000,1,1),QDate(2019,12,31),24,true);history.cancel();events(30);CHECK(total==5000);CHECK_FALSE(history.busy());
}
TEST_CASE("A server that ignores pagination cannot loop or publish an incomplete success") {
    LocalServer server;server.handler=[](auto* socket,const auto&) {
        QJsonArray rows;for(int i=0;i<500;++i)rows.append(QJsonArray{QDate(2024,1,1).addDays(i).toString("yyyy-MM-dd")+" 00:00:00",100,101,99,100,10});
        respond(socket,QJsonDocument(QJsonObject{{"candles",QJsonObject{{"columns",QJsonArray{"begin","open","high","low","close","volume"}},{"data",rows}}}}).toJson());
    };
    live::MarketHistory history(nullptr,server.url());int errors=0,success=0;
    QObject::connect(&history,&live::MarketHistory::failed,&history,[&](const auto&){++errors;});QObject::connect(&history,&live::MarketHistory::loaded,&history,[&]{++success;});
    history.load_range("SBER",QDate(2024,1,1),QDate(2026,1,1),24);
    REQUIRE(wait_for([&]{return errors>0;},5000));CHECK(server.received.size()==2);CHECK(success==0);CHECK_FALSE(history.busy());
}
TEST_CASE("Seeded malformed input stress does not produce invalid quotes") {
    std::mt19937 random(431);for(int i=0;i<5000;++i){QByteArray bytes;const int n=random()%1024;bytes.resize(n);for(int j=0;j<n;++j)bytes[j]=static_cast<char>(random()%256);const auto q=live::parse_alor(bytes);CHECK_FALSE(q);CHECK(live::parse_moex(bytes).isEmpty());CHECK(live::parse_catalog(bytes).isEmpty());CHECK(live::parse_candles(QJsonDocument::fromJson(bytes)).isEmpty());}
    CHECK_THROWS(live::parse_symbols(QString(2000,'A')));
    CHECK_FALSE(live::parse_alor(R"({"data":{"symbol":"SBER","last_price":1e300,"last_price_timestamp":1000}})"));
    const auto q=live::parse_alor(R"({"data":{"symbol":"SBER","last_price":100,"last_price_timestamp":1000,"bid":-1,"ask":0}})");REQUIRE(q);CHECK(std::isnan(q->bid));CHECK(std::isnan(q->ask));
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);doctest::Context context;context.applyCommandLine(argc,argv);return context.run();}
namespace {
QByteArray history_page(int start, int count, double close = 100) {
    QJsonArray rows;
    for (int i = start; i < start + count; ++i)
        rows.append(QJsonArray{QDate(2020, 1, 1).addDays(i).toString("yyyy-MM-dd") + " 00:00:00", 100, 102,
                               99, close, 10});
    return QJsonDocument(
               QJsonObject{{"candles", QJsonObject{{"columns", QJsonArray{"begin", "open", "high", "low",
                                                                          "close", "volume"}},
                                                   {"data", rows}}}})
        .toJson();
}
int page_start(const QByteArray &request) {
    return QUrlQuery(QUrl(QString::fromLatin1(request.split(' ').value(1)))).queryItemValue("start").toInt();
}
} // namespace
TEST_CASE("Interrupted history resumes after verifying the last page without publishing partial bars") {
    LocalServer server;
    QVector<int> starts;
    server.handler = [&](auto *socket, const auto &request) {
        const int start = page_start(request);
        starts.push_back(start);
        respond(socket, history_page(start, start == 0 ? 500 : 1));
    };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0, count = 0, received = 0;
    bool pause = true;
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        count = bars.size();
    });
    QObject::connect(&h, &live::MarketHistory::quality_report, &h,
                     [&](const auto &, int n, int) { received = n; });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2025, 1, 1), 24);
    REQUIRE(wait_for([&] { return h.checkpoint().available && !h.busy(); }));
    CHECK(loaded == 0);
    CHECK(h.checkpoint().bars == 500);
    CHECK(h.checkpoint().next_offset == 500);
    REQUIRE(h.resume());
    REQUIRE(wait_for([&] { return loaded == 1; }, 5000));
    CHECK(starts == QVector<int>{0, 0, 500});
    CHECK(count == 501);
    CHECK(received == 501);
    CHECK(h.current_info().resumed);
    CHECK_FALSE(h.checkpoint().available);
}
TEST_CASE("Changed resume boundary restarts the complete period instead of stitching incompatible pages") {
    LocalServer server;
    bool changed = false, pause = true;
    QVector<int> starts;
    server.handler = [&](auto *socket, const auto &request) {
        const int start = page_start(request);
        starts.push_back(start);
        respond(socket, history_page(start, start == 0 ? 500 : 1, changed ? 101 : 100));
    };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0;
    QVector<live::Candle> result;
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        result = bars;
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2025, 1, 1), 24);
    REQUIRE(wait_for([&] { return h.checkpoint().available && !h.busy(); }));
    changed = true;
    REQUIRE(h.resume());
    REQUIRE(wait_for([&] { return loaded == 1; }, 5000));
    CHECK(starts == QVector<int>{0, 0, 0, 500});
    REQUIRE(result.size() == 501);
    for (const auto &b : result)
        CHECK(b.close == 101);
    CHECK_FALSE(h.current_info().resumed);
}
TEST_CASE("History checkpoints expire, clear privately and honor source backoff") {
    LocalServer server;
    qint64 clock = 100000;
    bool pause = true;
    bool limited = false;
    int requests = 0;
    server.handler = [&](auto *socket, const auto &request) {
        ++requests;
        if (limited)
            respond(socket, "{}", 429, "Retry-After: 120\r\n");
        else
            respond(socket, history_page(page_start(request), 500));
    };
    live::MarketHistory h(nullptr, server.url(), [&] { return clock; });
    int errors = 0;
    QObject::connect(&h, &live::MarketHistory::failed, &h, [&] { ++errors; });
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2025, 1, 1), 24);
    REQUIRE(wait_for([&] { return h.checkpoint().available && !h.busy(); }));
    SUBCASE("expired") {
        clock += 1800001;
        CHECK_FALSE(h.checkpoint().available);
        CHECK_FALSE(h.resume());
        CHECK(requests == 1);
    }
    SUBCASE("429 on verification retains checkpoint but prevents immediate requests") {
        limited = true;
        REQUIRE(h.resume());
        REQUIRE(wait_for([&] { return errors == 1; }));
        CHECK(h.checkpoint().available);
        CHECK_FALSE(h.resume());
        CHECK(requests == 2);
        CHECK(errors == 2);
    }
    SUBCASE("explicit discard") {
        h.discard_checkpoint();
        CHECK_FALSE(h.resume());
        CHECK(requests == 1);
    }
    h.clear();
    CHECK(h.datasets().isEmpty());
    CHECK_FALSE(h.checkpoint().available);
    CHECK(h.current_info().sha256.isEmpty());
}
TEST_CASE("History RAM catalog revisions compare content and explicit reuse never accesses network") {
    LocalServer server;
    int mode = 0, requests = 0;
    server.handler = [&](auto *socket, const auto &) {
        ++requests;
        auto object = QJsonDocument::fromJson(history_page(0, 2)).object();
        if (mode == 1) {
            auto block = object["candles"].toObject();
            auto rows = block["data"].toArray();
            auto first = rows[0].toArray();
            first[4] = 101;
            rows[0] = first;
            rows.removeLast();
            rows.append(QJsonArray{"2020-01-03 00:00:00", 100, 102, 99, 100, 10});
            block["data"] = rows;
            object["candles"] = block;
        }
        respond(socket, QJsonDocument(object).toJson());
    };
    qint64 clock = 100000;
    live::MarketHistory h(nullptr, server.url(), [&] { return clock; });
    int loaded = 0;
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    auto load = [&] {
        h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 2, 1), 24, true);
        REQUIRE(wait_for([&] { return !h.busy(); }));
    };
    load();
    const auto first = h.current_info();
    CHECK(first.sha256.size() == 64);
    CHECK_FALSE(first.comparable);
    ++clock;
    load();
    CHECK(h.current_info().revision == first.revision);
    CHECK(h.current_info().sha256 == first.sha256);
    mode = 1;
    ++clock;
    load();
    const auto revised = h.current_info();
    CHECK(revised.revision > first.revision);
    CHECK(revised.added == 1);
    CHECK(revised.changed == 1);
    CHECK(revised.removed == 1);
    CHECK(revised.comparable);
    clock += 600000;
    const int before = requests;
    CHECK(h.restore(revised.key));
    CHECK(requests == before);
    CHECK(loaded == 4);
    CHECK(h.current_info().fetched_ms == revised.fetched_ms);
    CHECK_FALSE(h.restore("unknown"));
    for (int i = 0; i < 9; ++i) {
        ++clock;
        h.load_range(QString("A%1").arg(i), QDate(2020, 1, 1), QDate(2020, 2, 1), 24, true);
        REQUIRE(wait_for([&] { return !h.busy(); }));
    }
    CHECK(h.datasets().size() == 8);
    CHECK_FALSE(h.restore(revised.key));
}
TEST_CASE("Cancelled resume and malformed pages cannot publish stale or partial history") {
    LocalServer server;
    bool pause = true;
    int phase = 0, loaded = 0, errors = 0;
    server.handler = [&](auto *socket, const auto &request) {
        if (phase == 1)
            respond(socket, R"({"candles":{"columns":[],"data":[]}})");
        else
            respond(socket, history_page(page_start(request), 500));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    QObject::connect(&h, &live::MarketHistory::failed, &h, [&] { ++errors; });
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2025, 1, 1), 24);
    REQUIRE(wait_for([&] { return h.checkpoint().available && !h.busy(); }));
    SUBCASE("bad schema") {
        phase = 1;
        REQUIRE(h.resume());
        REQUIRE(wait_for([&] { return errors == 1; }));
        CHECK_FALSE(h.checkpoint().available);
    }
    SUBCASE("cancel from state message") {
        QObject::connect(&h, &live::MarketHistory::state_message, &h, [&] { h.cancel(); });
        CHECK_FALSE(h.resume());
        events(20);
    }
    SUBCASE("clear during resumed request") {
        REQUIRE(h.resume());
        h.clear();
        events(40);
        CHECK(h.datasets().isEmpty());
        CHECK_FALSE(h.checkpoint().available);
    }
    CHECK(loaded == 0);
    CHECK_FALSE(h.busy());
}

TEST_CASE("Cancellation from cached quality callbacks suppresses stale publication") {
    LocalServer server;int requests=0;server.handler=[&](auto *socket,const auto&){++requests;respond(socket,history_page(0,2));};
    live::MarketHistory h(nullptr,server.url());int loaded=0;
    QObject::connect(&h,&live::MarketHistory::loaded,&h,[&]{++loaded;});
    h.load_range("SBER",QDate(2020,1,1),QDate(2020,2,1),24);REQUIRE(wait_for([&]{return loaded==1;}));
    const auto key=h.current_info().key;
    QObject::connect(&h,&live::MarketHistory::quality_report,&h,[&]{h.cancel();});
    h.load_range("SBER",QDate(2020,1,1),QDate(2020,2,1),24);CHECK(loaded==1);CHECK(requests==1);
    h.restore(key);CHECK(loaded==1);CHECK(requests==1);
}
namespace {
QByteArray range_fixture(const QByteArray &request, int total, int changed = -1, int removed = -1) {
    const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
    const auto from = QDate::fromString(q.queryItemValue("from"), Qt::ISODate),
               till = QDate::fromString(q.queryItemValue("till"), Qt::ISODate);
    const QDate origin(2020, 1, 1);
    const int first = std::max(0, static_cast<int>(origin.daysTo(from)));
    const int last = std::min(total - 1, static_cast<int>(origin.daysTo(till)));
    int i = first + q.queryItemValue("start").toInt();
    if (removed >= first && removed <= i)
        ++i;
    QJsonArray rows;
    for (; i <= last && rows.size() < 500; ++i) {
        if (i == removed)
            continue;
        rows.append(QJsonArray{origin.addDays(i).toString("yyyy-MM-dd") + " 00:00:00", 100, 102, 99,
                               i == changed ? 101 : 100, 10});
    }
    return QJsonDocument(
               QJsonObject{{"candles", QJsonObject{{"columns", QJsonArray{"begin", "open", "high", "low",
                                                                          "close", "volume"}},
                                                   {"data", rows}}}})
        .toJson();
}
} // namespace
TEST_CASE("History extension checks overlap, preserves prefix and publishes only the merged target" * doctest::test_suite("history_extension")) {
    LocalServer server;
    int total = 10;
    QVector<QString> froms;
    server.handler = [&](auto *socket, const auto &request) {
        const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        froms.push_back(q.queryItemValue("from"));
        respond(socket, range_fixture(request, total));
    };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0;
    QVector<live::Candle> data;
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        data = bars;
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    total = 15;
    REQUIRE(h.extend(base.key, QDate(2020, 1, 15)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    const auto result = h.current_info();
    CHECK(data.size() == 15);
    CHECK(froms == QVector<QString>{"2020-01-01", "2020-01-09"});
    CHECK(result.extended);
    CHECK(result.reused_bars == 8);
    CHECK(result.checked_from == QDate(2020, 1, 9));
    CHECK(result.from == base.from);
    CHECK(result.till == QDate(2020, 1, 15));
    CHECK(result.base_revision == base.revision);
    CHECK(result.added == 5);
    CHECK(result.changed == 0);
    CHECK(result.removed == 0);
    CHECK(result.comparable);
    CHECK(h.datasets().size() == 2);
    CHECK(h.restore(base.key));
    CHECK(data.size() == 10);
}
TEST_CASE("Changed or deleted overlap causes one complete target reload" * doctest::test_suite("history_extension")) {
    LocalServer server;
    int total = 10, changed = -1, removed = -1;
    QVector<QString> froms;
    server.handler = [&](auto *socket, const auto &request) {
        const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        froms.push_back(q.queryItemValue("from"));
        respond(socket, range_fixture(request, total, changed, removed));
    };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0;
    QVector<live::Candle> data;
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        data = bars;
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto key = h.current_info().key;
    total = 15;
    SUBCASE("changed OHLCV") { changed = 9; }
    SUBCASE("missing boundary candle") { removed = 8; }
    REQUIRE(h.extend(key, QDate(2020, 1, 15)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    CHECK(froms == QVector<QString>{"2020-01-01", "2020-01-09", "2020-01-01"});
    CHECK_FALSE(h.current_info().extended);
    CHECK(data.size() == (removed >= 0 ? 14 : 15));
    if (changed >= 0)
        CHECK(data[9].close == 101);
}
TEST_CASE("Extension checkpoint retains its original dataset when resumed" * doctest::test_suite("history_extension")) {
    LocalServer server;
    int total = 10, loaded = 0, changed = -1;
    bool pause = true;
    QVector<live::Candle> data;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, total, changed));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        data = bars;
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto key = h.current_info().key;
    total = 620;
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    REQUIRE(h.extend(key, QDate(2020, 1, 1).addDays(619)));
    REQUIRE(wait_for([&] { return !h.busy() && h.checkpoint().available; }));
    CHECK(loaded == 1);
    CHECK(h.checkpoint().extending);
    CHECK(h.checkpoint().from == QDate(2020, 1, 1));
    SUBCASE("same guard") {}
    SUBCASE("changed guard requires complete reload") { changed = 400; }
    REQUIRE(h.resume());
    REQUIRE(wait_for([&] { return loaded == 2; }, 5000));
    CHECK(data.size() == 620);
    if (changed < 0) {
        CHECK(h.current_info().extended);
        CHECK(h.current_info().resumed);
        CHECK(h.current_info().reused_bars == 8);
    } else {
        CHECK_FALSE(h.current_info().extended);
        CHECK_FALSE(h.current_info().resumed);
        CHECK(data[400].close == 101);
    }
    CHECK_FALSE(h.checkpoint().available);
}
TEST_CASE("Cancelled or failed extension keeps the previous RAM dataset intact" * doctest::test_suite("history_extension")) {
    LocalServer server;
    bool failed = false;
    int total = 10, loaded = 0, errors = 0;
    server.handler = [&](auto *socket, const auto &request) {
        if (failed)
            respond(socket, "{}", 503);
        else
            respond(socket, range_fixture(request, total));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    QObject::connect(&h, &live::MarketHistory::failed, &h, [&] { ++errors; });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    SUBCASE("HTTP failure") {
        failed = true;
        REQUIRE(h.extend(base.key, QDate(2020, 1, 15)));
        REQUIRE(wait_for([&] { return errors == 1; }));
    }
    SUBCASE("cancel request") {
        total = 15;
        REQUIRE(h.extend(base.key, QDate(2020, 1, 15)));
        h.cancel();
        events(50);
    }
    CHECK(loaded == 1);
    CHECK(h.datasets().size() == 1);
    CHECK(h.datasets()[0].sha256 == base.sha256);
    REQUIRE(h.restore(base.key));
    CHECK(loaded == 2);
    h.clear();
    CHECK(h.datasets().isEmpty());
    CHECK_FALSE(h.checkpoint().available);
}
TEST_CASE("Extension validates date boundaries and H1 identity without silently switching intervals" * doctest::test_suite("history_extension")) {
    LocalServer server;
    int total = 10, loaded = 0;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, total));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int preset, const auto &, bool) {
        CHECK(preset == 2);
        ++loaded;
    });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 60);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto key = h.current_info().key;
    CHECK_FALSE(h.extend("unknown", QDate(2020, 1, 15)));
    CHECK_FALSE(h.extend(key, QDate(2020, 1, 9)));
    CHECK_FALSE(h.extend(key, QDate(2020, 1, 10)));
    CHECK_FALSE(h.extend(key, QDate()));
    CHECK_FALSE(h.extend(key, QDate(2050, 1, 1)));
    total = 11;
    REQUIRE(h.extend(key, QDate(2020, 1, 11)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    CHECK(h.current_info().interval == 60);
}
TEST_CASE("Extension falls back to bounded full load at the combined history limit" * doctest::test_suite("history_extension_limits")) {
    LocalServer server;
    int total = 4900, loaded = 0;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, total));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 1).addDays(4899), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }, 10000));
    const auto key = h.current_info().key;
    total = 5100;
    REQUIRE(h.extend(key, QDate(2020, 1, 1).addDays(5099)));
    REQUIRE(wait_for([&] { return loaded == 2; }, 10000));
    CHECK(h.current_info().bars == 5000);
    CHECK(h.current_info().limited);
    CHECK_FALSE(h.current_info().extended);
    const auto before = server.received.size();
    CHECK_FALSE(h.extend(h.current_info().key, QDate(2020, 1, 1).addDays(5200)));
    CHECK(server.received.size() == before);
}

TEST_CASE("Extending into an existing target retains content revision or advances it without rollback" * doctest::test_suite("history_extension")) {
    LocalServer server;
    int total = 10, loaded = 0;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, total));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    total = 15;
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 15), 24);
    REQUIRE(wait_for([&] { return loaded == 2; }));
    const auto target = h.current_info();
    REQUIRE(h.extend(base.key, QDate(2020, 1, 15)));
    REQUIRE(wait_for([&] { return loaded == 3; }));
    CHECK(h.current_info().revision == target.revision);
    total = 10;
    REQUIRE(h.extend(base.key, QDate(2020, 1, 15)));
    REQUIRE(wait_for([&] { return loaded == 4; }));
    CHECK(h.current_info().revision > target.revision);
    CHECK(h.current_info().bars == 10);
}

TEST_CASE("Earlier history verifies the left overlap and reuses the later suffix" * doctest::test_suite("history_prepend")) {
    LocalServer server;
    QVector<QString> ranges;
    server.handler = [&](auto *socket, const auto &request) {
        const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        ranges.push_back(q.queryItemValue("from") + "/" + q.queryItemValue("till"));
        respond(socket, range_fixture(request, 15));
    };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0;
    QVector<live::Candle> data;
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&](const auto &, int, const auto &bars, bool) {
        ++loaded;
        data = bars;
    });
    h.load_range("SBER", QDate(2020, 1, 6), QDate(2020, 1, 15), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    REQUIRE(h.prepend(base.key, QDate(2020, 1, 1)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    const auto result = h.current_info();
    CHECK(ranges == QVector<QString>{"2020-01-06/2020-01-15", "2020-01-01/2020-01-07"});
    CHECK(data.size() == 15);
    for (int i = 0; i < data.size(); ++i)
        CHECK(data[i].time_ms ==
              QDateTime(QDate(2020, 1, 1).addDays(i), QTime(0, 0), QTimeZone("Europe/Moscow"))
                  .toMSecsSinceEpoch());
    CHECK(result.extended);
    CHECK(result.prepended);
    CHECK(result.reused_bars == 8);
    CHECK(result.from == QDate(2020, 1, 1));
    CHECK(result.till == base.till);
    CHECK(result.checked_from == QDate(2020, 1, 1));
    CHECK(result.checked_till == QDate(2020, 1, 7));
    CHECK(result.added == 5);
    CHECK(result.changed == 0);
    CHECK(result.removed == 0);
    CHECK(result.base_revision == base.revision);
    const auto hash = result.sha256;
    REQUIRE(h.restore(base.key));
    CHECK(data.size() == 10);
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 15), 24, true);
    REQUIRE(wait_for([&] { return loaded == 4; }));
    CHECK(h.current_info().sha256 == hash);
    const auto revision = h.current_info().revision;
    REQUIRE(h.prepend(base.key, QDate(2020, 1, 1)));
    REQUIRE(wait_for([&] { return loaded == 5; }));
    CHECK(h.current_info().revision == revision);
}
TEST_CASE("Changed missing or added left overlap reloads the entire expanded "
          "range once" *
          doctest::test_suite("history_prepend")) {
    LocalServer server;
    int changed = -1, removed = -1, loaded = 0;
    QVector<QString> tills;
    // Missing day in the original base can become an added old candle later.
    bool initial = true, extra = false;
    server.handler = [&](auto *socket, const auto &request) {
        const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        tills.push_back(q.queryItemValue("till"));
        respond(socket, range_fixture(request, 15, changed, initial && extra ? 6 : removed));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    SUBCASE("changed value") { changed = 5; }
    SUBCASE("removed old row") { removed = 6; }
    SUBCASE("extra old row") { extra = true; }
    const auto planned_changed = changed, planned_removed = removed;
    changed = removed = -1;
    h.load_range("SBER", QDate(2020, 1, 6), QDate(2020, 1, 15), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    initial = false;
    changed = planned_changed;
    removed = planned_removed;
    const auto base = h.current_info();
    REQUIRE(h.prepend(base.key, QDate(2020, 1, 1)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    CHECK(tills == QVector<QString>{"2020-01-15", "2020-01-07", "2020-01-15"});
    CHECK_FALSE(h.current_info().extended);
    CHECK(h.current_info().from == QDate(2020, 1, 1));
    CHECK(h.current_info().bars == (removed >= 0 ? 14 : 15));
    REQUIRE(h.restore(base.key));
    CHECK(h.current_info().sha256 == base.sha256);
}
TEST_CASE("Earlier extension resumes the complete target or restarts it after "
          "a changed guard" *
          doctest::test_suite("history_prepend")) {
    LocalServer server;
    int loaded = 0, changed = -1;
    bool pause = true;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, 630, changed));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    const QDate origin(2020, 1, 1);
    h.load_range("SBER", origin.addDays(610), origin.addDays(629), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    QObject::connect(&h, &live::MarketHistory::progress, &h, [&](const auto &, int n, int) {
        if (pause && n == 500) {
            pause = false;
            h.cancel();
        }
    });
    REQUIRE(h.prepend(base.key, origin));
    REQUIRE(wait_for([&] { return !h.busy() && h.checkpoint().available; }));
    CHECK(loaded == 1);
    CHECK(h.checkpoint().from == origin);
    CHECK(h.checkpoint().till == origin.addDays(629));
    CHECK(h.checkpoint().extending);
    SUBCASE("unchanged guard") {}
    SUBCASE("changed guard") { changed = 400; }
    REQUIRE(h.resume());
    REQUIRE(wait_for([&] { return loaded == 2; }, 5000));
    CHECK(h.current_info().bars == 630);
    CHECK(h.current_info().till == origin.addDays(629));
    CHECK(h.current_info().extended == (changed < 0));
    if (changed < 0) {
        CHECK(h.current_info().prepended);
        CHECK(h.current_info().resumed);
        CHECK(h.current_info().reused_bars == 18);
    }
    CHECK_FALSE(h.checkpoint().available);
    h.clear();
    CHECK(h.datasets().isEmpty());
    CHECK_FALSE(h.checkpoint().available);
}
TEST_CASE("Earlier extension failures cancellation and invalid dates never "
          "replace the base" *
          doctest::test_suite("history_prepend")) {
    LocalServer server;
    int loaded = 0, errors = 0;
    bool fail = false;
    server.handler = [&](auto *socket, const auto &request) {
        if (fail)
            respond(socket, "{}", 503);
        else
            respond(socket, range_fixture(request, 15));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    QObject::connect(&h, &live::MarketHistory::failed, &h, [&] { ++errors; });
    h.load_range("SBER", QDate(2020, 1, 6), QDate(2020, 1, 15), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    const auto count = server.received.size();
    CHECK_FALSE(h.prepend(base.key, QDate()));
    CHECK_FALSE(h.prepend("unknown", QDate(2020, 1, 1)));
    CHECK_FALSE(h.prepend(base.key, QDate(2020, 1, 6)));
    CHECK_FALSE(h.prepend(base.key, QDate(2020, 1, 7)));
    CHECK_FALSE(h.prepend(base.key, QDate(1989, 1, 1)));
    CHECK_FALSE(h.prepend(base.key, QDate(1990, 1, 1)));
    CHECK(server.received.size() == count);
    errors = 0;
    SUBCASE("503") {
        fail = true;
        REQUIRE(h.prepend(base.key, QDate(2020, 1, 1)));
        REQUIRE(wait_for([&] { return errors == 1; }));
    }
    SUBCASE("cancel") {
        REQUIRE(h.prepend(base.key, QDate(2020, 1, 1)));
        h.cancel();
        events(40);
    }
    SUBCASE("cancel from state callback") {
        QObject::connect(&h, &live::MarketHistory::state_message, &h, [&] { h.cancel(); });
        CHECK_FALSE(h.prepend(base.key, QDate(2020, 1, 1)));
        events(40);
    }
    CHECK(loaded == 1);
    CHECK_FALSE(h.busy());
    CHECK(h.datasets().size() == 1);
    CHECK(h.datasets()[0].sha256 == base.sha256);
}
TEST_CASE("Earlier H1 extension verifies every intraday candle and handles a "
          "tiny base" *
          doctest::test_suite("history_prepend")) {
    LocalServer server;
    int loaded = 0;
    bool changed_hour = false;
    QVector<live::Candle> data;
    server.handler = [&](auto *socket, const auto &request) {
        const QUrlQuery q(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        CHECK(q.queryItemValue("interval") == "60");
        auto doc = QJsonDocument::fromJson(range_fixture(request, 15));
        auto block = doc.object()["candles"].toObject();
        QJsonArray rows;
        for (const auto &value : block["data"].toArray()) {
            auto row = value.toArray();
            rows.append(row);
            row[0] = row[0].toString().left(10) + " 23:00:00";
            if (changed_hour && row[0].toString() == "2020-01-15 23:00:00")
                row[4] = 101;
            rows.append(row);
        }
        block["data"] = rows;
        respond(socket, QJsonDocument(QJsonObject{{"candles", block}}).toJson());
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h,
                     [&](const auto &, int preset, const auto &bars, bool) {
                         CHECK(preset == 2);
                         ++loaded;
                         data = bars;
                     });
    h.load_range("SBER", QDate(2020, 1, 15), QDate(2020, 1, 15), 60);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    SUBCASE("unchanged intraday overlap") {}
    SUBCASE("only the later hour changes") { changed_hour = true; }
    REQUIRE(h.prepend(h.current_info().key, QDate(2020, 1, 1)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    CHECK(data.size() == 30);
    CHECK(h.current_info().extended == !changed_hour);
    if (changed_hour)
        CHECK(data.back().close == 101);
    CHECK(h.current_info().interval == 60);
    CHECK(h.current_info().reused_bars == 0);
    for (int i = 1; i < data.size(); ++i)
        CHECK(data[i].time_ms > data[i - 1].time_ms);
}
TEST_CASE("Earlier extension above 5000 falls back to an explicitly limited "
          "full range" *
          doctest::test_suite("history_prepend_limits")) {
    LocalServer server;
    int loaded = 0;
    server.handler = [&](auto *socket, const auto &request) {
        respond(socket, range_fixture(request, 5100));
    };
    live::MarketHistory h(nullptr, server.url());
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    const QDate origin(2020, 1, 1);
    h.load_range("SBER", origin.addDays(200), origin.addDays(5099), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }, 10000));
    const auto base = h.current_info();
    REQUIRE(h.prepend(base.key, origin));
    REQUIRE(wait_for([&] { return loaded == 2; }, 10000));
    CHECK(h.current_info().bars == 5000);
    CHECK(h.current_info().limited);
    CHECK_FALSE(h.current_info().extended);
    CHECK(h.current_info().from == origin);
    CHECK(h.current_info().till == origin.addDays(5099));
    CHECK_FALSE(h.prepend(h.current_info().key, origin.addDays(-1)));
    REQUIRE(h.restore(base.key));
    CHECK(h.current_info().sha256 == base.sha256);
}
TEST_CASE("An earlier empty date range expands identity without fabricating candles" *
          doctest::test_suite("history_prepend")) {
    LocalServer server;
    server.handler = [&](auto *socket, const auto &request) { respond(socket, range_fixture(request, 10)); };
    live::MarketHistory h(nullptr, server.url());
    int loaded = 0;
    QObject::connect(&h, &live::MarketHistory::loaded, &h, [&] { ++loaded; });
    h.load_range("SBER", QDate(2020, 1, 1), QDate(2020, 1, 10), 24);
    REQUIRE(wait_for([&] { return loaded == 1; }));
    const auto base = h.current_info();
    REQUIRE(h.prepend(base.key, QDate(2019, 12, 1)));
    REQUIRE(wait_for([&] { return loaded == 2; }));
    CHECK(h.current_info().from == QDate(2019, 12, 1));
    CHECK(h.current_info().till == base.till);
    CHECK(h.current_info().bars == 10);
    CHECK(h.current_info().sha256 == base.sha256);
    CHECK(h.current_info().added == 0);
    CHECK(h.current_info().changed == 0);
    CHECK(h.current_info().removed == 0);
    CHECK(h.current_info().prepended);
    CHECK(h.current_info().reused_bars == 8);
}
