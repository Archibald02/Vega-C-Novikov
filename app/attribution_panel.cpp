#include "research_panel.hpp"
#include <QBuffer>
#include <QDateTime>
#include <QEventLoop>
#include <QHeaderView>
#include <QScrollArea>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <algorithm>
namespace live {
namespace {
QString number(double x, int digits = 2) {
    return std::isfinite(x) ? QString::number(x, 'f', digits) : QStringLiteral("—");
}
QLabel *paragraph(const QString &value) {
    auto *label = new QLabel(value);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
QTableWidget *table(const QStringList &headers) {
    auto *t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setMinimumHeight(180);
    return t;
}
void fit_table(QTableWidget *t) {
    const int rows = std::min(8, t->rowCount());
    const int height = t->horizontalHeader()->height() + rows * t->verticalHeader()->defaultSectionSize() +
                       2 * t->frameWidth() + 24;
    t->setFixedHeight(std::clamp(height, 90, 320));
}
void add_row(QTableWidget *t, const QStringList &values, const QString &tooltip = {}) {
    const int row = t->rowCount();
    t->insertRow(row);
    for (int col = 0; col < values.size(); ++col) {
        auto *item = new QTableWidgetItem(values[col]);
        item->setToolTip(tooltip);
        t->setItem(row, col, item);
    }
}
QString local_time(const quant::Bar &b) {
    return QDateTime::fromMSecsSinceEpoch(b.time)
        .toOffsetFromUtc(b.utc_offset_seconds)
        .toString("dd.MM.yy HH:mm");
}
QString month_name(int key) { return QString("%1-%2").arg(key / 100).arg(key % 100, 2, 10, QChar('0')); }
QString period_name(const quant::PerformancePeriod &p, bool month) {
    return month ? month_name(p.key) : QStringLiteral("Участок %1").arg(p.key);
}
QString summary(const quant::PerformanceAttribution &a) {
    if (a.cancelled)
        return QStringLiteral("Расчёт отменён; частичные оценки не опубликованы.");
    QString text = QStringLiteral("Итог: %1 ₽ → %2 ₽; P&L %3 ₽. Месяцев с наблюдениями: %4.")
                       .arg(number(a.initial_capital), number(a.final_capital), number(a.total_pnl))
                       .arg(a.months.size());
    if (a.trace_status != quant::AttributionTrace::Ready)
        return text +
               QStringLiteral("\nРазбор сделок недоступен: полная согласованная трассировка отсутствует. "
                              "Комиссии по периодам не восстанавливаются из усечённого журнала.");
    return text +
           QStringLiteral(
               "\nЗакрыто позиций %1, их P&L %2 ₽. Открытая позиция: %3 акций, совокупный P&L %4 ₽ "
               "(частичные продажи + оценка остатка). Комиссии всех исполнений %5 ₽. Погрешность сверки %6 ₽."
               "\nСумма положительных P&L закрытых позиций %7 ₽; убытков %8 ₽. Доля лучшей / трёх лучших в "
               "сумме положительных P&L: %9 / %10%.")
               .arg(a.positions.size())
               .arg(number(a.closed_pnl))
               .arg(a.open_units)
               .arg(number(a.open_position_pnl), number(a.fees), number(a.reconciliation_error, 8),
                    number(a.gross_profit), number(a.gross_loss), number(a.top_one_profit_share * 100),
                    number(a.top_three_profit_share * 100));
}
} // namespace
QWidget *ResearchPanel::attribution_panel() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(
        paragraph(QStringLiteral("Анализ зафиксированной кривой одной стратегии. Месяц определяется началом "
                                 "свечи с её часовым смещением; "
                                 "неполные месяцы остаются неполными, отсутствующие не заполняются. Участки "
                                 "— три последовательные части по числу свечей. "
                                 "Позиции переносятся через границы, нового подбора правил нет. Просадка "
                                 "каждого периода отсчитывается заново от его начального капитала. "
                                 "При неположительном начальном капитале процентные показатели периода "
                                 "недоступны, рублёвый результат сохраняется.")));
    attribution_status_ = paragraph(QStringLiteral("Нужен расчёт стратегии."));
    layout->addWidget(attribution_status_);
    const QStringList columns{QStringLiteral("Период"),      QStringLiteral("Свечей"),
                              QStringLiteral("P&L, ₽"),      QStringLiteral("Доходность, %"),
                              QStringLiteral("Просадка, %"), QStringLiteral("Комиссии, ₽")};
    layout->addWidget(paragraph(
        QStringLiteral("Месяцы • последние 120 с данными; капитал начала/конца — в подсказке строки")));
    attribution_months_ = table(columns);
    layout->addWidget(attribution_months_);
    layout->addWidget(paragraph(QStringLiteral("Последовательные участки • одна непрерывная стратегия")));
    attribution_sections_ = table(columns);
    layout->addWidget(attribution_sections_);
    layout->addWidget(
        paragraph(QStringLiteral("Закрытые позиции • последние 500, полный список в HTML. Частичные продажи "
                                 "объединены с финальным выходом. "
                                 "Вклад, п.п. = P&L / начальный капитал ×100. Доходность позиции делится на "
                                 "стоимость её входа с комиссией. "
                                 "Издержки уже учтены в P&L; повторно их вычитать не нужно. Концентрация "
                                 "относится к положительным P&L закрытых позиций, а не к чистой прибыли. "
                                 "Удаление удачной сделки потребовало бы нового бэктеста с иным капиталом; "
                                 "здесь такого пересчёта нет.")));
    attribution_positions_ =
        table({QStringLiteral("Свеча входа"), QStringLiteral("Свеча выхода"), QStringLiteral("Акций"),
               QStringLiteral("Исполнений"), QStringLiteral("P&L, ₽"), QStringLiteral("Комиссии, ₽"),
               QStringLiteral("Доходность, %"), QStringLiteral("Вклад, п.п.")});
    layout->addWidget(attribution_positions_);
    attribution_chart_ = new VisualChart;
    attribution_chart_->setMinimumHeight(220);
    layout->addWidget(attribution_chart_);
    return page;
}
void ResearchPanel::clear_attribution() {
    attribution_status_->setText(QStringLiteral("Нужен новый расчёт стратегии."));
    for (auto *t : {attribution_months_, attribution_sections_, attribution_positions_})
        t->setRowCount(0);
    attribution_chart_->clear();
    for (auto *t : {attribution_months_, attribution_sections_, attribution_positions_})
        fit_table(t);
}
void ResearchPanel::present_attribution(const LabResult &r) {
    const auto &a = r.attribution;
    clear_attribution();
    attribution_status_->setText(summary(a));
    auto show_periods = [&](QTableWidget *t, const std::vector<quant::PerformancePeriod> &periods,
                            bool months) {
        const auto begin = months && periods.size() > 120 ? periods.size() - 120 : 0;
        for (std::size_t i = begin; i < periods.size(); ++i) {
            const auto &p = periods[i];
            add_row(t,
                    {period_name(p, months), QString::number(p.end - p.begin), number(p.pnl),
                     number(p.return_fraction * 100, 4), number(p.max_drawdown * 100, 4), number(p.fees)},
                    QStringLiteral("Капитал: %1 → %2 ₽. Свечи [%3, %4), индексация с нуля.")
                        .arg(number(p.starting_equity), number(p.ending_equity))
                        .arg(p.begin)
                        .arg(p.end));
            t->item(t->rowCount() - 1, 2)->setBackground(p.pnl >= 0 ? QColor("#245a48") : QColor("#693c45"));
        }
    };
    show_periods(attribution_months_, a.months, true);
    show_periods(attribution_sections_, a.sections, false);
    fit_table(attribution_months_);
    fit_table(attribution_sections_);
    if (a.trace_status != quant::AttributionTrace::Ready || !r.snapshot)
        return;
    const auto &bars = r.snapshot->bars;
    const auto first = a.positions.size() > 500 ? a.positions.size() - 500 : 0;
    QVector<QPointF> cumulative;
    double total = 0;
    const auto stride = std::max<std::size_t>(1, (a.positions.size() + 498) / 499);
    cumulative.push_back({0, 0});
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        const auto &p = a.positions[i];
        total += p.pnl;
        if ((i + 1) % stride == 0 || i + 1 == a.positions.size())
            cumulative.push_back({static_cast<double>(i + 1), total});
        if (i >= first)
            add_row(attribution_positions_,
                    {local_time(bars[p.entry]), local_time(bars[p.exit]), QString::number(p.quantity),
                     QString::number(p.fills), number(p.pnl), number(p.fees),
                     number(p.return_fraction * 100, 4), number(p.initial_capital_fraction * 100, 4)},
                    QStringLiteral("Вход с комиссией: %1 ₽; свечей удержания: %2. Даты — начала свечей, не "
                                   "точные внутрисвечные времена исполнения.")
                        .arg(number(p.entry_cost))
                        .arg(p.exit - p.entry + 1));
    }
    fit_table(attribution_positions_);
    if (a.positions.empty())
        attribution_chart_->clear(
            QStringLiteral("Закрытых позиций нет; результат открытой позиции показан отдельно."));
    else
        attribution_chart_->set_lines(
            {{QStringLiteral("Закрытые позиции"), QColor("#39d2b0"), cumulative, false}},
            QStringLiteral("Накопленный P&L закрытых позиций, ₽"),
            QStringLiteral(
                "X — номер закрытой позиции; открытая позиция не включена; расчёт без прореживания"));
    attribution_status_->setText(summary(a) +
                                 QStringLiteral("\nВ таблицах: месяцев %1 из %2, позиций %3 из %4.")
                                     .arg(attribution_months_->rowCount())
                                     .arg(a.months.size())
                                     .arg(attribution_positions_->rowCount())
                                     .arg(a.positions.size()));
}
QString ResearchPanel::attribution_html(bool compact) const {
    const auto &r = last_result_;
    const auto &a = r.attribution;
    QString html =
        "<h2>Периоды и вклад сделок</h2><p>" + summary(a).toHtmlEscaped().replace("\n", "<br>") + "</p>";
    html +=
        QStringLiteral("<p>Месяц — начало свечи с её смещением. Границы не сбрасывают позицию и не "
                       "перенастраивают стратегию. "
                       "Неполные периоды не дополняются. Просадка начинается от капитала границы периода. "
                       "Вклад позиции — процентные пункты исходного капитала; "
                       "P&L уже содержит издержки. Открытая позиция учитывает её частичные продажи и оценку "
                       "остатка. Доля лучших рассчитана от суммы положительных P&L закрытых позиций. "
                       "Отчёт не является результатом удаления сделок или новым прогнозом. — означает "
                       "недоступную оценку.</p>");
    auto periods = [&](const std::vector<quant::PerformancePeriod> &rows, bool months) {
        QString out = months ? "<h3>Месяцы</h3>" : "<h3>Последовательные участки</h3>";
        out += "<table><tr><th>Период</th><th>Свечей</th><th>Капитал начало / конец, ₽</th><th>P&L, "
               "₽</th><th>Доходность, %</th><th>Просадка, %</th><th>Комиссии, ₽</th></tr>";
        const auto count = compact ? std::min<std::size_t>(120, rows.size()) : rows.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto &p = rows[i];
            out +=
                QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3 / "
                               "%4</td><td>%5</td><td>%6</td><td>%7</td><td>%8</td></tr>")
                    .arg(period_name(p, months))
                    .arg(p.end - p.begin)
                    .arg(number(p.starting_equity), number(p.ending_equity), number(p.pnl),
                         number(p.return_fraction * 100, 4), number(p.max_drawdown * 100, 4), number(p.fees));
        }
        return out + QStringLiteral("</table><p>Показано %1 из %2.</p>").arg(count).arg(rows.size());
    };
    html += periods(a.months, true) + periods(a.sections, false);
    if (a.trace_status != quant::AttributionTrace::Ready || !r.snapshot)
        return html;
    html += QStringLiteral("<h3>Закрытые позиции</h3><table><tr><th>Свеча входа</th><th>Свеча "
                           "выхода</th><th>Акций</th><th>Исполнений</th><th>P&L, ₽</th><th>Комиссии, "
                           "₽</th><th>Доходность позиции, %</th><th>Вклад, п.п.</th></tr>");
    const auto count = compact ? std::min<std::size_t>(500, a.positions.size()) : a.positions.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto &p = a.positions[i];
        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</"
                               "td><td>%7</td><td>%8</td></tr>")
                    .arg(local_time(r.snapshot->bars[p.entry]), local_time(r.snapshot->bars[p.exit]))
                    .arg(p.quantity)
                    .arg(p.fills)
                    .arg(number(p.pnl), number(p.fees), number(p.return_fraction * 100, 4),
                         number(p.initial_capital_fraction * 100, 4));
    }
    html += QStringLiteral(
                "</table><p>Позиций показано %1 из %2. В PDF первые 500, HTML содержит весь ограниченный "
                "расчёт. Даты — начала свечей с их смещением, не точное время исполнения.</p>")
                .arg(count)
                .arg(a.positions.size());
    if (!a.positions.empty()) {
        QByteArray image;
        QBuffer buffer(&image);
        buffer.open(QIODevice::WriteOnly);
        attribution_chart_->grab().save(&buffer, "PNG");
        html += "<img src='data:image/png;base64," + QString::fromLatin1(image.toBase64()) + "'>";
    }
    return html;
}
bool ResearchPanel::attribution_test() {
    const auto &a = last_result_.attribution;
    if (a.trace_status != quant::AttributionTrace::Ready || a.months.empty() || a.sections.size() != 3 ||
        a.positions.size() != static_cast<std::size_t>(last_result_.simulation.metrics.closed_trades) ||
        attribution_months_->rowCount() != static_cast<int>(std::min<std::size_t>(120, a.months.size())))
        return false;
    if (!attribution_html(false).contains(QStringLiteral("Периоды и вклад сделок")))
        return false;
    double pnl = 0;
    for (const auto &p : a.months)
        pnl += p.pnl;
    if (std::abs(pnl - a.total_pnl) > 1e-6)
        return false;
    {
        std::vector<quant::Bar> bars;
        for (int i = 0; i < 5000; ++i) {
            const auto t = 1706659200000LL + static_cast<long long>(i) * 86400000;
            bars.push_back({t, t + 3600000, 100, 101, 99, 100, 10000, 10800});
        }
        quant::SimConfig c;
        c.strategy.entry = {
            {{quant::Feature::Close, quant::Feature::Constant, quant::Compare::Above, 0, true}}, true};
        c.strategy.exit = {};
        c.max_holding_bars = 1;
        c.stop_fraction = c.target_fraction = 0;
        c.commission_rate = c.fixed_fee = c.spread_bps = c.slippage_bps = 0;
        const auto simulation = quant::backtest(bars, c, 0, true);
        LabResult bounded;
        bounded.snapshot = std::make_shared<const ReplaySnapshot>(ReplaySnapshot{bars, c, simulation});
        bounded.attribution = quant::performance_attribution(bars, simulation);
        present_attribution(bounded);
        if (attribution_months_->rowCount() != 120 || attribution_positions_->rowCount() != 500)
            return false;
        present_attribution(last_result_);
    }
    const auto frozen = attribution_status_->text();
    const int old = fast_->value();
    fast_->setValue(old + 1);
    if (attribution_status_->text() != frozen)
        return false;
    fast_->setValue(old);
    result_current_ = true;
    status_->setText(
        QStringLiteral("СИНТЕТИЧЕСКОЕ ДЕМО • периоды, вклад позиций и сверка капитала проверены"));
    tabs_->setCurrentIndex(5);
    if (auto *scroll = qobject_cast<QScrollArea *>(tabs_->widget(5))) {
        scroll->widget()->layout()->activate();
        scroll->ensureWidgetVisible(attribution_months_, 0, 0);
    }
    update_journey();
    return true;
}
} // namespace live
