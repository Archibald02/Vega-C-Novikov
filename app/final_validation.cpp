#include "research_panel.hpp"
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace live {
namespace {
QString utc(qint64 t, bool compact = false) {
    static const QTimeZone zone("UTC");
    const auto date = QDateTime::fromMSecsSinceEpoch(t, zone);
    return compact ? date.toString("yyyy-MM-dd HH:mm:ss") : date.toString(Qt::ISODate);
}
QString number(double v) { return QString::number(v, 'f', 6); }
QList<QStringList> comparison_rows(const quant::HoldoutResult &r) {
    QList<QStringList> rows;
    for (const auto &model : {std::make_pair(QStringLiteral("Стратегия SMA %1/%2").arg(r.frozen.strategy.fast).arg(r.frozen.strategy.slow), &r.test),
                              std::make_pair(QStringLiteral("Купить и держать"), &r.baseline)}) {
        const auto &s = *model.second;
        if (s.times.empty() || s.equity.empty()) continue;
        rows << QStringList{model.first, utc(s.start_time, true), utc(s.times.back(), true), number(s.equity.front()),
                            number(s.metrics.total_return * 100), number(s.metrics.max_drawdown * 100)};
    }
    return rows;
}
bool save(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool wait_finished(QFutureWatcher<LabResult> *worker) {
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (!worker->isRunning()) loop.quit(); });
    poll.start(5);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::processEvents();
    return !worker->isRunning();
}
}
QString final_validation_html(const quant::HoldoutResult &r) {
    QString html = QStringLiteral("<h2>Финальная проверка</h2><p>Обе модели начинают с денег, без позиции с обучения. "
        "Для подбора использованы первые %1 свечей. Разрыв перед оценкой составляет %2 свечей. "
        "Комиссия %3%, фиксированный сбор %4 ₽, спред %5 б.п., проскальзывание %6 б.п. "
        "Доля капитала %7%. Начало включительно, конец исключительно. Время указано в UTC.</p>")
        .arg(r.split).arg(r.gap).arg(number(r.frozen.commission_rate * 100), number(r.frozen.fixed_fee),
                       number(r.frozen.spread_bps), number(r.frozen.slippage_bps), number(r.frozen.allocation * 100));
    html += QStringLiteral("<table border='1' cellspacing='0' cellpadding='5'><tr><th>Модель</th><th>Начало UTC</th>"
        "<th>Конец UTC (искл.)</th><th>Начальный капитал, ₽</th><th>Чистая доходность, %</th><th>Макс. просадка, %</th></tr>");
    for (const auto &row : comparison_rows(r)) {
        html += "<tr>";
        for (const auto &cell : row) html += "<td>" + cell.toHtmlEscaped() + "</td>";
        html += "</tr>";
    }
    return html + "</table>";
}
void ResearchPanel::present_holdout_table() {
    holdout_table_->setRowCount(0);
    if (!last_result_.has_holdout) return;
    for (const auto &values : comparison_rows(last_result_.holdout)) {
        const auto row = holdout_table_->rowCount();
        holdout_table_->insertRow(row);
        for (int col = 0; col < values.size(); ++col)
            holdout_table_->setItem(row, col, new QTableWidgetItem(values[col]));
    }
    holdout_table_->setToolTip(QStringLiteral("Одинаковые издержки и капитал. Свежие счета после разрыва, "
        "без переноса позиций с обучения. Результаты за весь диапазон показаны отдельно."));
}
bool ResearchPanel::holdout_self_test() {
    ResearchPanel example;
    example.demo();
    auto &bars = example.data_.at(example.selected_->currentText());
    bars.resize(100);
    example.source_dirty_ = false;
    example.update_journey();
    example.gap_->setValue(15);
    example.holdout_open_->click();
    if (example.worker_->isRunning() || !example.holdout_used_.empty() ||
        !example.holdout_status_->text().contains(QStringLiteral("отложено 20; разрыв 15")) ||
        !example.holdout_status_->text().contains(QStringLiteral("остаётся 5, требуется минимум 10")) ||
        !example.holdout_open_->isEnabled()) {
        qWarning("Holdout regression: invalid split consumed first view"); return false;
    }
    example.gap_->setValue(5);
    example.holdout_open_->click();
    if (example.holdout_open_->isEnabled()) return false;
    if (!wait_finished(example.worker_) || !example.result_current_ || !example.last_result_.has_holdout ||
        example.holdout_used_.size() != 1 || example.holdout_table_->rowCount() != 2 ||
        !example.holdout_status_->text().startsWith(QStringLiteral("Первый успешный")) ||
        !example.holdout_open_->isEnabled()) {
        qWarning("Holdout regression: first successful view: %s", qPrintable(example.holdout_status_->text())); return false;
    }
    if (!example.report_html(false).contains(final_validation_html(example.last_result_.holdout)) ||
        !example.report_html(true).contains(final_validation_html(example.last_result_.holdout))) return false;
    example.holdout_open_->click();
    if (!wait_finished(example.worker_) || !example.holdout_status_->text().startsWith(QStringLiteral("ПОВТОРНЫЙ"))) return false;
    // Explicitly exposing the reserve still counts, even before a final calculation.
    example.clear_private_state(); example.demo(); example.data_.at(example.selected_->currentText()).resize(100);
    example.reserve_->setChecked(false); example.update_journey();
    example.holdout_open_->click();
    if (!wait_finished(example.worker_) || !example.holdout_status_->text().startsWith(QStringLiteral("ПОВТОРНЫЙ"))) return false;
    return true;
}

