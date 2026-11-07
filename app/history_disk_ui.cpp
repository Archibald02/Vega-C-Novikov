#include "research_panel.hpp"
#include "ui_helpers.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

namespace live {
void ResearchPanel::setup_disk_cache(QVBoxLayout *layout) {
    const auto isolated = QCoreApplication::instance()->property("historyCacheDirectory").toString();
    disk_directory_ = isolated.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("history-cache")
        : isolated;
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    auto *help = new QLabel(QStringLiteral(
        "По умолчанию выключен. Включение показывает сохранённые наборы; запись выполняется только "
        "кнопкой. До 8 наборов, один последний вариант каждого периода, без капитала и токенов. "
        "Открытие не проверяет сеть и не запускает стратегию. Для свежих данных загрузите MOEX заново. "
        "Общая очистка приватных данных удаляет и этот кэш. Выключение само по себе файлы не удаляет."));
    help->setWordWrap(true); help->setTextFormat(Qt::PlainText); l->addWidget(help);
    disk_enabled_ = new QCheckBox(QStringLiteral("Включить локальный дисковый кэш в этой сессии"));
    disk_enabled_->setObjectName("historyDiskEnabled"); l->addWidget(disk_enabled_);
    disk_quota_ = new QSpinBox;
    disk_quota_->setRange(1, 8); disk_quota_->setValue(8); disk_quota_->setSuffix(QStringLiteral(" МиБ"));
    disk_quota_->setAccessibleName(QStringLiteral("Лимит дискового кэша"));
    auto *quota = new QHBoxLayout; quota->addWidget(disk_quota_);
    disk_trim_ = new QPushButton(QStringLiteral("Применить лимит")); quota->addWidget(disk_trim_);
    l->addLayout(quota);
    disk_sets_ = new QComboBox; disk_sets_->setAccessibleName(QStringLiteral("Наборы на диске"));
    l->addWidget(disk_sets_);
    auto *buttons = new QVBoxLayout;
    disk_save_ = new QPushButton(QStringLiteral("Сохранить выбранный RAM-набор на диск"));
    disk_open_ = new QPushButton(QStringLiteral("Открыть с диска без сети"));
    disk_refresh_ = new QPushButton(QStringLiteral("Обновить список"));
    for (auto *b : {disk_save_, disk_open_, disk_refresh_}) buttons->addWidget(b);
    l->addLayout(buttons);
    disk_clear_ = new QPushButton(QStringLiteral("Очистить дисковый кэш")); l->addWidget(disk_clear_);
    disk_status_ = new QLabel(QStringLiteral("Выключен. Папка не читается, свечи не записываются.\n") + disk_directory_);
    disk_status_->setWordWrap(true); disk_status_->setTextFormat(Qt::PlainText); l->addWidget(disk_status_);
    auto *fold = ui::fold(QStringLiteral("Необязательный дисковый кэш"), page);
    fold->findChild<QToolButton *>()->setObjectName("historyDiskToggle"); layout->addWidget(fold);
    disk_worker_ = new QFutureWatcher<DiskReply>(this);
    connect(disk_worker_, &QFutureWatcher<DiskReply>::finished, this, [this] {
        disk_busy_ = false;
        if (disk_clear_pending_) {
            disk_clear_pending_ = false;
            disk_operation(4);
            return;
        }
        const auto result = disk_worker_->result();
        if (!disk_cancel_->load()) {
            const QSignalBlocker blocker(disk_sets_);
            const auto selected = disk_sets_->currentData();
            disk_sets_->clear();
            for (const auto &item : result.catalog.items)
                disk_sets_->addItem(QStringLiteral("%1 • %2—%3 • %4 • %5 свечей")
                    .arg(item.info.symbol, item.info.from.toString(Qt::ISODate),
                         item.info.till.toString(Qt::ISODate), item.info.interval == 60 ? "H1" : "D1")
                    .arg(item.info.bars), item.file);
            if (disk_sets_->findData(selected) >= 0) disk_sets_->setCurrentIndex(disk_sets_->findData(selected));
            disk_status_->setText(result.message + QStringLiteral("\n%1 наборов, %2 МиБ; повреждённых/лишних файлов: %3.\n%4")
                .arg(result.catalog.items.size()).arg(result.catalog.bytes / 1048576.0, 0, 'f', 2)
                .arg(result.catalog.invalid_files).arg(disk_directory_));
            if (result.opened && disk_enabled_->isChecked() && disk_study_generation_ == study_generation_) {
                const auto &set = *result.opened;
                if (set.info.from < from_->minimumDate() || set.info.till > till_->maximumDate()) {
                    disk_status_->setText(QStringLiteral("Диапазон локального набора вне поддерживаемых дат. Текущие данные не заменены."));
                    refresh_disk_controls();
                    return;
                }
                prepare_history_set(set.info.symbol, set.info.from, set.info.till, set.info.interval);
                local_project_ = true;
                if (!loader_.restore_disk(set)) {
                    loading_ = false; load_->setEnabled(true); period_->setEnabled(true);
                    status_->setText(QStringLiteral("Локальные данные не прошли проверку; загрузите историю заново."));
                }
                update_journey();
            }
        } else {
            disk_status_->setText(QStringLiteral("Операция отменена. Выключение не удаляет уже сохранённые файлы.\n") + disk_directory_);
        }
        refresh_disk_controls();
    });
    connect(disk_enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) disk_operation(0);
        else {
            if (disk_cancel_) disk_cancel_->store(true);
            disk_sets_->clear();
            disk_status_->setText(QStringLiteral("Кэш выключен. Уже сохранённые файлы можно удалить отдельной кнопкой.\n") + disk_directory_);
        }
        refresh_disk_controls();
    });
    connect(disk_sets_, &QComboBox::currentIndexChanged, this, [this] { refresh_disk_controls(); });
    connect(disk_save_, &QPushButton::clicked, this, [this] { disk_operation(1); });
    connect(disk_open_, &QPushButton::clicked, this, [this] { disk_operation(2); });
    connect(disk_refresh_, &QPushButton::clicked, this, [this] { disk_operation(0); });
    connect(disk_trim_, &QPushButton::clicked, this, [this] { disk_operation(3); });
    connect(disk_clear_, &QPushButton::clicked, this, [this] { disk_operation(4); });
    refresh_disk_controls();
}
void ResearchPanel::refresh_disk_controls() {
    if (!disk_enabled_) return;
    const bool idle = !disk_busy_;
    const bool enabled = idle && disk_enabled_->isChecked();
    const bool study_idle = !loading_ && !loader_.busy() && (!worker_ || !worker_->isRunning());
    disk_save_->setEnabled(enabled && study_idle && history_sets_->currentIndex() >= 0);
    disk_open_->setEnabled(enabled && study_idle && disk_sets_->currentIndex() >= 0);
    disk_refresh_->setEnabled(enabled); disk_trim_->setEnabled(enabled);
    disk_clear_->setEnabled(idle); disk_quota_->setEnabled(enabled); disk_sets_->setEnabled(enabled);
}
void ResearchPanel::disk_operation(int operation) {
    if (disk_busy_ || (operation != 4 && !disk_enabled_->isChecked())) return;
    std::optional<HistoryDataset> input;
    if (operation == 1) {
        input = loader_.dataset(history_sets_->currentData().toString());
        if (!input) return;
    }
    disk_busy_ = true;
    disk_task_operation_ = operation;
    disk_study_generation_ = study_generation_;
    const auto directory = disk_directory_, file = disk_sets_->currentData().toString();
    const qint64 quota = disk_quota_->value() * HistoryDiskCache::file_limit;
    disk_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = disk_cancel_;
    disk_status_->setText(QStringLiteral("Обработка локального кэша в фоне…"));
    refresh_disk_controls();
    disk_worker_->setFuture(QtConcurrent::run([directory, file, input, quota, cancel, operation] {
        DiskReply reply;
        try {
            if (operation == 1) HistoryDiskCache::write(directory, *input, quota, *cancel);
            if (operation == 2) reply.opened = HistoryDiskCache::read(directory, file, *cancel);
            if (operation == 3) HistoryDiskCache::trim(directory, quota, *cancel);
            if (operation == 4) HistoryDiskCache::clear(directory, *cancel);
            reply.catalog = HistoryDiskCache::catalog(directory, *cancel);
            reply.message = operation == 1 ? QStringLiteral("Набор сохранён. Старейшие файлы вытесняются по лимиту.")
                : operation == 2 ? QStringLiteral("Локальные свечи проверены по схеме и SHA-256. Сеть не проверялась.")
                : operation == 3 ? QStringLiteral("Лимит применён; старейшие файлы удалены при необходимости.")
                : operation == 4 ? QStringLiteral("Дисковый кэш очищен.") : QStringLiteral("Список прочитан с диска.");
        } catch (const std::exception &) {
            reply.opened.reset();
            reply.message = QStringLiteral("Операция не завершена: файл повреждён, кэш занят, превышен лимит или нет доступа к папке. Исследовательские данные не заменены.");
        }
        return reply;
    }));
}
void ResearchPanel::reset_disk_cache() {
    if (!disk_enabled_) return;
    const QSignalBlocker blocker(disk_enabled_);
    disk_enabled_->setChecked(false);
    disk_sets_->clear();
    if (disk_cancel_) disk_cancel_->store(true);
    // A pending writer must finish/cancel before the clearing task starts.
    if (disk_busy_) disk_clear_pending_ = true;
    else disk_operation(4);
}
bool ResearchPanel::disk_cache_test() {
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    disk_directory_ = directory.filePath("cache");
    if (disk_enabled_->isChecked() || QFileInfo::exists(disk_directory_)) return false;
    const auto saved = loader_.dataset(history_sets_->currentData().toString());
    if (!saved) return false;
    const auto ram_count = loader_.datasets().size();
    std::atomic<bool> cancel{false};
    auto wait = [this] {
        QEventLoop loop;
        QTimer deadline, poll;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&poll, &QTimer::timeout, &loop, [&] { if (!disk_busy_) loop.quit(); });
        deadline.start(5000);
        poll.start(10);
        loop.exec();
        return !disk_busy_;
    };
    disk_enabled_->setChecked(true);
    if (!wait() || disk_sets_->count() != 0 || QFileInfo::exists(disk_directory_)) return false;
    if (!disk_save_->isEnabled()) return false;
    disk_save_->click();
    if (!wait() || disk_sets_->count() != 1) return false;
    const auto copy = HistoryDiskCache::read(disk_directory_, disk_sets_->currentData().toString(), cancel);
    if (copy.info.sha256 != saved->info.sha256 || copy.info.fetched_ms != saved->info.fetched_ms) return false;
    disk_quota_->setValue(1);
    disk_trim_->click();
    if (!wait() || disk_sets_->count() != 1) return false;
    disk_open_->click();
    if (!wait() || data_.size() != 1 || selected_bars().size() != saved->bars.size() || !local_project_ ||
        !history_origins_.value(saved->info.symbol).startsWith(QStringLiteral("ДИСК")) ||
        loader_.datasets().size() != ram_count || !loader_.current_info().from_disk || worker_->isRunning())
        return false;
    disk_open_->click();
    clear_private_state();
    if (!wait() || !data_.empty() || disk_enabled_->isChecked() ||
        !HistoryDiskCache::catalog(disk_directory_, cancel).items.isEmpty()) return false;
    return true;
}
} // namespace live
