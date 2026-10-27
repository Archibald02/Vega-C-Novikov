#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>
/** @file quote_parser.hpp @brief Validated provider payloads. */
namespace live {
/** A market quote. NaN represents an unavailable optional numeric field. */
struct Quote {
    QString symbol;
    double last = 0;
    double bid = 0, ask = 0, change_percent = 0;
    qint64 market_time_ms = 0;
};
/** Parse an ALOR Simple quote; nonquote control messages return nullopt. */
std::optional<Quote> parse_alor(const QByteArray& payload);
/** Parse MOEX ISS columns by name; malformed rows are ignored. */
QVector<Quote> parse_moex(const QByteArray& payload);
/** Trim, uppercase and validate up to 20 distinct TQBR-style symbols. */
QStringList parse_symbols(const QString& input);
}
