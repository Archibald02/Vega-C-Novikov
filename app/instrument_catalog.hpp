#pragma once
#include "quote_parser.hpp"
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
namespace live {
/** In-memory MOEX board catalog, bounded response and 15-minute cache. */
class InstrumentCatalog : public QObject {
    Q_OBJECT
public:
    explicit InstrumentCatalog(QObject* parent=nullptr) : QObject(parent) {}
    void load(bool force=false);
    const QVector<Instrument>& instruments() const {return items_;}
    int lot_size(const QString& symbol) const;
signals:
    void updated();
    void failed(const QString& reason);
private:
    QNetworkAccessManager network_;
    QVector<Instrument> items_;
    QDateTime fetched_;
    bool pending_=false;
};
}