bool ResearchPanel::create_defense_example(const QString &directory) {
    QDir root(directory);
    if (!QDir().mkpath(directory)) return false;
    // This protocol and the complete deterministic data rule are written BEFORE opening holdout.
    const QString hypothesis = QStringLiteral("Пересечение SMA уменьшит максимальную просадку на отложенном "
        "участке; чистая доходность может ухудшиться из-за пропущенного роста и издержек. "
        "Это учебная гипотеза на синтетических данных, не вывод об акции MOEX.");
    const QJsonObject protocol{{"source", "SYNTHETIC DEMO, NOT MOEX"}, {"symbol", "DEMO"},
        {"bars", 480}, {"split", 384}, {"gap", 5}, {"cash", 100000}, {"commission", .0005},
        {"spread_bps", 2}, {"slippage_bps", 2}, {"allocation", 1}, {"lot", 1}, {"tick", .01},
        {"fixed_fee", 0}, {"stop_loss", 0}, {"take_profit", 0}, {"max_drawdown_limit", 1},
        {"daily_loss_limit", 1}, {"liquidate_end", true},
        {"entry_rule", "SMA fast crosses above SMA slow on close, buy next open"},
        {"exit_rule", "SMA fast crosses below SMA slow on close, sell next open"},
        {"hypothesis", hypothesis}, {"selection", "Max net training return over fast={5,10,20}, slow={20,40,60}, fast<slow; first candidate wins ties; no holdout selection"},
        {"generator", "480 weekdays from 2024-01-02; close(i)=100+0.08*i+8*sin(0.19*i) for i<384; else 130.72-0.22*(i-384)+8*sin(0.19*i); open=previous close; high=max(open,close)+0.5; low=min(open,close)-0.5; volume=50000; Moscow midnight"}};
    if (!save(root.filePath("protocol.json"), QJsonDocument(protocol).toJson())) return false;
    demo(); data_.clear(); history_origins_.clear();
    std::vector<quant::Bar> bars;
    QDate date(2024, 1, 2);
    const QTimeZone zone("Europe/Moscow");
    double previous = 100;
    for (int i = 0; i < 480; ++i) {
        while (date.dayOfWeek() > 5) date = date.addDays(1);
        const double close = (i < 384 ? 100 + .08 * i : 130.72 - .22 * (i - 384)) + 8 * std::sin(.19 * i);
        bars.push_back({QDateTime(date, QTime(0,0), zone).toMSecsSinceEpoch(),
            QDateTime(date.addDays(1), QTime(0,0), zone).toMSecsSinceEpoch(), previous,
            std::max(previous, close) + .5, std::min(previous, close) - .5, close, 50000, 10800});
        previous = close; date = date.addDays(1);
    }
    period_->setCurrentIndex(1);
    custom_dates_->setChecked(true);
    from_->setDate(QDateTime::fromMSecsSinceEpoch(bars.front().time, zone).date());
    till_->setDate(QDateTime::fromMSecsSinceEpoch(bars.back().time, zone).date());
    data_["DEMO"] = bars;
    symbols_->setText("DEMO");
    selected_->clear();
    selected_->addItem("DEMO");
    selected_->setCurrentIndex(0);
    synthetic_ = true; source_dirty_ = false; local_project_ = false;
    cash_->setValue(100000); commission_->setValue(.05); fixed_->setValue(0);
    spread_->setValue(2); slippage_->setValue(2); allocation_->setValue(100);
    stop_->setValue(0); target_->setValue(0); max_dd_->setValue(100); daily_->setValue(100);
    risk_->setValue(100); participation_->setValue(0); lot_->setValue(1); tick_->setValue(.01);
    gap_->setValue(5); reserve_->setChecked(true); bootstrap_enabled_->setChecked(false);
    save_cash_->setChecked(true); paper_.reset();
    update_journey();
    holdout_open_->click();
    if (!wait_finished(worker_) || !result_current_ || !last_result_.has_holdout ||
        !holdout_status_->text().startsWith(QStringLiteral("Первый успешный"))) {
        qWarning("Defense calculation: %s | %s", qPrintable(status_->text()), qPrintable(holdout_status_->text()));
        return false;
    }
    const auto &r = last_result_;
    const auto bytes = encode_snapshot(*r.snapshot, QStringLiteral("СИНТЕТИЧЕСКИЙ ПРИМЕР DEMO, НЕ MOEX"),
                                       QStringLiteral("SMA: просадка и чистая доходность на отложенном участке"), hypothesis);
    std::atomic<bool> cancel{false};
    const auto verified = verify_snapshot(bytes, cancel);
    if (!verified.startsWith(QStringLiteral("СОВПАЛО:")) || !save(root.filePath("example.qps.json"), bytes) ||
        !save_project(root.filePath("example.qpl.json"), true)) {
        qWarning("Defense replay or export: %s", qPrintable(verified));
        return false;
    }
    const auto &h = r.holdout;
    auto trade = std::find_if(h.test.journal.begin(), h.test.journal.end(), [](const auto &e) { return e.quantity > 0; });
    if (trade == h.test.journal.end()) {
        qWarning("Defense example has no entry on the final period");
        return false;
    }
    auto index = std::find_if(bars.begin(), bars.end(), [&](const auto &b) { return b.time == trade->time; }) - bars.begin();
    if (index < 2 || index == static_cast<std::ptrdiff_t>(bars.size())) return false;
    const auto features = quant::candle_features(bars, h.frozen.strategy.fast, h.frozen.strategy.slow, h.frozen.strategy.indicator_window);
    const auto feature = [](quant::Feature f) { return static_cast<std::size_t>(f); };
    const QString explanation = QStringLiteral("Сделка стратегии: %1 куплено %2 акций по модельной цене %3 ₽; деньги после входа %4 ₽. "
        "На предыдущем закрытии %5 быстрая SMA=%6, медленная=%7; ещё свечой раньше SMA=%8/%9. "
        "Быстрая SMA пересекла медленную вверх, покупка исполнена на следующем открытии с заданными издержками.")
        .arg(utc(trade->time)).arg(trade->quantity).arg(number(trade->price), number(trade->cash), utc(bars[index-1].end),
            number(features[index-1][feature(quant::Feature::FastSma)]), number(features[index-1][feature(quant::Feature::SlowSma)]),
            number(features[index-2][feature(quant::Feature::FastSma)]), number(features[index-2][feature(quant::Feature::SlowSma)]));
    const QString conclusion = QStringLiteral("На финальном участке просадка стратегии %1%, базы %2%; чистая доходность %3% и %4%. "
        "Разность доходностей стратегии и базы: %5 п.п. Гипотеза о меньшей просадке %6. "
        "Вывод относится к синтетическому сценарию. Он показывает, как устроена проверка стратегии.")
        .arg(number(h.test.metrics.max_drawdown*100), number(h.baseline.metrics.max_drawdown*100),
             number(h.test.metrics.total_return*100), number(h.baseline.metrics.total_return*100),
             number((h.test.metrics.total_return-h.baseline.metrics.total_return)*100),
             h.test.metrics.max_drawdown < h.baseline.metrics.max_drawdown ? QStringLiteral("подтвердилась в этом примере") : QStringLiteral("не подтвердилась"));
    const QString html = QStringLiteral("<!doctype html><html lang='ru'><meta charset='utf-8'><title>SMA: учебный пример</title>"
        "<body><h1>СИНТЕТИЧЕСКИЙ ПРИМЕР — НЕ ДАННЫЕ MOEX</h1><p>%1</p><p>480 дневных свечей DEMO, "
        "начала свечей %2 — %3 UTC. Первые 384 — обучение, 5 — разрыв, 91 — оценка. "
        "Параметры выбраны по максимальной чистой доходности только на обучении: SMA %4/%5, 8 кандидатов.</p>")
        .arg(hypothesis.toHtmlEscaped(), utc(bars.front().time), utc(bars.back().time))
        .arg(h.frozen.strategy.fast).arg(h.frozen.strategy.slow) + final_validation_html(h) +
        "<h2>Одна конкретная сделка</h2><p>" + explanation.toHtmlEscaped() + "</p><h2>Вывод</h2><p>" + conclusion.toHtmlEscaped() +
        "</p><p>Источник: формула protocol.json, без случайных чисел. Дивиденды, сплиты, налоги и ограничения ликвидности "
        "не моделируются; рыночных данных здесь нет. MOEX ISS недоступна при подготовке; подтверждённого сохранённого набора не найдено. "
        "Исследование реальной акции на MOEX остаётся незавершённым.</p><p><a href='example.qps.json'>Проверяемый снимок</a> · "
        "<a href='example.qpl.json'>Проект с исходными свечами</a> · <a href='protocol.json'>Протокол до открытия</a></p></body></html>";
    QJsonObject facts{{"symbol", "DEMO"}, {"synthetic", true}, {"fast",h.frozen.strategy.fast}, {"slow",h.frozen.strategy.slow},
        {"from",utc(h.test.start_time)}, {"till",utc(h.test.times.back())}, {"cash",h.test.equity.front()},
        {"strategy_return",h.test.metrics.total_return}, {"strategy_drawdown",h.test.metrics.max_drawdown},
        {"baseline_return",h.baseline.metrics.total_return}, {"baseline_drawdown",h.baseline.metrics.max_drawdown},
        {"verification",verified}, {"trade",explanation}};
    const auto project_report = report_html();
    const auto final_table = comparison_rows(h);
    ResearchPanel reopened;
    if (!reopened.open_project(root.filePath("example.qpl.json")) || !reopened.holdout_open_->isEnabled()) {
        qWarning("Defense project did not reopen with usable data");
        return false;
    }
    reopened.holdout_open_->click();
    if (!wait_finished(reopened.worker_) || !reopened.result_current_ || !reopened.last_result_.has_holdout ||
        comparison_rows(reopened.last_result_.holdout) != final_table ||
        !reopened.holdout_status_->text().startsWith(QStringLiteral("ПОВТОРНЫЙ"))) {
        qWarning("Defense project replay: %s | %s", qPrintable(reopened.status_->text()),
                 qPrintable(reopened.holdout_status_->text()));
        return false;
    }
    return write_report(html, root.filePath("report.html"), false) &&
           write_report(html, root.filePath("report.pdf"), true) &&
           write_report(project_report, root.filePath("full-report.html"), false) &&
           write_report(report_html(true), root.filePath("full-report.pdf"), true) &&
           save(root.filePath("result.json"), QJsonDocument(facts).toJson()) &&
           save(root.filePath("verification.txt"), verified.toUtf8() + "\n");
}
} // namespace live
