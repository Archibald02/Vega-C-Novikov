#include "market_history.hpp"
#include "quote_parser.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
namespace live {
namespace {
QByteArray page_hash(const QJsonObject &block) {
    return QCryptographicHash::hash(QJsonDocument(block).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}
bool same(const Candle &a, const Candle &b) {
    return a.time_ms == b.time_ms && a.open == b.open && a.high == b.high && a.low == b.low &&
           a.close == b.close && a.volume == b.volume;
}
QString content_hash(const QVector<Candle> &bars) {
    QJsonArray rows;
    for (const auto &b : bars)
        rows.append(QJsonArray{QString::number(b.time_ms), b.open, b.high, b.low, b.close, b.volume});
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(rows).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
bool valid_block(const QJsonObject &block) {
    if (!block.value("data").isArray() || !block.value("columns").isArray() ||
        block.value("data").toArray().size() > 500)
        return false;
    QSet<QString> columns;
    for (const auto &v : block.value("columns").toArray()) {
        if (!v.isString() || columns.contains(v.toString()))
            return false;
        columns.insert(v.toString());
    }
    for (const auto *name : {"begin", "open", "high", "low", "close", "volume"})
        if (!columns.contains(name))
            return false;
    return true;
}
} // namespace
QVector<Candle> parse_candles(const QJsonDocument &document) {
    QVector<Candle> bars;
    const auto block = document.object().value("candles").toObject();
    const auto cols = block.value("columns").toArray();
    const QStringList names{"begin", "open", "high", "low", "close", "volume"};
    QVector<int> positions;
    for (const auto &name : names) {
        int found = -1;
        for (int i = 0; i < cols.size(); ++i)
            if (cols[i].toString() == name) {
                found = i;
                break;
            }
        if (found < 0)
            return {};
        positions.push_back(found);
    }
    for (const auto &value : block.value("data").toArray()) {
        const auto row = value.toArray();
        auto at = [&](int index) {
            return positions[index] < row.size() ? row[positions[index]] : QJsonValue();
        };
        Candle bar;
        const auto date = QDateTime::fromString(at(0).toString(), "yyyy-MM-dd HH:mm:ss");
        if (!date.isValid() || date.date().year() < 1990 || date.date().year() > 3000)
            continue;
        bar.time_ms = QDateTime(date.date(), date.time(), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
        double *fields[]{&bar.open, &bar.high, &bar.low, &bar.close, &bar.volume};
        bool valid = true;
        for (int i = 0; i < 5; ++i) {
            const auto v = at(i + 1);
            const double n = v.toDouble(NAN);
            if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > (i == 4 ? 1e18 : 1e12) ||
                (i != 4 && n == 0))
                valid = false;
            *fields[i] = n;
        }
        if (!valid || bar.high < std::max(bar.open, bar.close) || bar.low > std::min(bar.open, bar.close) ||
            bar.low > bar.high)
            continue;
        if (!bars.isEmpty() && bar.time_ms <= bars.back().time_ms)
            continue;
        bars.push_back(bar);
        if (bars.size() >= 500)
            break;
    }
    return bars;
}
void MarketHistory::cancel(bool preserve_checkpoint) {
    if (!preserve_checkpoint)
        checkpoint_ = {};
    ++generation_;
    active_ = false;
    extension_.reset();
    accumulated_.clear();
    verifying_checkpoint_ = false;
    if (reply_) {
        auto old = reply_;
        reply_.clear();
        old->abort();
    }
}
void MarketHistory::clear() {
    cancel(false);
    cache_.clear();
    current_info_ = {};
    emit history_changed();
}
void MarketHistory::load(const QString &symbol, int preset, bool force) {
    if (preset < 0 || preset > 2) {
        cancel();
        emit failed(QStringLiteral("Некорректный период."));
        return;
    }
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    const int days = preset == 0 ? 90 : preset == 1 ? 365 : 7;
    load_range(symbol, today.addDays(-days), today, preset == 2 ? 60 : 24, force, preset);
}
void MarketHistory::load_range(const QString &symbol, const QDate &from, const QDate &till, int interval,
                               bool force, int result_preset) {
    cancel();
    try {
        if (parse_symbols(symbol) != QStringList{symbol} || !from.isValid() || !till.isValid() ||
            from.year() < 1990 || till.year() > 3000 || from > till || from.daysTo(till) > 7305 ||
            (interval != 24 && interval != 60))
            throw std::invalid_argument("range");
    } catch (...) {
        emit failed(QStringLiteral("Проверьте тикер, даты (не более 20 лет) и интервал."));
        return;
    }
    const int preset = result_preset >= 0 ? result_preset : (interval == 60 ? 2 : 1);
    const QString key = symbol + ":" + from.toString(Qt::ISODate) + ":" + till.toString(Qt::ISODate) + ":" +
                        QString::number(interval);
    const auto now = now_ms();
    auto cached = cache_.constFind(key);
    if (!force && cached != cache_.cend() && now >= cached->fetched_ms && now - cached->fetched_ms < 300000) {
        const auto entry = *cached;
        current_info_ = entry.info;
        const auto generation=generation_;
        emit quality_report(symbol, entry.received, entry.rejected);
        if(generation==generation_)emit loaded(symbol, preset, entry.bars, true);
        return;
    }
    if (now < resume_ms_) {
        emit failed(QStringLiteral("Лимит API; повторите через %1 с.").arg((resume_ms_ - now + 999) / 1000));
        return;
    }
    active_ = true;
    request_ = {key, symbol, from, till, interval, preset, now, false, false};
    received_ = rejected_ = 0;
    emit history_changed();
    page(symbol, preset, key, from, till, interval, generation_, 0);
}
void MarketHistory::complete(const QString &symbol, int preset, const QString &key, const Entry *comparison) {
    if (extension_) {
        complete_extension();
        return;
    }
    active_ = false;
    if (accumulated_.isEmpty()) {
        emit failed(QStringLiteral("Нет доступной публичной истории за выбранный период."));
        return;
    }
    HistorySetInfo info;
    info.key = key;
    info.symbol = symbol;
    info.from = request_.from;
    info.till = request_.till;
    info.interval = request_.interval;
    info.preset = preset;
    info.bars = accumulated_.size();
    info.started_ms = request_.started_ms;
    info.fetched_ms = now_ms();
    info.limited = request_.limited;
    info.resumed = request_.resumed;
    info.extended = request_.extended;
    info.checked_from = request_.checked_from;
    info.checked_till = request_.checked_till;
    info.prepended = request_.prepended;
    info.rechecked = request_.rechecked;
    info.base_revision = request_.base_revision;
    info.reused_bars = request_.reused_bars;
    info.sha256 = content_hash(accumulated_);
    const auto found = cache_.constFind(key);
    const Entry *previous = comparison ? comparison : (found == cache_.cend() ? nullptr : &found.value());
    if (previous) {
        info.comparable = true;
        info.revision = previous->info.revision;
        qsizetype a = 0, b = 0;
        while (a < previous->bars.size() || b < accumulated_.size()) {
            if (a == previous->bars.size()) {
                ++info.added;
                ++b;
            } else if (b == accumulated_.size()) {
                ++info.removed;
                ++a;
            } else if (previous->bars[a].time_ms < accumulated_[b].time_ms) {
                ++info.removed;
                ++a;
            } else if (previous->bars[a].time_ms > accumulated_[b].time_ms) {
                ++info.added;
                ++b;
            } else {
                info.changed += !same(previous->bars[a], accumulated_[b]);
                ++a;
                ++b;
            }
        }
    }
    const Entry *revision_base = found == cache_.cend() ? previous : &found.value();
    info.revision = (revision_base && info.sha256 == revision_base->info.sha256)
                        ? revision_base->info.revision
                        : ++revision_;
    if (cache_.size() >= 8 && !cache_.contains(key)) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->fetched_ms < oldest->fetched_ms)
                oldest = it;
        cache_.erase(oldest);
    }
    cache_[key] = {accumulated_, info.fetched_ms, received_, rejected_, info};
    current_info_ = info;
    if (checkpoint_.request.key == key)
        checkpoint_ = {};
    const auto bars = accumulated_;
    accumulated_.clear();
    const auto generation = generation_;
    emit history_changed();
    if(generation!=generation_)return;
    emit quality_report(symbol, received_, rejected_);
    if (generation == generation_)
        emit loaded(symbol, preset, bars, false);
}
void MarketHistory::page(const QString &symbol, int preset, const QString &key, QDate from, QDate till,
                         int interval, quint64 generation, int offset) {
    if (generation != generation_ || !active_)
        return;
    QUrl url = public_base_.resolved(QUrl(symbol + "/candles.json"));
    QUrlQuery query;
    query.addQueryItem("iss.meta", "off");
    query.addQueryItem("iss.only", "candles");
    query.addQueryItem("from", from.toString(Qt::ISODate));
    query.addQueryItem("till", till.toString(Qt::ISODate));
    query.addQueryItem("interval", QString::number(interval));
    query.addQueryItem("start", QString::number(offset));
    query.addQueryItem("limit", "500");
    query.addQueryItem("candles.columns", "begin,open,high,low,close,volume");
    url.setQuery(query);
    reply_ = request_json(
        network_, QNetworkRequest(url), this,
        [this, symbol, preset, key, from, till, interval, generation, offset](const HttpResult &result) {
            if (generation != generation_)
                return;
            reply_.clear();
            if (!result.ok()) {
                if (result.status == 429)
                    resume_ms_ = now_ms() + std::max(5, result.retry_after_seconds) * 1000;
                fail(http_error_text(result), result.error == HttpResult::Error::Network ||
                                                  result.error == HttpResult::Error::Timeout ||
                                                  result.status == 429 || result.status >= 500);
                return;
            }
            const auto block = result.document.object().value("candles").toObject();
            if (!valid_block(block)) {
                fail(QStringLiteral("Некорректная схема или размер страницы свечей API."), false);
                return;
            }
            if (verifying_checkpoint_) {
                verifying_checkpoint_ = false;
                if (page_hash(block) != checkpoint_.page_hash) {
                    if (extension_) {
                        restart_extension(QStringLiteral("Изменилась контрольная страница догрузки."));
                        return;
                    }
                    checkpoint_ = {};
                    accumulated_.clear();
                    received_ = rejected_ = 0;
                    request_.started_ms = now_ms();
                    request_.resumed = false;
                    emit state_message(
                        QStringLiteral("Контрольная страница изменилась. Период загружается заново."));
                    emit history_changed();
                    if (generation == generation_)
                        page(symbol, preset, key, from, till, interval, generation, 0);
                } else {
                    const int next = checkpoint_.next_offset;
                    emit state_message(
                        QStringLiteral("Контрольная страница совпала; загрузка продолжается."));
                    QTimer::singleShot(150, this, [=] {
                        page(symbol, preset, key, from, till, interval, generation, next);
                    });
                }
                return;
            }
            const int raw = block.value("data").toArray().size();
            auto bars = parse_candles(result.document);
            received_ += raw;
            rejected_ += raw - static_cast<int>(bars.size());
            const auto before = accumulated_.size();
            for (const auto &bar : bars) {
                const auto date =
                    QDateTime::fromMSecsSinceEpoch(bar.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).date();
                if (date < from || date > till ||
                    (!accumulated_.isEmpty() && bar.time_ms <= accumulated_.back().time_ms)) {
                    ++rejected_;
                    continue;
                }
                if (accumulated_.size() < 5000)
                    accumulated_.push_back(bar);
            }
            if (raw > 0 && bars.isEmpty()) {
                fail(QStringLiteral("Страница не содержит корректных свечей."), false);
                return;
            }
            if (raw == 500 && accumulated_.size() > before && accumulated_.size() < 5000) {
                checkpoint_ = {request_,  accumulated_, page_hash(block), received_,
                               rejected_, offset,       offset + raw,     now_ms()};
                checkpoint_.extension = extension_;
                emit history_changed();
            }
            if (generation != generation_)
                return;
            emit progress(symbol, accumulated_.size(), 5000);
            if (generation != generation_)
                return;
            if (accumulated_.size() >= 5000) {
                request_.limited = true;
                emit truncated(symbol);
                if (generation != generation_)
                    return;
                complete(symbol, preset, key);
                return;
            }
            if (raw == 0 || raw < 500) {
                complete(symbol, preset, key);
                return;
            }
            if (accumulated_.size() == before || offset >= 5000) {
                fail(QStringLiteral("API не продвигает страницу; загрузка остановлена без зацикливания."),
                     false);
                return;
            }
            QTimer::singleShot(150, this, [=] {
                page(symbol, preset, key, from, till, interval, generation, offset + raw);
            });
        },
        {}, {512 * 1024, 15000});
}
void MarketHistory::fail(const QString &message, bool retain) {
    active_ = false;
    extension_.reset();
    accumulated_.clear();
    verifying_checkpoint_ = false;
    if (!retain && checkpoint_.request.key == request_.key)
        checkpoint_ = {};
    emit history_changed();
    emit failed(message);
}
QVector<HistorySetInfo> MarketHistory::datasets() const {
    QVector<HistorySetInfo> result;
    for (const auto &entry : cache_)
        result.push_back(entry.info);
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.fetched_ms > b.fetched_ms; });
    return result;
}
HistoryCheckpointInfo MarketHistory::checkpoint() const {
    const auto now = now_ms();
    if (checkpoint_.bars.isEmpty() || now < checkpoint_.saved_ms || now - checkpoint_.saved_ms > 1800000)
        return {};
    const auto &c = checkpoint_;
    return {true,
            c.request.symbol,
            c.extension ? c.extension->target_from : c.request.from,
            c.extension ? c.extension->target_till : c.request.till,
            c.request.interval,
            c.request.preset,
            static_cast<int>(c.bars.size()),
            c.next_offset,
            c.saved_ms,
            static_cast<bool>(c.extension)};
}
void MarketHistory::discard_checkpoint() {
    if (active_)
        cancel(false);
    checkpoint_ = {};
    emit history_changed();
}
bool MarketHistory::restore(const QString &key) {
    if (active_)
        return false;
    const auto it = cache_.constFind(key);
    if (it == cache_.cend())
        return false;
    const auto entry = *it;
    current_info_ = entry.info;
    const auto generation = generation_;
    emit state_message(QStringLiteral("Набор из RAM: сеть не перепроверялась."));
    if(generation!=generation_)return false;
    emit quality_report(entry.info.symbol, entry.received, entry.rejected);
    if (generation == generation_)
        emit loaded(entry.info.symbol, entry.info.preset, entry.bars, true);
    return true;
}
bool MarketHistory::resume() {
    if (active_)
        return false;
    if (!checkpoint().available) {
        discard_checkpoint();
        emit failed(QStringLiteral("Нет действующей контрольной точки (срок 30 минут)."));
        return false;
    }
    if (now_ms() < resume_ms_) {
        emit failed(QStringLiteral("Лимит API; продолжение доступно через %1 с.")
                        .arg((resume_ms_ - now_ms() + 999) / 1000));
        return false;
    }
    const auto saved = checkpoint_;
    cancel();
    request_ = saved.request;
    extension_ = saved.extension;
    request_.resumed = true;
    accumulated_ = saved.bars;
    received_ = saved.received;
    rejected_ = saved.rejected;
    active_ = true;
    verifying_checkpoint_ = true;
    const auto generation = generation_;
    emit state_message(QStringLiteral("Проверяется последняя принятая страница перед возобновлением."));
    emit history_changed();
    if (generation != generation_ || !active_)
        return false;
    page(request_.symbol, request_.preset, request_.key, request_.from, request_.till, request_.interval,
         generation, saved.last_offset);
    return true;
}
} // namespace live
