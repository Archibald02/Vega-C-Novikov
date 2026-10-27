#include "instrument_catalog.hpp"
#include "json_http.hpp"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <memory>
namespace live {
void InstrumentCatalog::load(bool force) {
    if(pending_) return;
    if(!force && !items_.isEmpty() && fetched_.secsTo(QDateTime::currentDateTimeUtc())<900) { emit updated(); return; }
    pending_=true;
    QNetworkRequest request{QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities.json?iss.meta=off&iss.only=securities&securities.columns=SECID,SHORTNAME,LOTSIZE,STATUS")};
    request_json(network_, request, this, [this](const HttpResult& result) {
        pending_ = false;
        if (!result.ok()) { emit failed(http_error_text(result)); return; }
        const auto items = parse_catalog(result.document.toJson(QJsonDocument::Compact));
        if (items.isEmpty()) { emit failed(QStringLiteral("В ответе нет действующих инструментов TQBR.")); return; }
        items_ = items; fetched_ = QDateTime::currentDateTimeUtc(); emit updated();
    });
}
int InstrumentCatalog::lot_size(const QString& symbol) const {
    for(const auto& item:items_) if(item.symbol==symbol) return item.lot_size;
    return 0;
}
}
