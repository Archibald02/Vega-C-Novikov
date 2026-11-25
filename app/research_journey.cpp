#include "research_panel.hpp"
#include <QApplication>
#include <QDateTime>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QWheelEvent>
namespace live {
void ResearchPanel::update_journey() {
    if (!next_ || !journey_)
        return;
    const int page = tabs_->currentIndex();
    int row = -1;
    for (int i = 0; i < journey_->count(); ++i)
        if (journey_->item(i)->data(Qt::UserRole).toInt() == page)
            row = i;
    {
        QSignalBlocker blocker(journey_);
        journey_->setCurrentRow(row);
    }
    const QStringList titles{
        QStringLiteral("Выберите данные"), QStringLiteral("Проверьте историю"),
        QStringLiteral("Изучите графики"), QStringLiteral("Настройте стратегию"),
        QStringLiteral("Результат стратегии"), QStringLiteral("Проверка устойчивости"),
        QStringLiteral("Виртуальный счёт"), QStringLiteral("Проект и отчёт"),
        QStringLiteral("Справка")};
    step_title_->setText(titles.value(row));
    const auto bars = selected_bars();
    const bool busy =
        !holdout_key_.isEmpty() || loading_ || (worker_ && worker_->isRunning()) || (snapshot_worker_ && snapshot_worker_->isRunning());
    const auto size = reserve_->isChecked() ? bars.size() * 8 / 10 : bars.size();
    const bool ready = size >= 40 && !source_dirty_ && !loading_;
    calculate_->setEnabled(ready && !busy);
    if (holdout_open_) holdout_open_->setEnabled(!busy && !source_dirty_ && bars.size() >= 100);
    calculate_->setVisible(row == 2 || row == 4);
    cancel_button_->setVisible(busy || calendar_loading_);
    calculate_->setText(result_current_ ? QStringLiteral("Пересчитать") : QStringLiteral("Рассчитать"));
    cancel_button_->setEnabled(busy || calendar_loading_);
    calendar_check_->setEnabled(!busy && !calendar_loading_ && !synthetic_ && !local_project_ &&
                                !source_dirty_ && bars.size() >= 2);
    symbols_->setEnabled(!loading_);
    period_->setEnabled(!loading_);
    custom_dates_->setEnabled(!loading_);
    from_->setEnabled(custom_dates_->isChecked() && !loading_);
    till_->setEnabled(custom_dates_->isChecked() && !loading_);
    calculate_->setToolTip(
        ready ? QStringLiteral("Анализ и проверка текущих правил • Ctrl+Enter")
              : QStringLiteral(
                    "Загрузите минимум 40 исследовательских свечей (50 при резерве 20%) и выберите акцию."));
    const QString mode = local_project_ ? (history_origins_.value(selected_->currentText()).startsWith(QStringLiteral("ДИСК"))
        ? QStringLiteral("ДИСК • сеть и происхождение не перепроверены")
        : QStringLiteral("ЛОКАЛЬНЫЙ ПРОЕКТ • происхождение не перепроверено"))
                         : synthetic_   ? QStringLiteral("ДЕМО • синтетические цены")
                                        : QStringLiteral("MOEX ISS • данные с задержкой");
    if (bars.empty())
        dataset_context_->setText(
            QStringLiteral("Нет загруженных свечей. Шаг 1: загрузите MOEX или откройте ДЕМО."));
    else {
        auto date = [](qint64 ms) {
            return QDateTime::fromMSecsSinceEpoch(ms)
                .toTimeZone(QTimeZone("Europe/Moscow"))
                .toString("dd.MM.yyyy HH:mm");
        };
        dataset_context_->setText(QStringLiteral("%1 • %2 • %3 свечей (%4 для исследования)\n%5 — %6 МСК%7")
                                      .arg(mode, selected_->currentText())
                                      .arg(bars.size())
                                      .arg(size)
                                      .arg(date(bars.front().time), date(bars.back().end),
                                           source_dirty_
                                               ? QStringLiteral(" • ИЗМЕНЕНЫ ДАТЫ/ТИКЕРЫ: загрузите заново")
                                               : QString()));
        // Detailed origin and hashes remain on the quality step and in the report.
        if (history_origins_.value(selected_->currentText()).startsWith(QStringLiteral("АРХИВ")))
            dataset_context_->setText(QStringLiteral("АРХИВ RAM • ") + dataset_context_->text());
    }
    if (last_result_.simulation.equity.empty()) {
        result_context_->setText(
            QStringLiteral("Отчёт ещё не рассчитан. Откройте «Стратегия» и нажмите «Рассчитать»."));
        for (auto *card : {return_card_, drawdown_card_, fills_card_, cost_card_})
            card->setText(QStringLiteral("—"));
    } else
        result_context_->setText(
            QStringLiteral(
                "%1\nПоследний расчёт: %2 • %3 свечей. Экспорт содержит параметры именно этого расчёта.")
                .arg(result_current_ ? QStringLiteral("Результат актуален")
                                     : QStringLiteral("Параметры изменены: на экране предыдущий результат"),
                     last_result_.source)
                .arg(last_result_.features.size()));
    if (last_result_.has_holdout)
        result_context_->setText(result_context_->text() + QStringLiteral(
            "\nКарточки и графики — за всю историю. Финальный период сравнивается отдельно в таблице «Финальная проверка» на шаге «Устойчивость»."));
    result_banner_->setText(result_context_->text());
    back_->setEnabled(row > 0);
    const QStringList actions{
        QStringLiteral("Проверить данные →"),       QStringLiteral("Перейти к анализу →"),
        QStringLiteral("Настроить стратегию →"),    QStringLiteral("Рассчитать и открыть результат →"),
        QStringLiteral("Проверить устойчивость →"), QStringLiteral("Открыть виртуальный счёт →"),
        QStringLiteral("Проект и отчёт →"),         QStringLiteral("Вернуться к данным"),
        QStringLiteral("Вернуться к данным")};
    const bool data_ready = !bars.empty() && !source_dirty_;
    const bool needs_data = row >= 1 && row <= 6 && !data_ready;
    const bool needs_result = (row == 4 || row == 5) && !result_current_;
    next_->setVisible(row != 0 || data_ready);
    next_->setText(needs_data ? QStringLiteral("Выбрать данные →")
                  : needs_result ? QStringLiteral("Перейти к стратегии →")
                                 : actions.value(row, QStringLiteral("Далее →")));
    next_->setEnabled(!busy && (needs_data || row != 3 || ready));
    const QStringList hints{
        QStringLiteral("Проверьте, какой период фактически загрузился."),
        QStringLiteral("На следующем шаге откроются графики и показатели."),
        QStringLiteral("Готовые правила можно запустить с настройками по умолчанию."),
        QStringLiteral("Результат покажет доходность, просадку и издержки."),
        QStringLiteral("Сравните доходность с просадкой и простой покупкой."),
        QStringLiteral("Проверка показывает, как результат меняется при других условиях."),
        QStringLiteral("Можно проиграть историю или использовать новые свечи MOEX."),
        QStringLiteral("Сохраните настройки или экспортируйте рассчитанный отчёт."),
        QStringLiteral("Чтобы продолжить исследование, вернитесь к данным.")};
    if (busy || calendar_loading_)
        step_hint_->setText(QStringLiteral("Выполняется операция. При необходимости нажмите «Отменить»."));
    else if (row == 0 && !data_ready)
        step_hint_->setText(QStringLiteral("Загрузите свечи MOEX или откройте ДЕМО выше."));
    else if (needs_data)
        step_hint_->setText(QStringLiteral("Для этого шага сначала нужны свечи выбранной акции."));
    else if (row == 3 && !ready)
        step_hint_->setText(QStringLiteral("Нужно 40 свечей для расчёта или 50 при резерве 20%."));
    else
        step_hint_->setText(hints.value(row));
    if (auto *b = findChild<QPushButton *>("loadMetadata"))
        b->setEnabled(!synthetic_ && !loading_);
}
void ResearchPanel::next_step() {
    const int row = journey_->currentRow();
    if (row >= 1 && row <= 6 && (selected_bars().empty() || source_dirty_)) {
        journey_->setCurrentRow(0);
        return;
    }
    if ((row == 4 || row == 5) && !result_current_) {
        journey_->setCurrentRow(3);
        return;
    }
    if (row == 3) {
        const int before = attempts_;
        show_result_when_ready_ = true;
        calculate();
        if (attempts_ == before)
            show_result_when_ready_ = false;
        return;
    }
    journey_->setCurrentRow(row >= 7 ? 0 : row + 1);
    if (row == 0)
        preflight();
}
void ResearchPanel::show_demo() {
    cancel_work();
    demo();
    tabs_->setCurrentIndex(6);
    update_journey();
}
bool ResearchPanel::journey_test() {
    show_demo();
    update_journey();
    if (journey_->currentRow() != 0 || !next_->isEnabled() || expert_->isChecked())
        return false;
    next_->click();
    if (tabs_->currentIndex() != 4)
        return false;
    next_->click();
    if (tabs_->currentIndex() != 0)
        return false;
    expert_->setChecked(true);
    if (analytics_expert_->isHidden())
        return false;
    expert_->setChecked(false);
    if (!analytics_expert_->isHidden())
        return false;
    tabs_->setCurrentIndex(1);
    entry_rules_[1].enabled->setChecked(false);
    if (entry_rules_[1].lhs->isEnabled())
        return false;
    entry_rules_[1].enabled->setChecked(true);
    if (!entry_rules_[1].lhs->isEnabled())
        return false;
    entry_rules_[1].enabled->setChecked(false);
    tabs_->setCurrentIndex(0);
    calculate_->setFocus();
    const int previous = window_->value();
    QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(window_, &wheel);
    if (window_->value() != previous)
        return false;
    tabs_->setCurrentIndex(6);
    return true;
}
} // namespace live
