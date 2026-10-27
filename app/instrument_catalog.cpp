#include "instrument_catalog.hpp"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <memory>
namespace live {
void InstrumentCatalog::load(bool force) {
    if(pending_) return;
    if(!force && !items_.isEmpty() && fetched_.secsTo(QDateTime::currentDateTimeUtc())<900) { emit updated(); return; }
    pending_=true;
    QNetworkRequest request{QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities.json?iss.meta=off&iss.only=securities&securities.columns=SECID,SHORTNAME,LOTSIZE,STATUS")};
    request.setTransferTimeout(15000);
    auto* reply=network_.get(request);
    auto bytes=std::make_shared<QByteArray>();
    constexpr int limit=1024*1024;
    connect(reply,&QNetworkReply::readyRead,this,[reply,bytes] {
        if(bytes->size()+reply->bytesAvailable()>limit) {reply->abort();return;}
        bytes->append(reply->readAll());
    });
    connect(reply,&QNetworkReply::finished,this,[this,reply,bytes] {
        reply->deleteLater();pending_=false;
        if(reply->error()!=QNetworkReply::NoError || bytes->size()+reply->bytesAvailable()>limit) {
            emit failed(QStringLiteral("Не удалось загрузить каталог. Проверьте интернет; повторите обновление.")); return;
        }
        bytes->append(reply->readAll()); const auto items=parse_catalog(*bytes);
        if(items.isEmpty()) {emit failed(QStringLiteral("В ответе нет действующих инструментов TQBR."));return;}
        items_=items;fetched_=QDateTime::currentDateTimeUtc();emit updated();
    });
}
int InstrumentCatalog::lot_size(const QString& symbol) const {
    for(const auto& item:items_) if(item.symbol==symbol) return item.lot_size;
    return 0;
}
}
