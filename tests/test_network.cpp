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
