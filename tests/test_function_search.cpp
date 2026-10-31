#include "function_search.hpp"
#include <doctest/doctest.h>
#include <set>
TEST_CASE("Function search handles Russian, abbreviations and multiple words locally") {
    const auto &entries = live::function_destinations();
    for (const auto &query :
         {QStringLiteral("  КоРРЕЛЯЦИИ  "), QStringLiteral("ATR"), QStringLiteral("матрица доходностей"),
          QStringLiteral("ADF KPSS"), QStringLiteral("Walk-Forward"), QStringLiteral("проекты эксперименты"),
          QStringLiteral("скобки"), QStringLiteral("вложенные"), QStringLiteral("частичный выход")}) {
        const auto found = live::find_functions(query);
        REQUIRE(found.size() == 1);
        CHECK(entries[found.front()].workspace == 7);
    }
    CHECK(live::find_functions(QStringLiteral("капитал портфель")).size() == 1);
    CHECK(live::find_functions(QStringLiteral("\t  \n")).size() == entries.size());
    CHECK(live::find_functions(QStringLiteral("[.*<html>INVALID_987654")).isEmpty());
    CHECK(live::find_functions(QString(100000, QChar('x'))).isEmpty());
}
TEST_CASE("Every searchable function has a bounded unique navigation destination") {
    const auto &entries = live::function_destinations();
    REQUIRE(entries.size() >= 20);
    CHECK(entries.size() <= 100);
    std::set<QString> titles;
    for (const auto &entry : entries) {
        CHECK(titles.insert(entry.title).second);
        CHECK_FALSE(entry.detail.isEmpty());
        CHECK(entry.workspace >= 0);
        CHECK(entry.workspace <= 10);
        CHECK(entry.research_page >= -1);
        CHECK(entry.research_page <= 8);
        if (entry.research_page >= 0)
            CHECK(entry.workspace == 7);
    }
}
