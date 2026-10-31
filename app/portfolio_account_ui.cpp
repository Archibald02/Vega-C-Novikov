#include "portfolio_panel.hpp"
#include "quote_parser.hpp"
#include <QEventLoop>
#include <QFileDialog>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>
namespace live {
void PortfolioPanel::record_stop(const std::string &reason) {
    if (!replay_ || replay_->account->halted())
        return;
    replay_->account->request_stop(reason);
    replay_->checkpoint.stop_after = static_cast<int>(replay_->index);
    replay_->checkpoint.stop_reason = reason;
}
PortfolioAccountSnapshot PortfolioPanel::account_snapshot() const {
    if (!replay_)
        throw std::invalid_argument("No account");
    auto s = replay_->checkpoint;
    s.index = replay_->index;
    s.failed = replay_->failed;
    s.reference = replay_->account->result();
    s.baseline_reference = replay_->baseline->result();
    return s;
}
void PortfolioPanel::setup_account_files() {
    account_worker_ = new QFutureWatcher<PortfolioAccountFileOutcome>(this);
    connect(account_save_, &QPushButton::clicked, this, [this] { account_file(true); });
    connect(account_open_, &QPushButton::clicked, this, [this] { account_file(false); });
    connect(account_resume_, &QPushButton::clicked, this, [this] { account_resume(); });
    connect(account_confirm_, &QCheckBox::toggled, this, [this] { replay_controls(); });
    connect(account_worker_, &QFutureWatcher<PortfolioAccountFileOutcome>::finished, this, [this] {
        auto out = account_worker_->future().takeResult();
        account_io_ = false;
        busy(false);
        if (cancelled_ && cancelled_->load())
            return;
        if (!out.error.isEmpty()) {
            status_->setText(out.error);
            return;
        }
        if (out.saved) {
            status_->setText(QStringLiteral("Счёт сохранён с ценами и виртуальным капиталом. Сохранённые "
                                            "файлы не удаляются очисткой приложения."));
            return;
        }
        auto restored = std::move(out.restored);
        apply_project(restored->snapshot.project);
        auto state = std::make_shared<PortfolioReplay>();
        state->account = std::move(restored->account);
        state->baseline = std::move(restored->baseline);
        state->schedule = std::move(restored->schedule);
        state->checkpoint = std::move(restored->snapshot);
        state->index = state->checkpoint.index;
        state->live = state->checkpoint.live;
        state->interval = state->checkpoint.interval;
        state->failed = state->checkpoint.failed;
        state->restored = true;
        state->source = QStringLiteral("ЛОКАЛЬНЫЙ ФАЙЛ • происхождение исходных данных не подтверждено • ") +
                        state->checkpoint.project.input.source;
        state->checkpoint.reference = {};
        state->checkpoint.baseline_reference = {};
        replay_ = std::move(state);
        account_confirm_->setChecked(false);
        busy(true);
        replay_present();
        status_->setText(QStringLiteral(
            "Счёт проверен повторным расчётом и открыт на паузе, без сети. SHA-256 не подтверждает автора. "
            "Для новых свечей требуется отдельное разрешение; источник файла остаётся непроверенным."));
    });
}
void PortfolioPanel::account_file(bool save, QString path) {
    if (account_io_ || worker_->isRunning() || file_worker_->isRunning() ||
        (save ? !replay_ : static_cast<bool>(replay_)))
        return;
    if (path.isEmpty()) {
        QFileDialog dialog(this,
                           save ? QStringLiteral("Сохранить свечи, капитал и позиции счёта")
                                : QStringLiteral("Восстановить счёт без сети"),
                           {}, QStringLiteral("Счёт Quant (*.qpa.json)"));
        dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
        dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
        dialog.setDefaultSuffix("qpa.json");
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        path = dialog.selectedFiles().front();
    }
    if (save && !replay_)
        return;
    auto s = save ? account_snapshot() : PortfolioAccountSnapshot{};
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto flag = cancelled_;
    account_io_ = true;
    busy(true);
    status_->setText(QStringLiteral("Проверяем и обрабатываем файл счёта в фоне (до 32 МиБ)…"));
    account_worker_->setFuture(QtConcurrent::run([path, save, s = std::move(s), flag]() mutable {
        PortfolioAccountFileOutcome out;
        try {
            if (save) {
                write_portfolio_account(path, s, *flag);
                out.saved = true;
            } else
                out.restored = std::make_shared<PortfolioAccountRestore>(read_portfolio_account(path, *flag));
        } catch (const std::exception &) {
            out.error = QStringLiteral("Файл счёта не обработан: проверьте доступ к папке, версию/модель, "
                                       "SHA-256, входы и совпадение расчёта. Пределы: 32 МиБ, 6 акций, по "
                                       "5000 свечей прогрева и торговли. Текущее состояние не заменено.");
        }
        return out;
    }));
    replay_controls();
}
void PortfolioPanel::account_resume() {
    if (!replay_ || !replay_->live || !replay_->restored || replay_->failed || account_io_ ||
        live_feed_->active() || !account_confirm_->isChecked())
        return;
    try {
        const auto assets = portfolio_project_assets(replay_->checkpoint.project);
        for (const auto &a : assets)
            if (a.tick_size <= 0 || parse_symbols(QString::fromStdString(a.symbol)) !=
                                        QStringList{QString::fromStdString(a.symbol)})
                throw std::invalid_argument("metadata/symbol");
        if (!replay_->checkpoint.audit)
            throw std::invalid_argument("audit");
        auto buffer = std::make_unique<quant::PortfolioStreamBuffer>(
            quant::PortfolioStreamBuffer::restore(*replay_->checkpoint.audit));
        live_feed_->start(std::move(buffer), replay_->interval);
        replay_present();
    } catch (const std::exception &) {
        status_->setText(QStringLiteral("Подключение недоступно: проверьте символы TQBR, положительные "
                                        "лоты/шаги и сохранённое окно контроля данных."));
    }
}
bool PortfolioPanel::account_self_test() {
    clear_private_state();
    demo();
    auto wait = [this](auto *watcher) {
        QEventLoop loop;
        const auto c = connect(watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(c);
        return !watcher->isRunning();
    };
    replay_start_->click();
    if (!wait(worker_) || !replay_)
        return false;
    replay_step_->click();
    replay_step_->click();
    replay_stop_->click();
    const auto cash = replay_->account->result().cash;
    const auto count = replay_->index;
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    auto path = dir.filePath("account.qpa.json");
    account_file(true, path);
    if (!wait(account_worker_) || !replay_ || capital_->isEnabled() ||
        !status_->text().contains(QStringLiteral("сохранён")))
        return false;
    replay_end();
    account_file(false, path);
    if (!wait(account_worker_) || !replay_ || replay_->index != count || !replay_->account->halted() ||
        replay_->account->result().cash != cash || replay_timer_.isActive() || live_feed_->active() ||
        account_confirm_->isChecked())
        return false;
    account_resume();
    if (live_feed_->active())
        return false;
    replay_step_->click();
    for (const auto &p : replay_->account->result().positions)
        if (p.units)
            return false;
    replay_end();
    account_file(false, dir.filePath("absent"));
    if (!wait(account_worker_) || replay_)
        return false;
    account_file(false, path);
    clear_private_state();
    if (!wait(account_worker_) || replay_ || !input_.assets.empty() || positions_->rowCount())
        return false;
    demo();
    auto project_copy = project();
    project_copy.input.synthetic = false;
    const auto old_start = QDateTime::currentMSecsSinceEpoch() - 22 * 86400000LL;
    for (auto &a : project_copy.input.assets)
        for (std::size_t i = 0; i < a.bars.size(); ++i) {
            auto &b = a.bars[i];
            b.time = old_start + static_cast<qint64>(i) * 3600000;
            b.end = b.time + 3600000;
            b.utc_offset_seconds =
                QDateTime::fromMSecsSinceEpoch(b.time, QTimeZone("Europe/Moscow")).offsetFromUtc();
        }
    auto assets = portfolio_project_assets(project_copy), seed = assets;
    for (auto &a : seed)
        a.bars.resize(40);
    auto schedule = quant::portfolio_schedule(seed);
    auto state = std::make_shared<PortfolioReplay>();
    state->live = true;
    state->interval = 60;
    state->account = std::make_unique<quant::PortfolioAccount>(assets, project_copy.config);
    state->baseline = std::make_unique<quant::PortfolioAccount>(
        quant::portfolio_baseline_account(assets, project_copy.config));
    for (const auto &f : schedule.frames) {
        state->account->warmup(f);
        state->baseline->warmup(f);
    }
    state->account->arm_from_warmup();
    state->baseline->arm_from_warmup();
    state->schedule.excluded = schedule.excluded;
    state->checkpoint.project = project_copy;
    state->checkpoint.live = true;
    state->checkpoint.interval = 60;
    state->checkpoint.warmup_cutoff = state->checkpoint.warmup_end = schedule.frames.back()[0].end;
    state->checkpoint.excluded = schedule.excluded;
    quant::PortfolioStreamBuffer buffer(seed, schedule.frames.back()[0].time, schedule.frames.back()[0].end,
                                        old_start, 60);
    std::vector<std::vector<quant::Bar>> cycle;
    for (const auto &a : assets)
        cycle.push_back(std::vector<quant::Bar>(a.bars.begin(), a.bars.begin() + 42));
    auto accepted = buffer.merge(cycle, old_start, cycle[0].back().end);
    replay_ = std::move(state);
    busy(true);
    live_frames(accepted);
    replay_->checkpoint.audit = buffer.snapshot();
    account_file(true, path);
    if (!wait(account_worker_) || !status_->text().contains(QStringLiteral("сохранён")))
        return false;
    const auto units = replay_->account->result().positions[0].units;
    replay_end();
    account_file(false, path);
    if (!wait(account_worker_) || !replay_ || !replay_->live || live_feed_->active() ||
        account_confirm_->isChecked() || account_resume_->isEnabled())
        return false;
    account_resume();
    if (live_feed_->active())
        return false;
    account_confirm_->setChecked(true);
    if (!account_resume_->isEnabled())
        return false;
    account_resume_->click(); // stale guard rejects before any HTTP request
    if (live_feed_->active() || !replay_->failed || replay_->account->result().positions[0].units != units)
        return false;
    clear_private_state();
    return true;
}
} // namespace live
