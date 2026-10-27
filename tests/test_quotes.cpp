#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "quote_parser.hpp"
#include <cmath>
#include <stdexcept>
TEST_CASE("ALOR Simple quote and optional fields") {
    auto q=live::parse_alor(R"({"data":{"symbol":"SBER","last_price":272,"last_price_timestamp":1703844426,"bid":271.99,"ask":272.01,"change_percent":0.1},"guid":"test"})");
    REQUIRE(q.has_value()); CHECK(q->symbol=="SBER"); CHECK(q->last==272);
    CHECK(q->market_time_ms==1703844426000LL); CHECK(q->bid==doctest::Approx(271.99));
    CHECK(q->change_percent==doctest::Approx(0.1));
    q=live::parse_alor(R"({"data":{"symbol":"GAZP","last_price":123.45,"last_price_timestamp":1703844426}})");
    REQUIRE(q.has_value()); CHECK(std::isnan(q->ask)); CHECK(std::isnan(q->change_percent));
}
TEST_CASE("ALOR control messages and invalid payloads are not quotes") {
    CHECK_FALSE(live::parse_alor(R"({"httpCode":200,"requestGuid":"abc"})"));
    CHECK_FALSE(live::parse_alor(R"({"httpCode":401,"message":"Invalid JWT"})"));
    CHECK_FALSE(live::parse_alor("not JSON"));
    CHECK_FALSE(live::parse_alor(R"({"data":{"symbol":"SBER","last_price":null,"last_price_timestamp":1000}})"));
    CHECK_FALSE(live::parse_alor(R"({"data":{"symbol":"SBER","last_price":-1,"last_price_timestamp":1000}})"));
    CHECK_FALSE(live::parse_alor(R"({"data":{"symbol":"SBER","last_price":"272","last_price_timestamp":1000}})"));
    CHECK_FALSE(live::parse_alor(R"({"data":{"symbol":"SBER","last_price":272}})"));
}
TEST_CASE("MOEX columns can arrive in any order and optional data can be null") {
    const auto quotes=live::parse_moex(R"({"marketdata":{"columns":["LAST","SYSTIME","SECID","BID","OFFER","LASTTOPREVPRICE"],"data":[[296.35,"2026-10-27 12:11:05","SBER",296.34,296.35,0.12],[123,"2026-10-27 12:11:05","GAZP",null,null,null]]}})");
    REQUIRE(quotes.size()==2);
    CHECK(quotes[0].symbol=="SBER"); CHECK(quotes[0].last==doctest::Approx(296.35));
    CHECK(quotes[0].market_time_ms==1793092265000LL);
    CHECK(quotes[0].change_percent==doctest::Approx(0.12));
    CHECK(std::isnan(quotes[1].bid)); CHECK(std::isnan(quotes[1].change_percent));
}
TEST_CASE("MOEX rejects missing columns, malformed dates and invalid prices") {
    CHECK(live::parse_moex("not JSON").isEmpty());
    CHECK(live::parse_moex(R"({"marketdata":{"columns":["SECID","LAST"],"data":[["SBER",100]]}})").isEmpty());
    CHECK(live::parse_moex(R"({"marketdata":{"columns":["SECID","LAST","SYSTIME"],"data":[["SBER",null,"2026-10-27 12:00:00"],["GAZP",100,"invalid"]]}})").isEmpty());
}
TEST_CASE("Ticker list is normalized, deduplicated and bounded") {
    CHECK(live::parse_symbols(" sber, gazp; SBER\nLKOH ")==QStringList{"SBER","GAZP","LKOH"});
    CHECK_THROWS_AS(live::parse_symbols(""),std::invalid_argument);
    CHECK_THROWS_AS(live::parse_symbols("SBER https://bad"),std::invalid_argument);
    QString too_many; for(int i=0;i<21;++i) too_many+=" A"+QString::number(i);
    CHECK_THROWS_AS(live::parse_symbols(too_many),std::invalid_argument);
}
