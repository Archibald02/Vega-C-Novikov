#include "portfolio_panel.hpp"
namespace live {
void PortfolioPanel::replay_controls() {
    const bool active = static_cast<bool>(replay_);
    const bool more = active && !replay_->failed && replay_->index < replay_->schedule.frames.size();
    replay_start_->setEnabled(!working() && !input_.assets.empty());
    replay_step_->setEnabled(more && !replay_timer_.isActive());
    replay_play_->setEnabled(more);
    replay_play_->setText(replay_timer_.isActive() ? QStringLiteral("Пауза")
                                                   : QStringLiteral("Воспроизводить"));
    replay_stop_->setEnabled(active && !replay_->account->halted());
    replay_end_->setEnabled(active);
}
void PortfolioPanel::replay_present() {
    if (!replay_)
        return;
    if (replay_->index) {
        PortfolioOutcome out;
        out.result = replay_->account->result();
        out.baseline = replay_->baseline->result();
        out.result.excluded = replay_->schedule.excluded;
        out.source = replay_->source;
        present(out);
    }
    replay_status_->setText(
        QStringLiteral("ИСТОРИЯ • %1 / %2 общих свечей • %3. Будущие свечи ещё не переданы счёту. "
                       "Аварийный стоп запрещает входы; продажи ждут следующего шага. "
                       "Завершение сеанса не закрывает позиции и не сохраняет счёт. "
                       "Число исключённых интервалов относится ко всему загруженному набору.")
            .arg(replay_->index)
            .arg(replay_->schedule.frames.size())
            .arg(replay_->account->halted() ? QStringLiteral("входы остановлены")
                                            : QStringLiteral("счёт активен")));
    status_->setText(
        QStringLiteral("Пошаговый виртуальный счёт по архиву. Новые данные MOEX не запрашиваются."));
    replay_controls();
}
void PortfolioPanel::replay_step() {
    if (!replay_ || replay_->failed || replay_->index >= replay_->schedule.frames.size())
        return;
    try {
        const auto &frame = replay_->schedule.frames[replay_->index];
        replay_->account->step(frame);
        replay_->baseline->step(frame);
        ++replay_->index;
        if (replay_->index == replay_->schedule.frames.size())
            replay_timer_.stop();
        replay_present();
    } catch (const std::exception &) {
        replay_timer_.stop();
        replay_->failed = true;
        // The full schedule was validated before activation. Preserve the account for inspection.
        replay_->account->request_stop();
        replay_controls();
        status_->setText(QStringLiteral("Сеанс приостановлен из-за ошибки обработки. Последний отчёт "
                                        "сохранён на экране; завершите сеанс перед новым запуском."));
    }
}
void PortfolioPanel::replay_stop() {
    if (!replay_)
        return;
    replay_->account->request_stop();
    replay_present();
}
void PortfolioPanel::replay_end(bool notify) {
    replay_timer_.stop();
    const bool had_session = static_cast<bool>(replay_);
    replay_.reset();
    if (!worker_->isRunning() && !file_worker_->isRunning())
        busy(false);
    replay_controls();
    if (had_session && notify) {
        replay_status_->setText(QStringLiteral("Сеанс завершён. На экране последняя оценка; позиции не "
                                               "продавались. Состояние счёта удалено из RAM."));
        status_->setText(QStringLiteral("Можно изменить настройки и начать новый независимый сеанс."));
    } else if (!notify) {
        replay_status_->clear();
    }
}
} // namespace live
