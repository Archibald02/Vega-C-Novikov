#include "research_panel.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>

namespace live {
namespace {
QDate moscow_date(qint64 timestamp) {
    static const QTimeZone zone("Europe/Moscow");
    return QDateTime::fromMSecsSinceEpoch(timestamp).toTimeZone(zone).date();
}
}
void ResearchPanel::setup_calendar() {
    calendar_check_ = new QPushButton(QStringLiteral("Сверить дни с календарём MOEX"));
    calendar_status_ = new QLabel(QStringLiteral(
        "Сверка по кнопке. Календарь фондового рынка показывает дни торгов, часовые паузы требуют отдельных данных."));
    calendar_status_->setTextFormat(Qt::PlainText);
    calendar_status_->setWordWrap(true);
    quality_layout_->addWidget(calendar_check_);
    quality_layout_->addWidget(calendar_status_);
    connect(calendar_check_, &QPushButton::clicked, this, &ResearchPanel::start_calendar);
}
void ResearchPanel::start_calendar() {
    if (calendar_loading_ || loading_ || synthetic_ || local_project_ || source_dirty_)
        return;
    const auto &bars = selected_bars();
    if (bars.size() < 2)
        return;
    const auto first = moscow_date(bars.front().time);
    const auto last = moscow_date(bars.back().time);
    calendar_till_ = last.addDays(-1);
    calendar_from_ = first.addDays(1);
    if (calendar_from_.daysTo(calendar_till_) > 7305) {
        calendar_status_->setText(QStringLiteral("Сверка ограничена 20 годами. Выберите меньший диапазон."));
        return;
    }
    if (calendar_from_ > calendar_till_) {
        calendar_status_->setText(QStringLiteral("Внутри диапазона нет целых дней для сверки."));
        return;
    }
    if (const auto wait = std::max(calendar_retry_until_ - QDateTime::currentMSecsSinceEpoch(),
                                   loader_.retry_delay_ms()); wait > 0) {
        calendar_status_->setText(QStringLiteral("Лимит ISS. Повторите через %1 с.").arg((wait + 999) / 1000));
        return;
    }
    calendar_days_.clear();
    candle_days_.clear();
    for (const auto &bar : bars) {
        const auto day = moscow_date(bar.time);
        if (day >= calendar_from_ && day <= calendar_till_)
            candle_days_.insert(day);
    }
    calendar_loading_ = true;
    calendar_status_->setText(QStringLiteral("Запрашивается календарь фондового рынка MOEX…"));
    update_journey();
    calendar_page(0, 0, ++calendar_generation_);
}
void ResearchPanel::calendar_page(int offset, int page, quint64 generation) {
    if (generation != calendar_generation_ || !calendar_loading_)
        return;
    if (page >= 100) {
        finish_calendar(QStringLiteral("Превышен предел страниц календаря."));
        return;
    }
    QUrl url = calendar_url_;
    QUrlQuery query;
    query.addQueryItem("show_all_days", "1");
    query.addQueryItem("iss.only", "off_days");
    query.addQueryItem("iss.meta", "off");
    query.addQueryItem("from", calendar_from_.toString(Qt::ISODate));
    query.addQueryItem("till", calendar_till_.toString(Qt::ISODate));
    query.addQueryItem("start", QString::number(offset));
    url.setQuery(query);
    calendar_reply_ = request_json(study_network_, QNetworkRequest(url), this,
        [this, offset, page, generation](const HttpResult &result) {
            if (generation != calendar_generation_)
                return;
            calendar_reply_.clear();
            if (!result.ok()) {
                if (result.status == 429)
                    calendar_retry_until_ = QDateTime::currentMSecsSinceEpoch() +
                                            std::max(5, result.retry_after_seconds) * 1000;
                finish_calendar(http_error_text(result));
                return;
            }
            const auto block = result.document.object().value("off_days").toObject();
            const auto columns = block.value("columns").toArray();
            int date = -1, traded = -1;
            for (int i = 0; i < columns.size(); ++i) {
                if (columns[i].toString() == "tradedate")
                    date = date == -1 ? i : -2;
                if (columns[i].toString() == "is_traded")
                    traded = traded == -1 ? i : -2;
            }
            const auto rows = block.value("data").toArray();
            const auto count = calendar_from_.daysTo(calendar_till_) + 1;
            if (date < 0 || traded < 0 || !block.value("data").isArray() || offset + rows.size() > count) {
                finish_calendar(QStringLiteral("ISS вернул неверную структуру или лишние даты календаря."));
                return;
            }
            for (const auto &value : rows) {
                const auto row = value.toArray();
                if (row.size() <= std::max(date, traded)) {
                    finish_calendar(QStringLiteral("ISS вернул неполную строку календаря."));
                    return;
                }
                const auto day = QDate::fromString(row[date].toString(), Qt::ISODate);
                const auto state = row[traded];
                const auto number = state.toDouble(-1);
                if (!day.isValid() || day < calendar_from_ || day > calendar_till_ ||
                    calendar_days_.contains(day) ||
                    (!state.isNull() && (!state.isDouble() || (number != 0 && number != 1)))) {
                    finish_calendar(QStringLiteral("ISS вернул неверные или повторные даты календаря."));
                    return;
                }
                calendar_days_.insert(day, state.isNull() ? -1 : static_cast<int>(number));
            }
            if (rows.isEmpty() || calendar_days_.size() == count) {
                finish_calendar();
                return;
            }
            QTimer::singleShot(150, this, [this, next = offset + static_cast<int>(rows.size()), page, generation] {
                calendar_page(next, page + 1, generation);
            });
        }, {}, {128 * 1024, 10000});
}
void ResearchPanel::finish_calendar(const QString &error) {
    calendar_loading_ = false;
    if (!error.isEmpty()) {
        calendar_status_->setText(QStringLiteral("Календарь недоступен. ") + error);
    } else {
        int closed = 0, unknown = 0, days_with_candles = 0, candidates = 0, closed_with_candles = 0;
        QStringList examples;
        for (auto day = calendar_from_; day <= calendar_till_; day = day.addDays(1)) {
            const auto state = calendar_days_.value(day, -1);
            const bool has = candle_days_.contains(day);
            if (state == 0) {
                ++closed;
                closed_with_candles += has;
            } else if (state < 0) {
                ++unknown;
            } else if (has) {
                ++days_with_candles;
            } else {
                ++candidates;
                if (examples.size() < 8)
                    examples << day.toString(Qt::ISODate);
            }
        }
        calendar_status_->setText(QStringLiteral(
            "Календарь MOEX, %1—%2 (МСК). Торговых дней со свечами: %3. "
            "Торговых дней без свечей: %4. Неторговых: %5, из них со свечами: %6. Данных календаря нет: %7.\n"
            "Расписание рынка не подтверждает сделки по конкретному тикеру. Проверка не определяет "
            "часовые паузы и не меняет свечи. Граничные дни исключены, поскольку могут быть неполными.")
            .arg(calendar_from_.toString(Qt::ISODate), calendar_till_.toString(Qt::ISODate))
            .arg(days_with_candles).arg(candidates).arg(closed).arg(closed_with_candles).arg(unknown) +
            (examples.isEmpty() ? QString{} : QStringLiteral("\nДаты для проверки: ") + examples.join(", ")));
    }
    calendar_days_.clear();
    candle_days_.clear();
    update_journey();
}
void ResearchPanel::cancel_calendar() {
    ++calendar_generation_;
    calendar_loading_ = false;
    if (calendar_reply_) {
        auto reply = calendar_reply_;
        calendar_reply_.clear();
        reply->abort();
    }
    calendar_days_.clear();
    candle_days_.clear();
    if (calendar_status_)
        calendar_status_->setText(QStringLiteral(
            "Сверка по кнопке. Календарь фондового рынка показывает дни торгов, часовые паузы требуют отдельных данных."));
}
} // namespace live
