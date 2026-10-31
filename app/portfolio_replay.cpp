#include "portfolio_panel.hpp"
namespace live {
void PortfolioPanel::replay_controls() {
    const bool active = static_cast<bool>(replay_);
    account_save_->setEnabled(active && !account_io_ && !worker_->isRunning() && !file_worker_->isRunning());
    account_open_->setEnabled(!working());
    const bool reconnect = active && replay_->live && replay_->restored && !replay_->failed &&
                           !live_feed_->active() && !account_io_;
    account_confirm_->setEnabled(reconnect);
    account_resume_->setEnabled(reconnect && account_confirm_->isChecked());
    const bool live = active && replay_->live;
    const bool more = active && !live && !replay_->failed && replay_->index < replay_->schedule.frames.size();
    replay_start_->setEnabled(!working() && !input_.assets.empty());
    live_start_->setEnabled(!working() && !input_.assets.empty());
    replay_step_->setEnabled(more && !replay_timer_.isActive());
    replay_play_->setEnabled(live ? !replay_->failed && live_feed_->active() : more);
    replay_play_->setText(
        live ? (live_feed_->paused() ? QStringLiteral("Продолжить опрос") : QStringLiteral("Пауза опроса"))
             : (replay_timer_.isActive() ? QStringLiteral("Пауза") : QStringLiteral("Воспроизводить")));
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
    if (replay_->live) {
        replay_status_->setText(
            QStringLiteral(
                "MOEX • ЗАДЕРЖАННЫЙ ВИРТУАЛЬНЫЙ СЧЁТ • новых общих свечей: %1 / 5000. "
                "История использована только для прогрева. Опрос: %2. Входы: %3. Исполнение моделируется по "
                "уже "
                "прошедшему открытию следующего закрытого интервала, это не доступная сейчас цена. "
                "Автоматически счёт не "
                "сохраняется; завершение сеанса не продаёт позиции. Для выхода после стопа нужен новый общий "
                "интервал; на паузе возобновите опрос. Исключённые интервалы в таблице — из прогрева.")
                .arg(replay_->index)
                .arg(live_feed_->paused() ? QStringLiteral("пауза")
                     : replay_->failed    ? QStringLiteral("ошибка")
                                          : QStringLiteral("раз в минуту"))
                .arg(replay_->account->halted() ? QStringLiteral("запрещены")
                                                : QStringLiteral("разрешены моделью")));
        if (!replay_->failed)
            status_->setText(QStringLiteral("Ожидание нового общего закрытого интервала. "
                                            "Защитный буфер публикации — 20 минут, не гарантия максимальной "
                                            "задержки API. Пауза сохраняет счёт и прекращает опрос."));
        if (replay_->restored && !live_feed_->active() && !replay_->failed)
            status_->setText(QStringLiteral(
                "Счёт восстановлен без сети. Источник файла не подтверждён. Проверьте лоты/шаги и отдельно "
                "разрешите подключение MOEX; старше 7 дней поток не возобновляется."));
        replay_controls();
        return;
    }
    replay_status_->setText(
        QStringLiteral("ИСТОРИЯ • %1 / %2 общих свечей • %3. Будущие свечи ещё не переданы счёту. "
                       "Аварийный стоп запрещает входы; продажи ждут следующего шага. "
                       "Завершение сеанса не закрывает позиции и не сохраняет счёт автоматически. "
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
        record_stop("DATA_ERROR");
        replay_controls();
        status_->setText(QStringLiteral("Сеанс приостановлен из-за ошибки обработки. Последний отчёт "
                                        "сохранён на экране; завершите сеанс перед новым запуском."));
    }
}
void PortfolioPanel::replay_stop() {
    if (!replay_)
        return;
    record_stop("MANUAL_STOP");
    replay_present();
}
void PortfolioPanel::replay_end(bool notify) {
    replay_timer_.stop();
    live_feed_->stop();
    account_confirm_->setChecked(false);
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
void PortfolioPanel::live_frames(const std::vector<std::vector<quant::Bar>> &frames) {
    if (!replay_ || !replay_->live || replay_->failed)
        return;
    if (replay_->index + frames.size() > 5000) {
        record_stop("SESSION_LIMIT");
        live_failure(QStringLiteral("Предел 5000 новых общих свечей. Сеанс остановлен; позиции не проданы. "
                                    "Завершите его перед новым запуском."));
        return;
    }
    try {
        for (const auto &frame : frames) {
            replay_->account->step(frame);
            replay_->baseline->step(frame);
            replay_->checkpoint.frames.push_back(frame);
            ++replay_->index;
        }
        replay_present();
    } catch (const std::exception &) {
        live_failure(QStringLiteral("Набор интервалов не принят. Счёт остановлен, позиции не проданы."));
    }
}
void PortfolioPanel::live_failure(const QString &message) {
    if (!replay_ || !replay_->live)
        return;
    live_feed_->stop();
    replay_->failed = true;
    record_stop("DATA_ERROR");
    replay_present();
    status_->setText(message);
}
} // namespace live
