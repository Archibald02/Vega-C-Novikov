#include "research_panel.hpp"
#include "catalog_dialog.hpp"
#include "quote_parser.hpp"
#include "ui_helpers.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMap>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
namespace live {
namespace {
QString fmt(double v, int precision = 4) {
    return std::isfinite(v) ? QString::number(v, 'f', precision) : QStringLiteral("—");
}
QLabel *text(const QString &s) {
    auto *w = new QLabel(s);
    w->setTextFormat(Qt::PlainText);
    w->setWordWrap(true);
    return w;
}
QDoubleSpinBox *number(double value, double minimum, double maximum, int decimals = 3) {
    auto *w = new QDoubleSpinBox;
    w->setRange(minimum, maximum);
    w->setDecimals(decimals);
    w->setValue(value);
    return w;
}
QSpinBox *integer(int value, int minimum, int maximum) {
    auto *w = new QSpinBox;
    w->setRange(minimum, maximum);
    w->setValue(value);
    return w;
}
QStringList feature_names() {
    return {QStringLiteral("Число"),
            QStringLiteral("Цена закрытия"),
            QStringLiteral("Быстрая средняя SMA"),
            QStringLiteral("Медленная средняя SMA"),
            "EMA",
            "RSI",
            "MACD",
            QStringLiteral("Сигнал MACD"),
            QStringLiteral("Bollinger верхняя"),
            QStringLiteral("Bollinger нижняя"),
            "ATR 14",
            "NATR 14",
            "Z-score",
            QStringLiteral("Объём")};
}
QString stamp(std::int64_t time) {
    return QDateTime::fromMSecsSinceEpoch(time)
        .toTimeZone(QTimeZone("Europe/Moscow"))
        .toString("dd.MM.yyyy HH:mm");
}
void cells(QTableWidget *table, const QVector<QStringList> &rows) {
    table->setUpdatesEnabled(false);
    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        for (int j = 0; j < rows[i].size(); ++j)
            table->setItem(i, j, new QTableWidgetItem(rows[i][j]));
    table->setUpdatesEnabled(true);
}
QTableWidget *table(const QStringList &columns) {
    auto *w = new QTableWidget(0, columns.size());
    w->setHorizontalHeaderLabels(columns);
    w->setEditTriggers(QAbstractItemView::NoEditTriggers);
    w->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    w->horizontalHeader()->setStretchLastSection(true);
    w->setMinimumHeight(130);
    return w;
}
QVector<QPointF> points(const std::vector<double> &values, double factor = 1) {
    QVector<QPointF> result;
    const auto step = std::max<std::size_t>(1, (values.size() + 498) / 499);
    for (std::size_t i = 0; i < values.size(); i += step)
        result.push_back({static_cast<double>(i), values[i] * factor});
    if (!values.empty() && (result.isEmpty() || result.back().x() != values.size() - 1)) {
        if (result.size() >= 500)
            result.removeLast();
        result.push_back({static_cast<double>(values.size() - 1), values.back() * factor});
    }
    return result;
}
QString reason(const std::string &s) {
    const QString v = QString::fromStdString(s);
    if (v.startsWith("PARTIAL SELL"))
        return QStringLiteral("Плановый частичный выход на открытии");
    if (v.startsWith("SIGNAL: partial"))
        return QStringLiteral("Рост закрытия достиг этапа; назначен частичный выход");
    if (v.startsWith("partial exit pending"))
        return QStringLiteral("Частичный выход ожидает предыдущего доступного объёма");
    if (v.startsWith("partial exit skipped"))
        return QStringLiteral("Частичный этап пропущен: нет полного лота для продажи с сохранением остатка");
    if (v.startsWith("BUY"))
        return QStringLiteral("Покупка на следующем открытии; остаток отменён");
    if (v.startsWith("SELL"))
        return QStringLiteral("Выход по сигналу на следующем открытии");
    if (v.startsWith("SIGNAL: entry"))
        return QStringLiteral("Условия входа выполнены; ждём следующую свечу");
    if (v.startsWith("SIGNAL: exit"))
        return QStringLiteral("Условия выхода выполнены; ждём следующую свечу");
    if (v.startsWith("SIGNAL: time exit"))
        return QStringLiteral("Срок удержания достигнут; выход на следующем открытии");
    if (v.startsWith("time exit"))
        return QStringLiteral("Выход по сроку удержания на открытии");
    if (v.startsWith("both touched: stop"))
        return QStringLiteral("Стоп и цель в одной свече: сначала стоп");
    if (v.startsWith("both touched"))
        return QStringLiteral("Стоп и цель в одной свече: сначала цель");
    if (v.startsWith("stop gap"))
        return QStringLiteral("Гэп через стоп: цена открытия");
    if (v.startsWith("target gap"))
        return QStringLiteral("Гэп через цель: рыночный выход на открытии");
    if (v.startsWith("stop touched"))
        return QStringLiteral("Достигнут стоп по модели свечи");
    if (v.startsWith("target touched"))
        return QStringLiteral("Достигнута цель по модели свечи");
    if (v.startsWith("RISK"))
        return QStringLiteral("Лимит риска: запрет новых входов, выход на следующем открытии");
    if (v.startsWith("risk/manual"))
        return QStringLiteral("Выход после остановки по риску/пользователем");
    if (v.startsWith("entry rejected"))
        return QStringLiteral("Вход отклонён: капитал, лот, риск или лимит объёма");
    if (v.startsWith("exit pending"))
        return QStringLiteral("Выход отложен из-за лимита объёма");
    if (v.startsWith("end of"))
        return QStringLiteral("Завершение теста: ликвидация на последнем закрытии");
    return v;
}
void journal_rows(QTableWidget *table, const quant::SimResult &result) {
    const auto total = result.journal.size();
    const auto old = static_cast<std::size_t>(table->property("journal_total").toULongLong());
    auto signature = [](const quant::JournalEntry &e) {
        return QString::number(e.time) + ":" + QString::number(e.quantity) + ":" +
               QString::fromStdString(e.reason) + ":" + QString::number(e.price, 'g', 17);
    };
    const bool continuation =
        old > 0 && old <= total && table->rowCount() > 0 &&
        table->property("journal_last").toString() == signature(result.journal[old - 1]);
    if (continuation && old == total)
        return;
    table->setUpdatesEnabled(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->setColumnWidth(0, 160);
    table->setColumnWidth(1, 90);
    table->setColumnWidth(2, 100);
    table->setColumnWidth(3, 120);
    table->setWordWrap(false);
    const auto first = total > 500 ? total - 500 : 0;
    std::size_t begin = first;
    if (continuation) {
        begin = std::max(old, first);
        const auto append = total - begin;
        while (table->rowCount() + append > 500)
            table->removeRow(0);
    } else
        table->setRowCount(0);
    for (std::size_t i = begin; i < total; ++i) {
        const auto &j = result.journal[i];
        const QStringList row{stamp(j.time), QString::number(j.quantity), fmt(j.price, 2), fmt(j.cash, 2),
                              reason(j.reason)};
        const int index = table->rowCount();
        table->insertRow(index);
        for (int k = 0; k < row.size(); ++k) {
            auto *item = new QTableWidgetItem(row[k]);
            item->setToolTip(row[k]);
            table->setItem(index, k, item);
        }
    }
    table->setProperty("journal_total", QVariant::fromValue<qulonglong>(total));
    table->setProperty("journal_last", total ? signature(result.journal.back()) : QString());
    table->setUpdatesEnabled(true);
}
} // namespace
QString execution_gate(const std::string &gate) {
    static const QMap<QString, QString> gates{
        {"higher entry blocked", QStringLiteral("Вход не подтверждён закрытым старшим блоком")},
        {"higher exit blocked",
         QStringLiteral("Обычный выход не подтверждён старшим блоком; стоп и риск активны")},
        {"partial pending", QStringLiteral("Назначен частичный выход; оставшаяся позиция сохраняется")},
        {"halt", QStringLiteral("Аварийная остановка; новые входы запрещены")},
        {"session halt", QStringLiteral("Лимит убытка сессии; новые входы запрещены")},
        {"entry pending", QStringLiteral("Сигнал входа подтверждён, исполнение ожидает открытия")},
        {"exit pending", QStringLiteral("Выход ожидает открытия или доступного объёма")},
        {"holding", QStringLiteral("Позиция удерживается")},
        {"cooldown", QStringLiteral("Вход запрещён паузой после закрытия позиции")},
        {"entry limit", QStringLiteral("Достигнут лимит входов за сессию")},
        {"confirmation", QStringLiteral("Условие есть, подтверждений пока недостаточно")},
        {"no entry condition", QStringLiteral("Условие входа не выполнено или не прогрето")},
        {"predefined end", QStringLiteral("Завершение заранее заданного теста")}};
    return gates.value(QString::fromStdString(gate), QString::fromStdString(gate));
}
QString execution_reason(const std::string &code) { return reason(code); }
ResearchPanel::ResearchPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *heading = new QHBoxLayout;
    step_title_ = text(QStringLiteral("1. Загрузите данные"));
    step_title_->setStyleSheet("font-size:21px;font-weight:600;");
    heading->addWidget(step_title_, 1);
    calculate_ = new QPushButton(QStringLiteral("Рассчитать"));
    calculate_->setObjectName("connectButton");
    calculate_->setShortcut(QKeySequence("Ctrl+Return"));
    cancel_button_ = new QPushButton(QStringLiteral("Отменить загрузку / расчёт"));
    auto *cancel = cancel_button_;
    heading->addWidget(calculate_);
    heading->addWidget(cancel);
    layout->addLayout(heading);
    dataset_context_ = text(QStringLiteral("Данные ещё не выбраны"));
    dataset_context_->setStyleSheet("background:#132b3d;padding:9px;border-radius:5px;");
    layout->addWidget(dataset_context_);
    status_ = text(QStringLiteral("Начните с ДЕМО или загрузите закрытые свечи MOEX. Подключение потока "
                                  "котировок для этого не требуется."));
    layout->addWidget(status_);
    auto *body = new QHBoxLayout;
    journey_ = new QListWidget;
    journey_->setObjectName("researchJourney");
    journey_->setFixedWidth(182);
    journey_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QStringList steps{QStringLiteral("1  Данные"),           QStringLiteral("2  Качество"),
                            QStringLiteral("3  Анализ"),           QStringLiteral("4  Стратегия"),
                            QStringLiteral("5  Результат"),        QStringLiteral("6  Устойчивость"),
                            QStringLiteral("7  Виртуальный счёт"), QStringLiteral("Проект и отчёт"),
                            QStringLiteral("Справка и термины")};
    const int targets[]{6, 4, 0, 1, 2, 5, 3, 7, 8};
    for (int i = 0; i < steps.size(); ++i) {
        auto *item = new QListWidgetItem(steps[i], journey_);
        item->setData(Qt::UserRole, targets[i]);
    }
    body->addWidget(journey_);
    tabs_ = new QTabWidget;
    tabs_->tabBar()->hide();
    body->addWidget(tabs_, 1);
    layout->addLayout(body, 1);
    data_page_ = new QWidget;
    data_layout_ = new QVBoxLayout(data_page_);
    data_layout_->setSpacing(12);
    data_layout_->addWidget(text(QStringLiteral(
        "Здесь выбирается история для исследования. Она не зависит от таблицы текущих котировок.")));
    symbols_ = new QLineEdit("SBER, GAZP, LKOH");
    symbols_->setMaxLength(256);
    symbols_->setPlaceholderText(QStringLiteral("Например: SBER, GAZP, LKOH"));
    symbols_->setToolTip(
        QStringLiteral("До 6 тикеров MOEX TQBR, через запятую. Для сравнения нужны совпадающие интервалы."));
    period_ = new QComboBox;
    period_->addItems({QStringLiteral("Дневные свечи • 90 дней"), QStringLiteral("Дневные свечи • 1 год"),
                       QStringLiteral("Часовые свечи • 7 дней")});
    period_->setCurrentIndex(1);
    auto *data_form = new QFormLayout;
    auto *ticker_row = new QHBoxLayout;
    auto *catalog_button = new QPushButton(QStringLiteral("Выбрать из списка…"));
    ticker_row->addWidget(symbols_, 1);
    ticker_row->addWidget(catalog_button);
    data_form->addRow(QStringLiteral("Акции для загрузки"), ticker_row);
    connect(catalog_button, &QPushButton::clicked, this, [this] {
        QStringList selected;
        try {
            selected = parse_symbols(symbols_->text());
        } catch (...) {
        }
        CatalogDialog dialog(catalog_, selected, this, 6);
        if (dialog.exec() == QDialog::Accepted) {
            cancel_work();
            symbols_->setText(dialog.selection().join(", "));
            source_dirty_ = !data_.empty();
            result_current_ = false;
            update_journey();
        }
    });
    data_form->addRow(QStringLiteral("Период и размер свечи"), period_);
    data_layout_->addLayout(data_form);
    custom_dates_ = new QCheckBox(QStringLiteral("Выбрать свои даты"));
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    from_ = new QDateEdit(today.addYears(-3));
    till_ = new QDateEdit(today);
    for (auto *w : {from_, till_}) {
        w->setCalendarPopup(true);
        w->setDisplayFormat("dd.MM.yyyy");
        w->setDateRange(QDate(1990, 1, 1), today);
        w->setEnabled(false);
    }
    auto *dates = new QFormLayout;
    dates->addRow(custom_dates_);
    dates->addRow(QStringLiteral("С даты"), from_);
    dates->addRow(QStringLiteral("По дату включительно"), till_);
    data_layout_->addLayout(dates);
    dates->setRowVisible(from_, false);
    dates->setRowVisible(till_, false);
    connect(custom_dates_, &QCheckBox::toggled, this, [this, dates](bool on) {
        from_->setEnabled(on);
        till_->setEnabled(on);
        dates->setRowVisible(from_, on);
        dates->setRowVisible(till_, on);
    });
    auto *actions = new QHBoxLayout;
    load_ = new QPushButton(QStringLiteral("Загрузить свечи MOEX"));
    load_->setObjectName("connectButton");
    auto *demo_button = new QPushButton(QStringLiteral("Попробовать ДЕМО без сети"));
    actions->addWidget(load_);
    actions->addWidget(demo_button);
    actions->addStretch();
    data_layout_->addLayout(actions);
    data_layout_->addWidget(
        text(QStringLiteral("MOEX — официальные данные с задержкой, без регистрации. ДЕМО — вымышленные "
                            "цены. До 6 акций × 5000 свечей; незавершённые свечи исключаются.")));
    selected_ = new QComboBox;
    selected_->setPlaceholderText(QStringLiteral("Сначала загрузите историю"));
    selected_->setAccessibleName(QStringLiteral("Акция для анализа"));
    auto *selected_row = new QFormLayout;
    selected_row->addRow(QStringLiteral("Анализировать акцию"), selected_);
    data_layout_->addLayout(selected_row);
    window_ = integer(20, 3, 200);
    lags_ = integer(20, 1, 50);
    adf_lags_ = integer(2, 0, 10);
    kpss_lags_ = integer(4, 0, 50);
    expert_controls_ = new QWidget;
    auto *expert_form = new QFormLayout(expert_controls_);
    expert_form->addRow(QStringLiteral("Лагов ACF/PACF"), lags_);
    expert_form->addRow(QStringLiteral("Лагов ADF"), adf_lags_);
    expert_form->addRow(QStringLiteral("Полоса KPSS"), kpss_lags_);
    tabs_->addTab(analytics_page(), QStringLiteral("Анализ"));
    tabs_->addTab(strategy_page(), QStringLiteral("Стратегия"));
    tabs_->addTab(report_page(), QStringLiteral("Результат"));
    tabs_->addTab(paper_page(), QStringLiteral("Виртуальный счёт"));
    auto *quality_page = new QWidget;
    quality_layout_ = new QVBoxLayout(quality_page);
    quality_ = text(QStringLiteral("Сначала загрузите историю на шаге «Данные»."));
    live_quality_ = text(QStringLiteral(
        "Поток котировок — отдельный источник, подключать его для исследования не требуется."));
    quality_layout_->addWidget(quality_);
    auto *technical = new QWidget;
    auto *technical_layout = new QVBoxLayout(technical);
    technical_layout->addWidget(live_quality_);
    technical_layout->addWidget(
        text(QStringLiteral("Расчёты используют UTC, даты отображаются по Москве с историческим смещением. "
                            "Отсутствующие цены не подставляются. Доходности сопоставляются по одинаковым "
                            "интервалам. Цены без поправок на дивиденды/сплиты. Возраст дневной свечи может "
                            "увеличиваться в выходные; это само по себе не ошибка.")));
    quality_layout_->addWidget(ui::fold(QStringLiteral("Как проверяются время и интервалы"), technical));
    tabs_->addTab(quality_page, QStringLiteral("Качество"));
    tabs_->addTab(workbench_page(), QStringLiteral("Устойчивость"));
    tabs_->addTab(data_page_, QStringLiteral("Данные"));
    tabs_->addTab(project_page_, QStringLiteral("Проект и отчёт"));
    tabs_->addTab(about_page_, QStringLiteral("Справка"));
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *page = tabs_->widget(i);
        const auto title = tabs_->tabText(i);
        tabs_->removeTab(i);
        auto *scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs_->insertTab(i, scroll, title);
    }
    setup_workbench();
    heading->insertWidget(1, expert_);
    data_layout_->addStretch();
    auto *footer = new QHBoxLayout;
    back_ = new QPushButton(QStringLiteral("← Назад"));
    next_ = new QPushButton(QStringLiteral("Далее →"));
    next_->setObjectName("connectButton");
    step_hint_ = text(QString());
    footer->addWidget(back_);
    footer->addWidget(step_hint_, 1);
    footer->addWidget(next_);
    layout->addLayout(footer);
    connect(journey_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            tabs_->setCurrentIndex(journey_->item(row)->data(Qt::UserRole).toInt());
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { update_journey(); });
    connect(next_, &QPushButton::clicked, this, &ResearchPanel::next_step);
    connect(back_, &QPushButton::clicked, this, [this] {
        const int row = journey_->currentRow();
        if (row > 0)
            journey_->setCurrentRow(row - 1);
    });
    tabs_->setCurrentIndex(6);
    journey_->setCurrentRow(0);
    worker_ = new QFutureWatcher<LabResult>(this);
    connect(worker_, &QFutureWatcher<LabResult>::finished, this, [this] {
        calculate_->setEnabled(true);
        update_journey();
        if (cancel_ && cancel_->load())
            return;
        const auto result = worker_->result();
        if (!result.error.isEmpty()) {
            show_result_when_ready_ = false;
            status_->setText(result.error);
            return;
        }
        last_result_ = result;
        present(result);
        record_run(result);
        result_current_ = true;
        if (show_result_when_ready_) {
            show_result_when_ready_ = false;
            tabs_->setCurrentIndex(2);
        }
        update_journey();
        status_->setText(synthetic_
                             ? QStringLiteral("ДЕМО • расчёт завершён, все цены синтетические")
                             : QStringLiteral("MOEX ISS • расчёт закрытых задержанных свечей завершён"));
    });
    connect(load_, &QPushButton::clicked, this, [this] {
        cancel_work();
        data_.clear();
        result_current_ = false;
        diagnostics_.clear();
        selected_->clear();
        try {
            requested_ = parse_symbols(symbols_->text());
            if (requested_.size() > 6)
                throw std::invalid_argument("Six");
        } catch (...) {
            status_->setText(QStringLiteral("Введите от 1 до 6 тикеров."));
            return;
        }
        synthetic_ = false;
        local_project_ = false;
        loading_ = true;
        load_->setEnabled(false);
        period_->setEnabled(false);
        request_index_ = 0;
        update_journey();
        load_next();
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        cancel_work();
        status_->setText(
            QStringLiteral("Работа отменена. Ранее показанные результаты относятся к предыдущему расчёту."));
    });
    connect(demo_button, &QPushButton::clicked, this, [this] {
        cancel_work();
        demo();
    });
    connect(calculate_, &QPushButton::clicked, this, &ResearchPanel::calculate);
    connect(selected_, &QComboBox::currentTextChanged, this, [this] {
        if (!loading_) {
            cancel_work();
            result_current_ = false;
            update_journey();
        }
    });
    connect(&loader_, &MarketHistory::loaded, this,
            [this](const QString &symbol, int preset, const QVector<Candle> &bars, bool) {
                if (!loading_ || request_index_ >= requested_.size() || symbol != requested_[request_index_])
                    return;
                std::vector<quant::Bar> accepted;
                int incomplete = 0, gaps = 0;
                const auto now = QDateTime::currentDateTimeUtc();
                const auto today = now.toTimeZone(QTimeZone("Europe/Moscow")).date();
                for (const auto &b : bars) {
                    const auto start =
                        QDateTime::fromMSecsSinceEpoch(b.time_ms).toTimeZone(QTimeZone("Europe/Moscow"));
                    const auto end = preset == 2 ? b.time_ms + 3600000 : start.addDays(1).toMSecsSinceEpoch();
                    if ((preset != 2 && start.date() >= today) || end > now.toMSecsSinceEpoch()) {
                        ++incomplete;
                        continue;
                    }
                    if (!accepted.empty() &&
                        b.time_ms - accepted.back().time > (preset == 2 ? 3600000 : 86400000))
                        ++gaps;
                    accepted.push_back(
                        {b.time_ms, end, b.open, b.high, b.low, b.close, b.volume, start.offsetFromUtc()});
                }
                data_[symbol] = std::move(accepted);
                diagnostics_ << QStringLiteral(
                                    "%1: принято %2, незавершённых исключено %3, календарных разрывов %4")
                                    .arg(symbol)
                                    .arg(data_[symbol].size())
                                    .arg(incomplete)
                                    .arg(gaps);
                if (!data_[symbol].empty())
                    diagnostics_ << QStringLiteral("%1: последнее завершение %2 МСК; возраст %3 ч")
                                        .arg(symbol, stamp(data_[symbol].back().end))
                                        .arg(std::max<qint64>(0, now.toMSecsSinceEpoch() -
                                                                     data_[symbol].back().end) /
                                                 3600000.0,
                                             0, 'f', 1);
                ++request_index_;
                load_next();
            });
    connect(&loader_, &MarketHistory::failed, this, [this](const QString &error) {
        if (!loading_)
            return;
        diagnostics_ << requested_.value(request_index_) + ": " + error;
        ++request_index_;
        load_next();
    });
    auto dirty = [this] {
        result_current_ = false;
        show_result_when_ready_ = false;
        update_journey();
        if (cancel_)
            cancel_->store(true);
        if (!data_.empty())
            status_->setText(QStringLiteral(
                "Параметры изменены. Показаны результаты предыдущего расчёта; нажмите расчёт заново."));
    };
    for (auto *w : findChildren<QSpinBox *>())
        connect(w, &QSpinBox::valueChanged, this, dirty);
    for (auto *w : findChildren<QDoubleSpinBox *>())
        connect(w, &QDoubleSpinBox::valueChanged, this, dirty);
    for (auto *w : findChildren<QCheckBox *>())
        if (w != expert_ && w != save_cash_ && w != save_account_)
            connect(w, &QCheckBox::toggled, this, dirty);
    for (auto *w : findChildren<QComboBox *>())
        if (w != selected_ && w != period_ && w != distribution_mode_)
            connect(w, &QComboBox::currentIndexChanged, this, dirty);
    auto source_changed = [this, dirty] {
        source_dirty_ = !data_.empty();
        dirty();
        status_->setText(QStringLiteral("Параметры загрузки изменены. Нажмите «Загрузить свечи MOEX», чтобы "
                                        "использовать новый диапазон."));
    };
    connect(from_, &QDateEdit::dateChanged, this, [source_changed] { source_changed(); });
    connect(till_, &QDateEdit::dateChanged, this, [source_changed] { source_changed(); });
    connect(symbols_, &QLineEdit::textEdited, this, [source_changed] { source_changed(); });
    connect(custom_dates_, &QCheckBox::toggled, this, [source_changed] { source_changed(); });
    connect(period_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_) {
            cancel_work();
            data_.clear();
            selected_->clear();
            status_->setText(QStringLiteral("Период изменён. Загрузите историю заново."));
        }
    });
    connect(
        &loader_, &MarketHistory::quality_report, this,
        [this](const QString &symbol, int received, int rejected) {
            if (loading_)
                diagnostics_ << QStringLiteral(
                                    "%1: строк API %2; отклонено по типам/OHLC/датам/дубликатам/лимиту %3")
                                    .arg(symbol)
                                    .arg(received)
                                    .arg(rejected);
        });
    paper_timer_.setInterval(500);
    connect(&paper_timer_, &QTimer::timeout, this, &ResearchPanel::step_paper);
    ui::prepare_controls(this);
    expert_->setChecked(false);
    analytics_expert_->setVisible(false);
    update_journey();
}
ResearchPanel::~ResearchPanel() { cancel_work(); }
void ResearchPanel::set_symbol(const QString &symbol) {
    if (!symbol.isEmpty() && !loading_ && !worker_->isRunning()) {
        const int index = selected_->findText(symbol);
        if (index >= 0)
            selected_->setCurrentIndex(index);
        else if (symbols_->text().trimmed().isEmpty())
            symbols_->setText(symbol);
    }
}
void ResearchPanel::set_feed_quality(const QString &value) {
    if (live_quality_->text() != value)
        live_quality_->setText(value);
}
void ResearchPanel::cancel_work() {
    if (snapshot_cancel_)
        snapshot_cancel_->store(true);
    if (cancel_)
        cancel_->store(true);
    loader_.cancel();
    loading_ = false;
    load_->setEnabled(true);
    period_->setEnabled(true);
    paper_timer_.stop();
    if (paper_play_) {
        paper_play_->setChecked(false);
        paper_play_->setText(QStringLiteral("Запустить автоматически"));
    }
    show_result_when_ready_ = false;
    live_timer_.stop();
    live_loader_.cancel();
    live_paper_ = false;
    reference_loader_.cancel();
    ++study_generation_;
    if (dividend_reply_)
        dividend_reply_->abort();
    update_journey();
}
void ResearchPanel::clear_private_state() {
    cancel_work();
    data_.clear();
    runs_.clear();
    if (experiment_detail_)
        experiment_detail_->clear();
    runs_table_->setRowCount(0);
    runs_chart_->clear();
    holdout_used_.clear();
    attempts_ = 0;
    events_->setRowCount(0);
    reference_status_->clear();
    holdout_status_->clear();
    preflight_->clear();
    save_account_->setChecked(false);
    save_cash_->setChecked(false);
    paper_.reset();
    paper_bars_.clear();
    selected_->clear();
    last_result_ = LabResult{};
    result_current_ = false;
    source_dirty_ = false;
    cash_->setValue(100000);
    allocation_->setValue(20);
    stop_->setValue(2);
    target_->setValue(4);
    risk_->setValue(1);
    max_dd_->setValue(20);
    daily_->setValue(5);
    annual_->setValue(0);
    fixed_->setValue(0);
    commission_->setValue(.05);
    spread_->setValue(2);
    slippage_->setValue(2);
    participation_->setValue(0);
    symbols_->setText("SBER, GAZP, LKOH");
    paper_source_.clear();
    paper_parameters_ = {};
    live_symbol_.clear();
    live_last_ = 0;
    local_project_ = false;
    paper_manual_stop_ = -1;
    loader_.clear();
    live_loader_.clear();
    reference_loader_.clear();
    stability_->setRowCount(0);
    stress_->setRowCount(0);
    status_->setText(QStringLiteral("Лаборатория очищена. Загрузите историю или ДЕМО."));
    quality_->setText(QStringLiteral("Данные лаборатории очищены."));
    statistics_->clear();
    tests_->clear();
    feature_text_->clear();
    report_->clear();
    explanation_->clear();
    explanation_bar_->setEnabled(false);
    paper_status_->setText(QStringLiteral("Счёт очищен."));
    for (auto *t : {correlations_, folds_, journal_, features_table_, backtest_journal_})
        t->setRowCount(0);
    for (auto *c :
         {distribution_chart_, acf_chart_, price_chart_, equity_chart_, drawdown_chart_, paper_chart_})
        c->clear();
    update_journey();
}
void ResearchPanel::load_next() {
    if (request_index_ < requested_.size()) {
        status_->setText(QStringLiteral("MOEX ISS: %1 • запрос %2/%3")
                             .arg(requested_[request_index_])
                             .arg(request_index_ + 1)
                             .arg(requested_.size()));
        if (custom_dates_->isChecked())
            loader_.load_range(requested_[request_index_], from_->date(), till_->date(),
                               period_->currentIndex() == 2 ? 60 : 24);
        else
            loader_.load(requested_[request_index_], period_->currentIndex());
        return;
    }
    loading_ = false;
    source_dirty_ = false;
    load_->setEnabled(true);
    period_->setEnabled(true);
    for (const auto &kv : data_)
        if (kv.second.size() >= 3)
            selected_->addItem(kv.first);
    if (selected_->count() > 0)
        selected_->setCurrentIndex(0);
    quality_->setText(diagnostics_.join('\n'));
    catalog_.load();
    preflight();
    update_journey();
    status_->setText(QStringLiteral(
        "История загружена. Выберите инструмент и нажмите расчёт. Незавершённые свечи исключены."));
}
QWidget *ResearchPanel::analytics_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(
        text(QStringLiteral("Посмотрите форму движения цены и сравните её со средними. Все значения ниже "
                            "рассчитаны по закрытым свечам, а не по отдельным обновлениям котировок.")));
    auto *window_form = new QFormLayout;
    window_form->addRow(QStringLiteral("Окно EMA и Z-score, свечей"), window_);
    layout->addLayout(window_form);
    price_chart_ = new VisualChart;
    price_chart_->clear(QStringLiteral("Загрузите данные и нажмите «Рассчитать»"));
    layout->addWidget(price_chart_);
    feature_text_ = text(QStringLiteral("Индикаторы появятся после расчёта."));
    layout->addWidget(feature_text_);
    auto *distribution = new QWidget;
    auto *dl = new QVBoxLayout(distribution);
    statistics_ = text(QStringLiteral("Статистика появится после расчёта."));
    dl->addWidget(statistics_);
    distribution_mode_ = new QComboBox;
    distribution_mode_->addItems({QStringLiteral("Распределение доходностей"),
                                  QStringLiteral("Простые доходности по порядку"),
                                  QStringLiteral("Логарифмические доходности по порядку")});
    dl->addWidget(distribution_mode_);
    distribution_chart_ = new VisualChart;
    dl->addWidget(distribution_chart_);
    layout->addWidget(ui::fold(QStringLiteral("Доходности и распределение"), distribution, true));
    analytics_expert_ = new QWidget;
    auto *expert_layout = new QVBoxLayout(analytics_expert_);
    expert_layout->addWidget(expert_controls_);
    tests_ = text(QStringLiteral("ADF/KPSS: сначала выполните расчёт."));
    expert_layout->addWidget(tests_);
    acf_chart_ = new VisualChart;
    expert_layout->addWidget(acf_chart_);
    features_table_ =
        table({QStringLiteral("Свеча МСК"), QStringLiteral("Закрытие"), QStringLiteral("Быстрая SMA"),
               QStringLiteral("Медленная SMA"), "EMA", "RSI", "MACD", QStringLiteral("Сигнал"),
               QStringLiteral("BB верх"), QStringLiteral("BB низ"), "ATR", "NATR %", "Z-score"});
    expert_layout->addWidget(features_table_);
    correlations_ = table({});
    expert_layout->addWidget(
        text(QStringLiteral("Корреляции: насколько похожи доходности акций на совпадающих интервалах")));
    expert_layout->addWidget(correlations_);
    layout->addWidget(analytics_expert_);
    connect(distribution_mode_, &QComboBox::currentIndexChanged, this, [this] { show_distribution(); });
    return page;
}
quant::RuleGroup ResearchPanel::ui_rules(const std::vector<RuleUi> &controls, bool all, bool custom) const {
    quant::RuleGroup group;
    group.all = all;
    std::array<int, 3> joins{};
    std::array<int, 4> opens{}, closes{};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const auto &r = controls[i];
        group.rules.push_back({static_cast<quant::Feature>(r.lhs->currentIndex()),
                               static_cast<quant::Feature>(r.rhs->currentIndex()),
                               static_cast<quant::Compare>(r.op->currentIndex()), r.value->value(),
                               r.enabled->isChecked()});
        if (i)
            joins[i - 1] = r.join->currentIndex();
        opens[i] = r.open->currentIndex();
        closes[i] = r.close->currentIndex();
    }
    if (custom)
        group.expression = quant::compile_rule_expression(joins, opens, closes);
    return group;
}
QWidget *ResearchPanel::rules_box(const QString &title, std::vector<RuleUi> &controls, QCheckBox *&all,
                                  QComboBox *&expression_mode, bool entry) {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(text(title));
    expression_mode = new QComboBox;
    expression_mode->addItems(
        {QStringLiteral("Простой режим: все / любое"), QStringLiteral("Формула: И, ИЛИ и скобки")});
    expression_mode->setAccessibleName(title + QStringLiteral(" • способ объединения"));
    layout->addWidget(expression_mode);
    all = new QCheckBox(
        QStringLiteral("Должны выполняться ВСЕ отмеченные условия (снимите флажок для «хотя бы одного»)"));
    all->setChecked(true);
    layout->addWidget(all);
    QList<QWidget *> syntax_rows;
    for (int i = 0; i < 4; ++i) {
        RuleUi r;
        r.enabled = new QCheckBox(QString::number(i + 1));
        r.enabled->setAccessibleName(title + QStringLiteral(" • условие ") + QString::number(i + 1));
        r.enabled->setChecked(i == 0);
        r.lhs = new QComboBox;
        r.lhs->addItems(feature_names());
        if (auto *model = qobject_cast<QStandardItemModel *>(r.lhs->model()))
            model->item(0)->setEnabled(false);
        r.lhs->setCurrentIndex(i == 0 ? 2 : 5);
        r.op = new QComboBox;
        r.op->addItems({QStringLiteral("больше"), QStringLiteral("меньше"),
                        QStringLiteral("пересекает вверх"), QStringLiteral("пересекает вниз")});
        r.op->setCurrentIndex(i == 0 ? (entry ? 2 : 3) : (entry ? 1 : 0));
        r.rhs = new QComboBox;
        r.rhs->addItems(feature_names());
        r.rhs->setCurrentIndex(i == 0 ? 3 : 0);
        r.value = number(entry ? 70 : 75, -1e9, 1e9, 4);
        r.value->setEnabled(i != 0);
        auto sync = [r] {
            const bool on = r.enabled->isChecked();
            r.lhs->setEnabled(on);
            r.op->setEnabled(on);
            r.rhs->setEnabled(on);
            r.value->setVisible(r.rhs->currentIndex() == 0);
            r.value->setEnabled(on && r.rhs->currentIndex() == 0);
        };
        connect(r.rhs, &QComboBox::currentIndexChanged, r.value, [sync](int) { sync(); });
        connect(r.enabled, &QCheckBox::toggled, r.value, [sync](bool) { sync(); });
        sync();
        auto *row = new QHBoxLayout;
        row->addWidget(r.enabled);
        row->addWidget(r.lhs);
        row->addWidget(r.op);
        row->addWidget(r.rhs);
        row->addWidget(r.value);
        layout->addLayout(row);
        auto *syntax = new QWidget;
        auto *syntax_layout = new QHBoxLayout(syntax);
        syntax_layout->setContentsMargins(0, 0, 0, 0);
        r.join = new QComboBox;
        r.join->addItems({QStringLiteral("И"), QStringLiteral("ИЛИ")});
        r.join->setEnabled(i > 0);
        r.open = new QComboBox;
        r.open->addItems({QStringLiteral("Без открывающей"), "(", "((", "((("});
        r.close = new QComboBox;
        r.close->addItems({QStringLiteral("Без закрывающей"), ")", "))", ")))"});
        r.join->setAccessibleName(title + QStringLiteral(" • связь перед строкой %1").arg(i + 1));
        r.open->setAccessibleName(title + QStringLiteral(" • открыть скобки строки %1").arg(i + 1));
        r.close->setAccessibleName(title + QStringLiteral(" • закрыть скобки строки %1").arg(i + 1));
        syntax_layout->addWidget(text(QStringLiteral("Строка %1:").arg(i + 1)));
        syntax_layout->addWidget(r.join);
        syntax_layout->addWidget(r.open);
        syntax_layout->addWidget(r.close);
        syntax_layout->addStretch();
        layout->addWidget(syntax);
        syntax_rows.push_back(syntax);
        controls.push_back(r);
    }
    auto *preview = text(QString());
    preview->setObjectName(entry ? "entryFormulaPreview" : "exitFormulaPreview");
    layout->insertWidget(2, preview);
    auto *syntax_help = text(
        QStringLiteral("В формуле И выполняется раньше ИЛИ. Скобки меняют порядок; до 3 уровней. Выключенные "
                       "строки и пустые подгруппы исключаются; если выключено всё, сигнала нет. Пример 1 И "
                       "(2 ИЛИ 3): перед 2 выберите И и (, перед 3 — ИЛИ и ), строку 4 выключите."));
    layout->addWidget(syntax_help);
    auto *fields = &controls;
    const auto refresh = [this, fields, all, expression_mode, preview, syntax_rows, syntax_help] {
        const bool custom = expression_mode->currentIndex() == 1;
        all->setVisible(!custom);
        syntax_help->setVisible(custom);
        for (auto *row : syntax_rows)
            row->setVisible(custom);
        try {
            const auto group = ui_rules(*fields, all->isChecked(), custom);
            auto formula = QString::fromStdString(quant::rule_expression_text(group));
            formula.replace("AND", QStringLiteral("И")).replace("OR", QStringLiteral("ИЛИ"));
            if (formula == "EMPTY")
                formula = QStringLiteral("Нет включённых условий — сигнала нет");
            preview->setText(QStringLiteral("Итоговая формула: %1").arg(formula));
        } catch (const std::exception &) {
            preview->setText(
                QStringLiteral("Ошибка скобок: проверьте баланс и глубину до 3. Расчёт не будет запущен."));
        }
    };
    connect(expression_mode, &QComboBox::currentIndexChanged, this, [refresh] { refresh(); });
    connect(all, &QCheckBox::toggled, this, [refresh] { refresh(); });
    for (const auto &r : controls) {
        connect(r.enabled, &QCheckBox::toggled, this, [refresh] { refresh(); });
        for (auto *field : {r.join, r.open, r.close})
            connect(field, &QComboBox::currentIndexChanged, this, [refresh] { refresh(); });
    }
    refresh();
    return page;
}
QWidget *ResearchPanel::strategy_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(text(QStringLiteral(
        "Когда условие входа выполнено на закрытии свечи, тренажёр покупает на следующем открытии. Условие "
        "выхода работает так же. Продажи без покрытия и реальные заявки отсутствуют.")));
    auto *presets = new QHBoxLayout;
    presets->addWidget(text(QStringLiteral("Готовое правило:")));
    for (int preset = 0; preset < 3; ++preset) {
        const QStringList names{QStringLiteral("Пересечение средних"),
                                QStringLiteral("RSI: ниже 30 / выше 70"), QStringLiteral("Цена и EMA")};
        auto *button = new QPushButton(names[preset]);
        presets->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, preset] {
            for (auto *group : {&entry_rules_, &exit_rules_})
                for (std::size_t i = 0; i < group->size(); ++i)
                    (*group)[i].enabled->setChecked(i == 0);
            auto set = [](RuleUi &r, int lhs, int op, int rhs, double value) {
                r.lhs->setCurrentIndex(lhs);
                r.op->setCurrentIndex(op);
                r.rhs->setCurrentIndex(rhs);
                r.value->setValue(value);
            };
            if (preset == 0) {
                fast_->setValue(10);
                slow_->setValue(30);
                set(entry_rules_[0], 2, 2, 3, 0);
                set(exit_rules_[0], 2, 3, 3, 0);
            } else if (preset == 1) {
                set(entry_rules_[0], 5, 1, 0, 30);
                set(exit_rules_[0], 5, 0, 0, 70);
            } else {
                set(entry_rules_[0], 1, 2, 4, 0);
                set(exit_rules_[0], 1, 3, 4, 0);
            }
            entry_all_->setChecked(true);
            exit_all_->setChecked(true);
            entry_expression_->setCurrentIndex(0);
            exit_expression_->setCurrentIndex(0);
        });
    }
    layout->addLayout(presets);
    fast_ = integer(10, 2, 100);
    slow_ = integer(30, 3, 200);
    auto *periods = new QFormLayout;
    periods->addRow(QStringLiteral("Быстрая средняя, свечей"), fast_);
    periods->addRow(QStringLiteral("Медленная средняя, свечей"), slow_);
    layout->addLayout(periods);
    layout->addWidget(
        rules_box(QStringLiteral("Когда покупать"), entry_rules_, entry_all_, entry_expression_, true));
    layout->addWidget(
        rules_box(QStringLiteral("Когда продавать"), exit_rules_, exit_all_, exit_expression_, false));
    entry_confirmation_ = integer(1, 1, 50);
    exit_confirmation_ = integer(1, 1, 50);
    cooldown_bars_ = integer(0, 0, 500);
    max_holding_bars_ = integer(0, 0, 5000);
    signal_delay_ = integer(0, 0, 10);
    auto *timing = new QWidget;
    auto *tf = new QFormLayout(timing);
    tf->addRow(QStringLiteral("Подтвердить вход, закрытий подряд"), entry_confirmation_);
    tf->addRow(QStringLiteral("Подтвердить выход, закрытий подряд"), exit_confirmation_);
    tf->addRow(QStringLiteral("Пауза после закрытия позиции, свечей"), cooldown_bars_);
    tf->addRow(QStringLiteral("Срок удержания, свечей (0 = без срока)"), max_holding_bars_);
    tf->addRow(QStringLiteral("Дополнительная задержка сигнала, свечей"), signal_delay_);
    tf->addRow(text(QStringLiteral(
        "Подтверждение требует истинности всей группы на каждом закрытии. Пересечение — событие одной свечи: "
        "для выдержки обычно нужны «больше/меньше». Пауза отсчитывает полные закрытия после свечи выхода. "
        "Срок включает свечу входа; выход — на следующем открытии без дополнительной задержки. Стопы и риск "
        "не ждут подтверждения или паузы. Эти параметры относятся к стратегии одной акции, не к отдельному "
        "портфельному движку.")));
    layout->addWidget(ui::fold(QStringLiteral("Подтверждение, пауза и время исполнения"), timing, true));
    cash_ = number(100000, 100, 1e12, 2);
    lot_ = integer(1, 1, 1000000);
    commission_ = number(.05, 0, 10, 4);
    fixed_ = number(0, 0, 100000, 2);
    spread_ = number(2, 0, 500);
    slippage_ = number(2, 0, 500);
    allocation_ = number(20, .1, 100);
    participation_ = number(0, 0, 100);
    stop_ = number(2, 0, 99);
    target_ = number(4, 0, 1000);
    partial_target_ = number(0, 0, 1000, 2);
    partial_fraction_ = number(50, 1, 99, 2);
    partial_target_->setToolTip(QStringLiteral(
        "Например: частичный рост 2%, полная цель 4%. Если полная цель ниже частичного порога, она может "
        "закрыть всю позицию раньше. Триггер не гарантирует цену следующего открытия."));
    risk_ = number(1, .01, 100);
    max_dd_ = number(20, .01, 100);
    daily_ = number(5, .01, 100);
    max_entries_ = integer(10, 1, 1000);
    annual_ = integer(0, 0, 100000);
    ambiguity_ = new QComboBox;
    ambiguity_->addItems(
        {QStringLiteral("Осторожно: сначала стоп"), QStringLiteral("Оптимистично: сначала цель")});
    auto *budget = new QWidget;
    auto *form = new QFormLayout(budget);
    form->addRow(QStringLiteral("Виртуальные деньги, ₽"), cash_);
    form->addRow(QStringLiteral("Риск одной сделки, % капитала"), risk_);
    form->addRow(QStringLiteral("Максимум денег в позиции, %"), allocation_);
    form->addRow(QStringLiteral("Стоп: допустимое движение вниз, %"), stop_);
    form->addRow(QStringLiteral("Цель: движение вверх для выхода, %"), target_);
    layout->addWidget(ui::fold(QStringLiteral("Деньги и риск сделки"), budget, true));
    auto *partial = new QWidget;
    auto *pf = new QFormLayout(partial);
    pf->addRow(QStringLiteral("Рост закрытия от цены покупки, % (0 = выключено)"), partial_target_);
    pf->addRow(QStringLiteral("Продать долю позиции, %"), partial_fraction_);
    pf->addRow(text(
        QStringLiteral("Один этап на сделку. Триггер — закрытие, исполнение — следующее открытие с обычной "
                       "задержкой сигнала. Количество округляется вниз до лотов; минимум один лот остаётся. "
                       "Если продаваемого лота нет, этап пропускается. Полный выход и стоп имеют приоритет; "
                       "при нехватке предыдущего объёма переносится только назначенный остаток. Это рост "
                       "цены без вычета комиссий; все издержки учитываются в деньгах.")));
    layout->addWidget(ui::fold(QStringLiteral("Плановый частичный выход"), partial));
    auto *higher = new QWidget;
    auto *hf = new QFormLayout(higher);
    higher_factor_ = new QComboBox;
    for (int factor : {0, 2, 3, 4, 6, 12, 24})
        higher_factor_->addItem(
            factor ? QStringLiteral("%1 часа/часов из H1").arg(factor) : QStringLiteral("Выключен"), factor);
    higher_fast_ = integer(2, 2, 99);
    higher_slow_ = integer(5, 3, 100);
    higher_exit_ = new QCheckBox(QStringLiteral("Подтверждать обычный выход: быстрая SMA ниже медленной"));
    hf->addRow(QStringLiteral("Старший блок (только часовые данные)"), higher_factor_);
    hf->addRow(QStringLiteral("Быстрая SMA, старших блоков"), higher_fast_);
    hf->addRow(QStringLiteral("Медленная SMA, старших блоков"), higher_slow_);
    hf->addRow(higher_exit_);
    hf->addRow(text(QStringLiteral("Вход требует готовую старшую SMA: быстрая > медленной. Блоки "
                                   "выровнены от местной полуночи; берутся только полностью закрытые "
                                   "блоки без пропусков. Это локальная агрегация H1, не отдельные "
                                   "биржевые свечи. После разрыва новые подтверждения запрещены до "
                                   "свежего полного блока. Стопы, риск, срок и частичный этап независимы. "
                                   "На коротких сессиях большие блоки могут не сформироваться.")));
    auto *higher_demo = new QPushButton(QStringLiteral("Открыть ДЕМО H1 + фильтр 4 часа (без сети)"));
    hf->addRow(higher_demo);
    connect(higher_demo, &QPushButton::clicked, this, [this] {
        cancel_work();
        demo(true);
        higher_factor_->setCurrentIndex(3);
    });
    const auto higher_controls = [this] {
        const bool enabled = higher_factor_->currentData().toInt() != 0;
        higher_fast_->setEnabled(enabled);
        higher_slow_->setEnabled(enabled);
        higher_exit_->setEnabled(enabled);
    };
    connect(higher_factor_, &QComboBox::currentIndexChanged, this, [higher_controls] { higher_controls(); });
    higher_controls();
    layout->addWidget(ui::fold(QStringLiteral("Подтверждение закрытым старшим интервалом"), higher));
    auto *execution = new QWidget;
    auto *ef = new QFormLayout(execution);
    ef->addRow(QStringLiteral("Комиссия при покупке и продаже, %"), commission_);
    ef->addRow(QStringLiteral("Фиксированная комиссия за исполнение, ₽"), fixed_);
    ef->addRow(QStringLiteral("Спред, базисных пунктов"), spread_);
    ef->addRow(QStringLiteral("Проскальзывание за сторону, б.п."), slippage_);
    ef->addRow(QStringLiteral("Если стоп и цель достигнуты в одной свече"), ambiguity_);
    ef->addRow(text(QStringLiteral(
        "1 б.п. = 0,01%. Спред и проскальзывание ухудшают цену исполнения. Комиссии вычитаются из "
        "виртуального счёта. Нулевой стоп или цель выключают соответствующий триггер.")));
    layout->addWidget(ui::fold(QStringLiteral("Как учитываются комиссии и исполнение"), execution));
    auto *limits = new QWidget;
    auto *lf = new QFormLayout(limits);
    lf->addRow(QStringLiteral("Остановить входы при просадке, %"), max_dd_);
    lf->addRow(QStringLiteral("Лимит потери за сессию, %"), daily_);
    lf->addRow(QStringLiteral("Максимум входов за сессию"), max_entries_);
    lf->addRow(QStringLiteral("Доля предыдущего объёма, % (0 = без ограничения)"), participation_);
    lf->addRow(QStringLiteral("Годовой масштаб Sharpe N (0 = на свечу)"), annual_);
    lf->addRow(
        text(QStringLiteral("Лимиты проверяются на закрытиях. Гэп может увеличить убыток: лимит и стоп не "
                            "гарантируют цену выхода. Годовой масштаб по умолчанию выключен.")));
    layout->addWidget(ui::fold(QStringLiteral("Аварийные лимиты и дополнительные допущения"), limits));
    risk_->setToolTip(QStringLiteral("Размер позиции ограничивается этим бюджетом с учётом ожидаемых "
                                     "издержек стопа. Убыток при гэпе может быть больше."));
    return page;
}
QWidget *ResearchPanel::report_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    result_banner_ = text(QStringLiteral(
        "Сначала рассчитайте стратегию. Здесь появится результат по выбранным закрытым свечам."));
    layout->addWidget(result_banner_);
    auto *summary = new QHBoxLayout;
    auto card = [summary](const QString &title) {
        auto *box = new QFrame;
        box->setStyleSheet("QFrame{background:#132b3d;border-radius:6px;}");
        auto *l = new QVBoxLayout(box);
        l->addWidget(text(title));
        auto *value = text(QStringLiteral("—"));
        value->setStyleSheet("font-size:23px;font-weight:600;");
        l->addWidget(value);
        summary->addWidget(box, 1);
        return value;
    };
    return_card_ = card(QStringLiteral("Доходность, %"));
    drawdown_card_ = card(QStringLiteral("Просадка, %"));
    fills_card_ = card(QStringLiteral("Исполнений"));
    cost_card_ = card(QStringLiteral("Издержки, ₽"));
    layout->addLayout(summary);
    auto *explanation_box = new QWidget;
    auto *explanation_layout = new QVBoxLayout(explanation_box);
    explanation_layout->addWidget(text(QStringLiteral("Выберите номер закрытой свечи завершённого расчёта. "
                                                      "Условия оцениваются на её закрытии; исполнение и "
                                                      "ограничения счёта смотрите в журнале сделок.")));
    explanation_bar_ = integer(1, 1, 1);
    explanation_bar_->setAccessibleName(QStringLiteral("Свеча для объяснения сигнала"));
    explanation_bar_->setEnabled(false);
    explanation_layout->addWidget(explanation_bar_);
    explanation_ = text(QStringLiteral("Сначала выполните расчёт стратегии."));
    explanation_->setTextFormat(Qt::PlainText);
    explanation_layout->addWidget(explanation_);
    connect(explanation_bar_, qOverload<int>(&QSpinBox::valueChanged), this, &ResearchPanel::explain_bar);
    layout->addWidget(ui::fold(QStringLiteral("Почему возникла или не возникла сделка? Условия и исполнение"),
                               explanation_box));
    auto *walk_parameters = new QWidget;
    auto *row = new QHBoxLayout(walk_parameters);
    train_ = integer(120, 60, 300);
    test_ = integer(40, 10, 100);
    row->addWidget(text(QStringLiteral("Обучение, свечей")));
    row->addWidget(train_);
    row->addWidget(text(QStringLiteral("Проверка, свечей")));
    row->addWidget(test_);
    row->addStretch();
    layout->addWidget(
        ui::fold(QStringLiteral("Настроить последовательную проверку (walk-forward)"), walk_parameters));
    report_ = text(QStringLiteral("Нажмите общий расчёт после настройки правил."));
    layout->addWidget(ui::fold(QStringLiteral("Все показатели и методика расчёта"), report_));
    equity_chart_ = new VisualChart;
    drawdown_chart_ = new VisualChart;
    layout->addWidget(equity_chart_);
    layout->addWidget(ui::fold(QStringLiteral("График просадки"), drawdown_chart_));
    folds_ = table({QStringLiteral("Обучение"), QStringLiteral("Тест"), "Fast / slow",
                    QStringLiteral("Обучение, %"), QStringLiteral("Вне выборки, %")});
    layout->addWidget(ui::fold(QStringLiteral("Результаты на следующих отрезках истории"), folds_));
    backtest_journal_ = table({QStringLiteral("Свеча МСК"), QStringLiteral("Акций ±"), QStringLiteral("Цена"),
                               QStringLiteral("Деньги"), QStringLiteral("Журнал бэктеста: причина")});
    layout->addWidget(
        ui::fold(QStringLiteral("Сделки и причины решений (нажмите строку для перехода к графику)"),
                 backtest_journal_));
    layout->addWidget(ui::fold(
        QStringLiteral("Что означает сравнение с базами"),
        text(QStringLiteral(
            "Сетка walk-forward: fast {5,10,20}, slow {20,40,60}, fast<slow; выбирается чистая доходность "
            "обучения после издержек. Другие правила фиксированы. Каждый тест начинается без позиции и "
            "заканчивается ликвидацией; капитал переносится между тестами. Остаток истории короче окна теста "
            "не "
            "используется. Это не независимый финальный holdout при многократном ручном подборе.\nБаза "
            "«держать» "
            "использует ту же долю капитала, комиссии и спред; покупка на первом открытии, без стопов и "
            "лимитов "
            "стратегии. Денежная база без процентов. CAGR/Calmar показываются только при горизонте не менее "
            "30 "
            "календарных дней; короткая годовая экстраполяция ненадёжна."))));
    return page;
}
QWidget *ResearchPanel::paper_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(
        text(QStringLiteral("ВИРТУАЛЬНЫЙ СЧЁТ • проигрывание загруженных закрытых свечей • реальных денег и "
                            "торговых API нет. Параметры фиксируются при открытии счёта.")));
    auto *controls = new QHBoxLayout;
    auto *start = new QPushButton(QStringLiteral("Начать проигрывание истории"));
    auto *step = new QPushButton(QStringLiteral("Следующая свеча"));
    auto *play = new QPushButton(QStringLiteral("Запустить автоматически"));
    paper_play_ = play;
    paper_step_ = step;
    play->setCheckable(true);
    auto *kill = new QPushButton(QStringLiteral("АВАРИЙНЫЙ СТОП"));
    kill->setStyleSheet("background:#703645;");
    kill->setToolTip(QStringLiteral(
        "Запрет новых входов. Открытая позиция закрывается на следующей доступной свече по модели."));
    for (auto *b : {start, step, play, kill})
        controls->addWidget(b);
    controls->addStretch();
    layout->addLayout(controls);
    paper_status_ = text(QStringLiteral("Счёт не открыт."));
    layout->addWidget(paper_status_);
    paper_chart_ = new VisualChart;
    layout->addWidget(paper_chart_);
    journal_ = table({QStringLiteral("Свеча МСК"), QStringLiteral("Акций ±"), QStringLiteral("Цена"),
                      QStringLiteral("Деньги"), QStringLiteral("Причина / решение")});
    layout->addWidget(journal_);
    connect(start, &QPushButton::clicked, this, &ResearchPanel::start_paper);
    connect(step, &QPushButton::clicked, this, &ResearchPanel::step_paper);
    connect(play, &QPushButton::clicked, this, [this, play] {
        if (paper_timer_.isActive())
            paper_timer_.stop();
        else if (paper_)
            paper_timer_.start();
        play->setChecked(paper_timer_.isActive());
        play->setText(paper_timer_.isActive() ? QStringLiteral("Приостановить")
                                              : QStringLiteral("Запустить автоматически"));
    });
    connect(&paper_timer_, &QTimer::timeout, play, [this, play] {
        play->setChecked(paper_timer_.isActive());
        if (!paper_timer_.isActive())
            play->setText(QStringLiteral("Запустить автоматически"));
    });
    connect(kill, &QPushButton::clicked, this,
            [this] { stop_paper(QStringLiteral("Остановка пользователем")); });
    return page;
}
quant::SimConfig ResearchPanel::settings() const {
    quant::SimConfig c;
    c.strategy.fast = fast_->value();
    c.strategy.indicator_window = window_->value();
    c.strategy.slow = slow_->value();
    if (c.strategy.fast >= c.strategy.slow)
        throw std::invalid_argument("SMA fast must be smaller than slow");
    c.strategy.entry =
        ui_rules(entry_rules_, entry_all_->isChecked(), entry_expression_->currentIndex() == 1);
    c.strategy.exit = ui_rules(exit_rules_, exit_all_->isChecked(), exit_expression_->currentIndex() == 1);
    c.initial_cash = cash_->value();
    c.lot = lot_->value();
    c.tick_size = tick_ ? tick_->value() : 0;
    if (!synthetic_ && !local_project_ && !manual_metadata_->isChecked())
        for (const auto &i : catalog_.instruments())
            if (i.symbol == selected_->currentText()) {
                c.lot = i.lot_size;
                c.tick_size = i.tick_size;
                break;
            }
    c.commission_rate = commission_->value() / 100;
    c.fixed_fee = fixed_->value();
    c.spread_bps = spread_->value();
    c.slippage_bps = slippage_->value();
    c.allocation = allocation_->value() / 100;
    c.stop_fraction = stop_->value() / 100;
    c.target_fraction = target_->value() / 100;
    c.risk_per_trade = risk_->value() / 100;
    c.max_drawdown = max_dd_->value() / 100;
    c.daily_loss = daily_->value() / 100;
    c.max_entries_per_day = max_entries_->value();
    c.pessimistic = ambiguity_->currentIndex() == 0;
    c.annual_periods = annual_->value();
    c.participation = participation_->value() / 100;
    c.entry_confirmation = entry_confirmation_->value();
    c.exit_confirmation = exit_confirmation_->value();
    c.cooldown_bars = cooldown_bars_->value();
    c.max_holding_bars = max_holding_bars_->value();
    c.signal_delay = signal_delay_->value();
    c.partial_target_fraction = partial_target_->value() / 100;
    c.partial_exit_fraction = partial_fraction_->value() / 100;
    c.higher_factor = higher_factor_->currentData().toInt();
    c.higher_fast = higher_fast_->value();
    c.higher_slow = higher_slow_->value();
    c.higher_confirm_exit = higher_exit_->isChecked();
    if (c.higher_factor && period_->currentIndex() != 2)
        throw std::invalid_argument("Higher filter requires H1 data");
    return c;
}
std::vector<quant::Bar> ResearchPanel::selected_bars() const {
    const auto it = data_.find(selected_->currentText());
    return it == data_.end() ? std::vector<quant::Bar>{} : it->second;
}
void ResearchPanel::calculate() {
    if (loading_ || worker_->isRunning() || (snapshot_worker_ && snapshot_worker_->isRunning()))
        return;
    if (source_dirty_) {
        status_->setText(QStringLiteral("Параметры загрузки изменены. Сначала загрузите новый диапазон."));
        return;
    }
    auto bars = selected_bars();
    preflight();
    if (bars.size() < 40) {
        status_->setText(
            QStringLiteral("Нужно минимум 40 закрытых свечей. Для walk-forward — обучение + тест."));
        return;
    }
    quant::SimConfig config;
    try {
        config = settings();
    } catch (const std::exception &) {
        evaluate_holdout_ = false;
        status_->setText(QStringLiteral("Расчёт не запущен. Для старшего фильтра нужны H1, быстрая SMA "
                                        "меньше медленной. Проверьте также обычные SMA, баланс "
                                        "скобок и глубину формул до 3."));
        return;
    }
    if (!synthetic_ && !manual_metadata_->isChecked()) {
        bool known = false;
        for (const auto &item : catalog_.instruments())
            if (item.symbol == selected_->currentText() && item.tick_size > 0)
                known = true;
        if (!known) {
            status_->setText(QStringLiteral(
                "Сначала загрузите лот и шаг из MOEX или явно включите ручную проверку метаданных."));
            return;
        }
    }
    const auto complete_bars = bars;
    const bool holdout = evaluate_holdout_;
    evaluate_holdout_ = false;
    const bool reserve = reserve_->isChecked();
    const auto split = static_cast<std::size_t>(complete_bars.size() * .8);
    if (reserve && !holdout)
        bars.resize(split);
    if (bars.size() < 40)
        return;
    const int embargo = gap_->value();
    auto parameters = configuration();
    auto effective_values = parameters.value("values").toObject();
    effective_values.insert("study_lot", static_cast<double>(config.lot));
    effective_values.insert("study_tick", config.tick_size);
    parameters.insert("values", effective_values);
    parameters.insert("data_begin_utc", QString::number(bars.front().time));
    parameters.insert("data_end_utc", QString::number(bars.back().end));
    parameters.insert("bar_count", static_cast<int>(bars.size()));
    const auto hash = fingerprint(bars);
    const auto source = selected_->currentText() + " • " +
                        (local_project_ ? QStringLiteral("ЛОКАЛЬНЫЙ ПРОЕКТ, источник не перепроверен")
                         : synthetic_   ? QStringLiteral("СИНТЕТИЧЕСКОЕ ДЕМО")
                                        : QStringLiteral("MOEX ISS, задержанные данные"));
    attempts_ = std::min(1000000, attempts_ + 1);
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = cancel_;
    const int window = window_->value(), lags = lags_->value(), adf = adf_lags_->value(),
              kpss = kpss_lags_->value(), train = train_->value(), test = test_->value();
    std::vector<std::vector<quant::Bar>> series;
    for (const auto &kv : data_)
        if (kv.second.size() >= 3) {
            auto sample = kv.second;
            if (reserve && !holdout)
                sample.erase(std::remove_if(sample.begin(), sample.end(),
                                            [&bars](const quant::Bar &b) { return b.end > bars.back().end; }),
                             sample.end());
            series.push_back(std::move(sample));
        }
    calculate_->setEnabled(false);
    status_->setText(QStringLiteral("Считаем в фоне. Окно остаётся доступным; можно отменить расчёт."));
    worker_->setFuture(QtConcurrent::run([bars, complete_bars, series, config, window, lags, adf, kpss, train,
                                          test, cancelled, parameters, hash, source, holdout, split,
                                          embargo] {
        LabResult r;
        r.explained_strategy = config.strategy;
        r.explained_config = config;
        r.parameters = parameters;
        r.hash = hash;
        r.source = source;
        try {
            std::vector<double> prices;
            for (const auto &b : bars)
                prices.push_back(b.close);
            r.distribution = quant::distribution(prices);
            r.acf = quant::acf(r.distribution.log_returns,
                               std::min<int>(lags, static_cast<int>(prices.size()) / 2 - 1));
            r.pacf = quant::pacf(r.distribution.log_returns,
                                 std::min<int>(lags, static_cast<int>(prices.size()) / 2 - 1));
            r.stationarity = quant::stationarity(prices, adf, kpss);
            r.features = quant::candle_features(bars, config.strategy.fast, config.strategy.slow, window);
            if (cancelled->load())
                return r;
            if (std::any_of(series.begin(), series.end(),
                            [](const std::vector<quant::Bar> &s) { return s.empty(); }))
                r.correlation.matrix.assign(series.size(),
                                            std::vector<double>(series.size(), quant::unavailable()));
            else
                r.correlation = quant::aligned_correlation(series);
            r.simulation = quant::backtest(bars, config, 0, true);
            r.snapshot = std::make_shared<const ReplaySnapshot>(ReplaySnapshot{bars, config, r.simulation});
            r.baseline = quant::buy_hold(bars, config);
            if (cancelled->load())
                return r;
            r.walk = quant::walk_forward(bars, config, train, test);
            if (cancelled->load())
                return r;
            r.robustness = quant::robustness(bars, config, cancelled.get());
            if (holdout && !cancelled->load()) {
                r.holdout = quant::final_holdout(complete_bars, config, split, embargo);
                r.has_holdout = true;
            }
        } catch (const std::exception &) {
            r.error =
                QStringLiteral("Расчёт невозможен: недостаточно данных, некорректные интервалы/параметры "
                               "или вырожденный ряд. Проверьте окно и число лагов.");
        }
        return r;
    }));
    update_journey();
}
void ResearchPanel::explain_bar(int index) {
    const auto &r = last_result_;
    if (index < 1 || static_cast<std::size_t>(index) > r.features.size()) {
        explanation_->setText(QStringLiteral("Нет рассчитанной свечи."));
        return;
    }
    quant::Features previous;
    previous.fill(quant::unavailable());
    if (index > 1)
        previous = r.features[static_cast<std::size_t>(index - 2)];
    const auto &current = r.features[static_cast<std::size_t>(index - 1)];
    QStringList lines{QStringLiteral("Зафиксированный результат: %1\nСвеча №%2. Отпечаток данных: %3")
                          .arg(r.source)
                          .arg(index)
                          .arg(r.hash.left(16))};
    const auto names = feature_names();
    const auto describe = [&](const quant::RuleGroup &group, const QString &title) {
        lines << QStringLiteral("\n%1 • %2 • сигнал: %3")
                     .arg(title,
                          QString::fromStdString(quant::rule_expression_text(group))
                              .replace("AND", QStringLiteral("И"))
                              .replace("OR", QStringLiteral("ИЛИ"))
                              .replace("EMPTY", QStringLiteral("нет включённых условий")),
                          quant::evaluate(group, previous, current) ? QStringLiteral("да")
                                                                    : QStringLiteral("нет"));
        int rule_number = 0;
        for (const auto &rule : group.rules) {
            lines << QStringLiteral("Условие %1:").arg(++rule_number);
            if (!rule.enabled) {
                lines << QStringLiteral("Условие выключено");
                continue;
            }
            const auto lhs = static_cast<std::size_t>(rule.lhs), rhs = static_cast<std::size_t>(rule.rhs);
            if (lhs >= current.size() || rhs >= current.size()) {
                lines << QStringLiteral("Некорректное условие");
                continue;
            }
            const double a = current[lhs],
                         b = rule.rhs == quant::Feature::Constant ? rule.value : current[rhs];
            const double pa = previous[lhs],
                         pb = rule.rhs == quant::Feature::Constant ? rule.value : previous[rhs];
            const bool crossing =
                rule.op == quant::Compare::CrossAbove || rule.op == quant::Compare::CrossBelow;
            const bool ready = std::isfinite(a) && std::isfinite(b) &&
                               (!crossing || (std::isfinite(pa) && std::isfinite(pb)));
            const bool match = quant::evaluate(quant::RuleGroup{{rule}, true}, previous, current);
            const QStringList operations{QStringLiteral("выше"), QStringLiteral("ниже"),
                                         QStringLiteral("пересекает вверх"),
                                         QStringLiteral("пересекает вниз")};
            lines << QStringLiteral("%1 %2 %3: %4. Сейчас: %5 / %6%7")
                         .arg(names.value(static_cast<int>(lhs)), operations.value(static_cast<int>(rule.op)),
                              rule.rhs == quant::Feature::Constant ? fmt(rule.value)
                                                                   : names.value(static_cast<int>(rhs)),
                              !ready  ? QStringLiteral("недостаточно наблюдений / значение недоступно")
                              : match ? QStringLiteral("выполнено")
                                      : QStringLiteral("не выполнено"),
                              fmt(a), fmt(b),
                              crossing ? QStringLiteral("; предыдущая свеча: %1 / %2").arg(fmt(pa), fmt(pb))
                                       : QString());
        }
    };
    if (r.explained_strategy.buy_hold)
        lines << QStringLiteral("Режим удержания: вход задаётся моделью удержания, "
                                "условия входа не используются.");
    else
        describe(r.explained_strategy.entry, QStringLiteral("Вход"));
    describe(r.explained_strategy.exit, QStringLiteral("Выход"));
    if (static_cast<std::size_t>(index) <= r.simulation.trace.size()) {
        const auto &t = r.simulation.trace[static_cast<std::size_t>(index - 1)];
        lines << QStringLiteral("\nФАКТИЧЕСКАЯ МОДЕЛЬ ИСПОЛНЕНИЯ\n%1\nАкций: %2 → %3; деньги: %4 → %5 ₽; "
                                "капитал на закрытии: %6 ₽")
                     .arg(execution_gate(t.gate))
                     .arg(t.units_before)
                     .arg(t.units_after)
                     .arg(fmt(t.cash_before), fmt(t.cash_after), fmt(t.equity));
        lines << QStringLiteral("Подтверждения входа: %1 из %2; выхода: %3 из %4. Свечей удержания: %5 (срок "
                                "%6; 0 — без срока). Пауза на этом закрытии: %7.\nДополнительных открытий до "
                                "исполнения входа/выхода: %8/%9.")
                     .arg(t.entry_streak)
                     .arg(r.explained_config.entry_confirmation)
                     .arg(t.exit_streak)
                     .arg(r.explained_config.exit_confirmation)
                     .arg(t.held_bars)
                     .arg(r.explained_config.max_holding_bars)
                     .arg(t.cooldown_remaining)
                     .arg(t.entry_wait)
                     .arg(t.exit_wait);
        if (r.explained_config.partial_target_fraction > 0)
            lines
                << QStringLiteral(
                       "Частичный этап: рост %1%, доля %2%. Назначенный остаток: %3 акций; задержка: %4; "
                       "этап завершён/пропущен: %5. Последующие полные выходы отменяют назначенный остаток.")
                       .arg(fmt(r.explained_config.partial_target_fraction * 100),
                            fmt(r.explained_config.partial_exit_fraction * 100))
                       .arg(t.partial_remaining)
                       .arg(t.partial_wait)
                       .arg(t.partial_complete ? QStringLiteral("да") : QStringLiteral("нет"));
        if (r.explained_config.higher_factor) {
            lines << QStringLiteral("Старший масштаб: %1 ч; полный прогрев: %2; блоков: %3; "
                                    "собирается %4/%1.\nПоследнее закрытие: %5; SMA %6 / "
                                    "%7. Вход разрешён: %8; обычный выход разрешён: %9. "
                                    "Разрывов/смен сессии: %10; отброшено неполных блоков: "
                                    "%11.")
                         .arg(r.explained_config.higher_factor)
                         .arg(t.higher.ready ? QStringLiteral("да") : QStringLiteral("нет"))
                         .arg(t.higher.completed)
                         .arg(t.higher.progress)
                         .arg(t.higher.last_end ? stamp(t.higher.last_end) : QStringLiteral("нет"))
                         .arg(fmt(t.higher.fast), fmt(t.higher.slow))
                         .arg(t.higher_entry_allowed ? QStringLiteral("да") : QStringLiteral("нет"))
                         .arg(t.higher_exit_allowed ? QStringLiteral("да") : QStringLiteral("нет"))
                         .arg(t.higher.breaks)
                         .arg(t.higher.discarded);
        }
        for (int i = 0; i < t.fill_count; ++i) {
            const auto &f = t.fills[i];
            lines << QStringLiteral(
                         "%1\nЗапрошено: %2; исполнено: %3 акций. Триггер: %4; модельная цена: %5 ₽. "
                         "Комиссия: %6 ₽; ухудшение цены: %7 ₽. Деньги после: %8 ₽; позиция: %9.")
                         .arg(reason(f.reason))
                         .arg(f.requested)
                         .arg(f.quantity)
                         .arg(fmt(f.trigger, 4), fmt(f.price, 4), fmt(f.fee, 4), fmt(f.cost, 4),
                              fmt(f.cash_after))
                         .arg(f.units_after);
        }
        if (!t.fill_count)
            lines << QStringLiteral("Попыток исполнения в этой свече не было.");
    }
    lines << QStringLiteral("\nСигнал не гарантирует сделку: учитываются позиция, деньги, задержка "
                            "исполнения, стопы и лимиты риска. Этот разбор относится только к "
                            "исходным правилам бэктеста, не к подобранным правилам "
                            "walk-forward/holdout.");
    explanation_->setText(lines.join('\n'));
}
void ResearchPanel::show_distribution() {
    const auto &d = last_result_.distribution;
    if (d.simple.empty())
        return;
    if (distribution_mode_->currentIndex() == 0) {
        QVector<QPointF> bins;
        for (std::size_t i = 0; i < d.bin_counts.size(); ++i)
            bins.push_back({d.bin_centers[i] * 100, d.bin_counts[i]});
        distribution_chart_->set_histogram(
            bins, QStringLiteral("Распределение доходностей"),
            QStringLiteral("Ось X: простая доходность, % • ось Y: число интервалов"));
    } else {
        const bool log = distribution_mode_->currentIndex() == 2;
        distribution_chart_->set_lines(
            {{log ? QStringLiteral("Логдоходность") : QStringLiteral("Доходность"), QColor("#39d2b0"),
              points(log ? d.log_returns : d.simple, 100), false}},
            QStringLiteral("Доходности, %"),
            QStringLiteral("Нулевые и пропущенные интервалы не подставляются"));
    }
}
void ResearchPanel::present(const LabResult &r) {
    explanation_bar_->setRange(1, std::max(1, static_cast<int>(r.features.size())));
    explanation_bar_->setEnabled(!r.features.empty());
    explain_bar(explanation_bar_->value());
    auto bars = selected_bars();
    if (bars.size() > r.features.size())
        bars.resize(r.features.size());
    const auto &d = r.distribution;
    statistics_->setText(
        QStringLiteral("%1 • %2 закрытых свечей, %3 доходностей\nСредняя: %4% • σ (выборочная): %5% • q05 / "
                       "медиана / q95: %6 / %7 / %8%\nАсимметрия: %9 • эксцесс: %10 (моментные оценки)")
            .arg(selected_->currentText())
            .arg(bars.size())
            .arg(d.simple.size())
            .arg(fmt(d.mean * 100), fmt(d.sd * 100), fmt(d.q05 * 100), fmt(d.median * 100), fmt(d.q95 * 100),
                 fmt(d.skew), fmt(d.excess_kurtosis)));
    const auto &s = r.stationarity;
    tests_->setText(
        QStringLiteral(
            "Тесты УРОВНЯ цены с константой: ADF=%1, критическое 5%=%2, lag=%3; %4\nKPSS=%5, критическое "
            "5%=0,463, bandwidth=%6; %7\nADF H₀: единичный корень; KPSS H₀: стационарность уровня. "
            "Неотвержение не доказывает гипотезу. Тренд/автолаг не подбираются. ACF/PACF ниже рассчитаны для "
            "логдоходностей, в торговых наблюдениях, не равных календарных часах.")
            .arg(fmt(s.adf), fmt(s.adf_critical5))
            .arg(s.adf_lags)
            .arg(!std::isfinite(s.adf)     ? QStringLiteral("вырожденный ряд")
                 : s.adf < s.adf_critical5 ? QStringLiteral("H₀ ADF отвергается")
                                           : QStringLiteral("H₀ ADF не отвергается"))
            .arg(fmt(s.kpss))
            .arg(s.kpss_lags)
            .arg(!std::isfinite(s.kpss) ? QStringLiteral("вырожденный ряд")
                 : s.kpss > .463        ? QStringLiteral("H₀ KPSS отвергается")
                                        : QStringLiteral("H₀ KPSS не отвергается")));
    show_distribution();
    acf_chart_->set_lines({{"ACF", QColor("#829eff"), points(r.acf), false},
                           {"PACF", QColor("#efc571"), points(r.pacf), false}},
                          QStringLiteral("ACF / PACF логдоходностей"),
                          QStringLiteral("PACF: Yule–Walker, без поправки ковариации; лаг 0 = 1"));
    const auto f = [](quant::Feature feature) { return static_cast<std::size_t>(feature); };
    const auto &v = r.features.back();
    feature_text_->setText(
        QStringLiteral("Индикаторы ПО СВЕЧАМ: ATR14=%1 ₽ • NATR14=%2% • Z-score(%3)=%4 • "
                       "RSI14=%5\nEMA/MACD/RSI/Bollinger используют только текущую и прошлые закрытые свечи. "
                       "Z-score использует population σ; при нулевом разбросе — недоступен.")
            .arg(fmt(v[f(quant::Feature::Atr)]), fmt(v[f(quant::Feature::Natr)]))
            .arg(window_->value())
            .arg(fmt(v[f(quant::Feature::Zscore)]), fmt(v[f(quant::Feature::Rsi)])));
    QVector<PlotLine> price_lines;
    const quant::Feature shown[]{quant::Feature::Close, quant::Feature::FastSma, quant::Feature::SlowSma,
                                 quant::Feature::Ema,   quant::Feature::Upper,   quant::Feature::Lower};
    const QStringList colors{"#39d2b0", "#efc571", "#829eff", "#b99fe8", "#79839e", "#79839e"};
    for (int j = 0; j < 6; ++j) {
        std::vector<double> values;
        for (const auto &row : r.features)
            values.push_back(row[f(shown[j])]);
        price_lines.push_back(
            {feature_names()[static_cast<int>(shown[j])], QColor(colors[j]), points(values), false});
    }
    price_chart_->set_lines(
        price_lines, selected_->currentText() + QStringLiteral(" • индикаторы закрытых свечей"),
        QStringLiteral("До прогрева значения не рисуются; ATR/NATR/RSI/MACD — в таблице ниже"));
    QVector<QStringList> feature_rows;
    const auto start = bars.size() > 100 ? bars.size() - 100 : 0;
    for (std::size_t i = start; i < bars.size(); ++i) {
        QStringList row{stamp(bars[i].time)};
        for (auto feature :
             {quant::Feature::Close, quant::Feature::FastSma, quant::Feature::SlowSma, quant::Feature::Ema,
              quant::Feature::Rsi, quant::Feature::Macd, quant::Feature::MacdSignal, quant::Feature::Upper,
              quant::Feature::Lower, quant::Feature::Atr, quant::Feature::Natr, quant::Feature::Zscore})
            row << fmt(r.features[i][f(feature)]);
        feature_rows.push_back(row);
    }
    cells(features_table_, feature_rows);
    QStringList names;
    for (const auto &kv : data_)
        if (kv.second.size() >= 3)
            names << kv.first;
    correlations_->setColumnCount(names.size());
    correlations_->setRowCount(names.size());
    correlations_->setHorizontalHeaderLabels(names);
    correlations_->setVerticalHeaderLabels(names);
    for (int i = 0; i < names.size(); ++i)
        for (int j = 0; j < names.size(); ++j) {
            const double value = r.correlation.matrix[i][j];
            auto *item = new QTableWidgetItem(fmt(value, 3));
            item->setToolTip(QStringLiteral("Общих интервалов: %1; минимум 3. Отсутствие вариации: —")
                                 .arg(r.correlation.common_returns));
            if (std::isfinite(value))
                item->setBackground(QColor(value >= 0 ? 35 : 100,
                                           value >= 0 ? static_cast<int>(60 + 80 * value) : 40,
                                           value >= 0 ? 75 : 65));
            correlations_->setItem(i, j, item);
        }
    const auto &m = r.simulation.metrics;
    return_card_->setText(fmt(m.total_return * 100, 2));
    drawdown_card_->setText(fmt(m.max_drawdown * 100, 2));
    fills_card_->setText(QString::number(m.fills));
    cost_card_->setText(fmt(m.fees + m.execution_cost, 2));
    report_->setText(
        QStringLiteral(
            "Чистый результат: %1% • максимум просадки по закрытиям: %2% • оборот / начальный капитал: "
            "%3\nИсполнений: %4 • закрытых позиций: %5 • выигрышных: %6% • profit factor: %7\nSharpe=%8 • "
            "Sortino=%9 (нулевая ставка, N=%10; 0=на свечу) • CAGR=%11% • Calmar=%12\nДержать с той же долей "
            "капитала: %13% • деньги без процентов: 0%\nWalk-forward: %14 полных окон, составной результат "
            "вне выборки: %15%\nОстановка по риску: %16 • незакрытых акций: %17. Стратегия: только long, без "
            "плеча; цены без дивидендных поправок.")
            .arg(fmt(m.total_return * 100), fmt(m.max_drawdown * 100), fmt(m.turnover))
            .arg(m.fills)
            .arg(m.closed_trades)
            .arg(fmt(m.win_rate * 100), fmt(m.profit_factor), fmt(m.sharpe), fmt(m.sortino))
            .arg(annual_->value())
            .arg(fmt(m.cagr * 100), fmt(m.calmar), fmt(r.baseline.metrics.total_return * 100))
            .arg(r.walk.folds.size())
            .arg(r.walk.folds.empty() ? QStringLiteral("недостаточно истории")
                                      : fmt(r.walk.compounded_return * 100))
            .arg(r.simulation.halted ? QStringLiteral("да") : QStringLiteral("нет"))
            .arg(r.simulation.units));
    report_->setText(report_->text() +
                     QStringLiteral("\nПлановых частичных исполнений: %1; назначенный остаток: %2 акций. Это "
                                    "части одной позиции, не отдельные закрытые сделки.")
                         .arg(m.partial_fills)
                         .arg(r.simulation.partial_remaining));
    equity_chart_->set_lines(
        {{QStringLiteral("Стратегия"), QColor("#39d2b0"), points(r.simulation.equity), false},
         {QStringLiteral("Держать"), QColor("#829eff"), points(r.baseline.equity), false},
         {QStringLiteral("Деньги"),
          QColor("#efc571"),
          {{0, r.simulation.equity.front()},
           {static_cast<double>(r.simulation.equity.size() - 1), r.simulation.equity.front()}},
          true}},
        QStringLiteral("Кривые капитала, ₽"),
        QStringLiteral("Все исполнения включают выбранные издержки; исходная точка — начальный капитал"));
    drawdown_chart_->set_lines(
        {{QStringLiteral("Просадка"), QColor("#f08c98"), points(r.simulation.drawdowns, -100), false}},
        QStringLiteral("Просадка стратегии, %"),
        QStringLiteral("По капиталу на закрытиях; внутрисвечная просадка может быть больше"));
    QVector<QStringList> fold_rows;
    for (const auto &fold : r.walk.folds)
        fold_rows.push_back({QStringLiteral("%1–%2").arg(fold.train_begin).arg(fold.test_begin - 1),
                             QStringLiteral("%1–%2").arg(fold.test_begin).arg(fold.test_end - 1),
                             QStringLiteral("%1 / %2").arg(fold.fast).arg(fold.slow),
                             fmt(fold.train_return * 100), fmt(fold.test_return * 100)});
    cells(folds_, fold_rows);
    backtest_journal_->setRowCount(0);
    backtest_journal_->setProperty("journal_total", 0);
    journal_rows(backtest_journal_, r.simulation);
    QVector<QPointF> markers;
    for (const auto &e : r.simulation.journal)
        if (e.quantity) {
            auto it = std::lower_bound(bars.begin(), bars.end(), e.time,
                                       [](const quant::Bar &b, qint64 t) { return b.time < t; });
            if (it != bars.end() && it->time == e.time)
                markers.push_back({static_cast<double>(it - bars.begin()), e.price});
        }
    price_chart_->set_markers(markers);
    update_journey();
}
void ResearchPanel::demo(bool hourly) {
    data_.clear();
    selected_->clear();
    diagnostics_.clear();
    synthetic_ = true;
    source_dirty_ = false;
    local_project_ = false;
    period_->setCurrentIndex(hourly ? 2 : 1);
    if (!hourly)
        higher_factor_->setCurrentIndex(0);
    const QStringList names{"SBER", "GAZP", "LKOH"};
    for (int k = 0; k < 3; ++k) {
        auto date = QDate::currentDate().addDays(-550);
        for (int i = 0; i < 360;) {
            date = date.addDays(1);
            if (!hourly && date.dayOfWeek() > 5)
                continue;
            const auto start =
                hourly ? QDateTime(QDate::currentDate().addDays(-20), QTime(0, 0), QTimeZone("Europe/Moscow"))
                                 .toMSecsSinceEpoch() +
                             i * 3600000LL
                       : QDateTime(date, QTime(0, 0), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
            const double open = (100 + k * 70) *
                                std::exp(.0005 * i + .035 * std::sin(i * .11 + k) + .009 * std::sin(i * .73)),
                         close = open * (1 + .005 * std::sin(i * .41 + k));
            data_[names[k]].push_back({start, start + (hourly ? 3600000 : 86400000), open,
                                       std::max(open, close) * 1.006, std::min(open, close) * .994, close,
                                       10000.0 + (i % 17) * 1000,
                                       QDateTime::fromMSecsSinceEpoch(start)
                                           .toTimeZone(QTimeZone("Europe/Moscow"))
                                           .offsetFromUtc()});
            ++i;
        }
        selected_->addItem(names[k]);
    }
    selected_->setCurrentIndex(0);
    quality_->setText(hourly ? QStringLiteral("ДЕМО H1: 3×360 синтетических часовых свечей, "
                                              "непрерывная условная сессия 24×7. Это не "
                                              "биржевой календарь и не реальные данные.")
                             : QStringLiteral("ДЕМО: 3×360 синтетических закрытых дневных свечей, "
                                              "условный календарь Пн–Пт. Это не биржевые данные."));
    result_current_ = false;
    update_metadata();
    update_journey();
    status_->setText(QStringLiteral("ДЕМО загружено. Нажмите «Проверить данные», "
                                    "затем следуйте шагам слева."));
}
void ResearchPanel::start_paper() {
    paper_timer_.stop();
    live_timer_.stop();
    live_loader_.cancel();
    live_paper_ = false;
    try {
        paper_bars_ = selected_bars();
        if (reserve_->isChecked())
            paper_bars_.resize(paper_bars_.size() * 8 / 10);
        quant::validate_bars(paper_bars_);
        auto config = settings();
        config.liquidate_end = false;
        paper_ = std::make_unique<quant::PaperEngine>(config);
        paper_parameters_ = configuration();
        paper_manual_stop_ = -1;
        paper_index_ = 0;
        journal_->setRowCount(0);
        journal_->setProperty("journal_total", 0);
        paper_source_ = selected_->currentText() + " • " +
                        (local_project_ ? QStringLiteral("ЛОКАЛЬНЫЙ ПРОЕКТ, НЕ ПРОВЕРЕН ОНЛАЙН")
                         : synthetic_   ? QStringLiteral("СИНТЕТИЧЕСКОЕ ДЕМО")
                                        : QStringLiteral("АРХИВ MOEX, С ЗАДЕРЖКОЙ"));
        update_paper();
    } catch (const std::exception &) {
        paper_.reset();
        paper_status_->setText(QStringLiteral("Сначала загрузите корректную историю и проверьте параметры."));
    }
}
void ResearchPanel::step_paper() {
    if (!paper_ || paper_index_ >= paper_bars_.size()) {
        paper_timer_.stop();
        return;
    }
    try {
        paper_->step(paper_bars_[paper_index_++]);
        if (paper_index_ == paper_bars_.size())
            paper_timer_.stop();
        update_paper();
    } catch (const std::exception &) {
        stop_paper(QStringLiteral("Ошибка данных или лимит сессии"));
    }
}
void ResearchPanel::stop_paper(const QString &why) {
    paper_timer_.stop();
    if (paper_) {
        paper_manual_stop_ = static_cast<int>(paper_index_);
        paper_->request_stop(why.toStdString());
        if (paper_index_ < paper_bars_.size()) {
            try {
                paper_->step(paper_bars_[paper_index_++]);
            } catch (const std::exception &) {
            }
        }
        update_paper();
    }
}
void ResearchPanel::update_paper() {
    paper_play_->setChecked(paper_timer_.isActive());
    paper_play_->setText(paper_timer_.isActive() ? QStringLiteral("Приостановить")
                                                 : QStringLiteral("Запустить автоматически"));
    paper_play_->setEnabled(paper_ && !live_paper_);
    paper_step_->setEnabled(paper_ && !live_paper_);
    if (!paper_)
        return;
    const auto r = paper_->result();
    paper_status_->setText(
        QStringLiteral("%1 • свеча %2/%3 • деньги %4 ₽ • акций %5 • нереализованный P&L %6 ₽\nКапитал %7 ₽ • "
                       "результат %8% • остановлен: %9. %10\nЖурнал: последние 500 событий. На конце истории "
                       "позиция остаётся открытой до следующей свечи/нового счёта.")
            .arg(paper_source_)
            .arg(paper_index_)
            .arg(paper_bars_.size())
            .arg(fmt(r.cash, 2))
            .arg(r.units)
            .arg(fmt(r.unrealized, 2), fmt(r.equity.back(), 2), fmt(r.metrics.total_return * 100))
            .arg(r.halted ? QStringLiteral("да") : QStringLiteral("нет"))
            .arg(r.halted
                     ? QStringLiteral("Причина: %1. Новые входы запрещены; выход при доступном открытии.")
                           .arg(r.halt_reason == "drawdown/equity limit"
                                    ? QStringLiteral("лимит просадки / капитала")
                                    : QString::fromStdString(r.halt_reason))
                     : QString()));
    paper_status_->setText(
        paper_status_->text() +
        QStringLiteral("\nЧастичных исполнений: %1; назначенный остаток частичного этапа: %2 акций.")
            .arg(r.metrics.partial_fills)
            .arg(r.partial_remaining));
    if (r.higher.completed || r.higher.progress || r.higher.breaks)
        paper_status_->setText(paper_status_->text() +
                               QStringLiteral("\nСтарший интервал: %1; SMA %2 / %3; полных блоков "
                                              "%4, разрывов %5.")
                                   .arg(r.higher.ready ? QStringLiteral("готов")
                                                       : QStringLiteral("ожидание полного блока / прогрев"))
                                   .arg(fmt(r.higher.fast), fmt(r.higher.slow))
                                   .arg(r.higher.completed)
                                   .arg(r.higher.breaks));
    paper_chart_->set_lines(
        {{QStringLiteral("Виртуальный капитал"), QColor("#39d2b0"), points(r.equity), false}},
        live_paper_ ? QStringLiteral("Новые закрытые свечи MOEX • задержанный тренажёр")
                    : QStringLiteral("Виртуальный счёт • проигрывание свечей"),
        QStringLiteral("Та же модель исполнения, что в бэктесте. Никаких запросов к торговому API."));
    journal_rows(journal_, r);
}
void ResearchPanel::set_test_view(int index) { tabs_->setCurrentIndex(std::clamp(index, 0, 8)); }
void ResearchPanel::navigate_to(int index, bool advanced) {
    if (advanced)
        expert_->setChecked(true);
    tabs_->setCurrentIndex(std::clamp(index, 0, 8));
}
void ResearchPanel::start_online_test() {
    symbols_->setText("SBER, GAZP");
    period_->setCurrentIndex(1);
    load_->click();
}
bool ResearchPanel::online_ready() {
    if (loading_ || data_.size() < 2 || selected_bars().size() < 40)
        return false;
    if (last_result_.simulation.equity.empty()) {
        if (!worker_->isRunning())
            calculate();
        return false;
    }
    return !worker_->isRunning() && last_result_.correlation.common_returns >= 3;
}
bool ResearchPanel::privacy_test() {
    cash_->setValue(999987.65);
    demo();
    start_paper();
    step_paper();
    clear_private_state();
    return data_.empty() && !paper_ && cash_->value() == 100000 && journal_->rowCount() == 0;
}
bool ResearchPanel::self_test() {
    cancel_work();
    expert_->setChecked(false);
    if (!journey_test())
        return false;
    demo();
    const auto bars = selected_bars();
    if (bars.size() != 360)
        return false;
    entry_expression_->setCurrentIndex(1);
    entry_rules_[0].enabled->setChecked(true);
    entry_rules_[1].enabled->setChecked(true);
    entry_rules_[2].enabled->setChecked(true);
    entry_rules_[3].enabled->setChecked(false);
    entry_rules_[1].open->setCurrentIndex(1);
    entry_rules_[2].close->setCurrentIndex(1);
    entry_rules_[2].join->setCurrentIndex(1);
    auto *preview = findChild<QLabel *>("entryFormulaPreview");
    if (!preview || !preview->text().contains(QStringLiteral("1 И (2 ИЛИ 3)")))
        return false;
    const auto nested = configuration();
    if (settings().strategy.entry.expression != configuration_settings(nested).strategy.entry.expression)
        return false;
    auto bad = nested;
    auto values = bad["values"].toObject();
    values[entry_rules_[2].close->objectName()] = 0;
    bad["values"] = values;
    if (apply_configuration(bad) || configuration() != nested)
        return false;
    entry_rules_[2].close->setCurrentIndex(0);
    const int attempts_before_error = attempts_;
    calculate();
    if (worker_->isRunning() || attempts_ != attempts_before_error ||
        !preview->text().contains(QStringLiteral("Ошибка скобок")))
        return false;
    entry_rules_[2].close->setCurrentIndex(1);
    const auto portfolio = portfolio_input();
    const auto visible = reserve_->isChecked() ? bars.size() * 8 / 10 : bars.size();
    if (portfolio.assets.size() != 3 || !portfolio.synthetic || !portfolio.metadata_known)
        return false;
    for (const auto &asset : portfolio.assets)
        for (const auto &bar : asset.bars)
            if (bar.end > bars[visible - 1].end)
                return false;
    std::vector<double> prices;
    for (const auto &b : bars)
        prices.push_back(b.close);
    LabResult r;
    r.distribution = quant::distribution(prices);
    r.stationarity = quant::stationarity(prices);
    r.acf = quant::acf(r.distribution.log_returns, 20);
    r.pacf = quant::pacf(r.distribution.log_returns, 20);
    r.features = quant::candle_features(bars);
    std::vector<std::vector<quant::Bar>> all;
    for (const auto &kv : data_)
        all.push_back(kv.second);
    r.correlation = quant::aligned_correlation(all);
    partial_target_->setValue(.1);
    partial_fraction_->setValue(50);
    const auto config = settings();
    r.simulation = quant::backtest(bars, config, 0, true);
    r.baseline = quant::buy_hold(bars, config);
    r.walk = quant::walk_forward(bars, config);
    r.explained_strategy = config.strategy;
    r.explained_config = config;
    last_result_ = r;
    present(r);
    explanation_bar_->setValue(1);
    explain_bar(1);
    if (!explanation_->text().contains(QStringLiteral("недостаточно наблюдений")))
        return false;
    explanation_bar_->setValue(100);
    const auto frozen_explanation = explanation_->text();
    if (!frozen_explanation.contains(QStringLiteral("ФАКТИЧЕСКАЯ МОДЕЛЬ ИСПОЛНЕНИЯ")))
        return false;
    const int old_fast = fast_->value();
    const int old_confirmation = entry_confirmation_->value();
    const double old_partial_target = partial_target_->value();
    partial_target_->setValue(99);
    entry_confirmation_->setValue(old_confirmation == 50 ? 1 : 50);
    fast_->setValue(old_fast + 1);
    explain_bar(100);
    if (explanation_->text() != frozen_explanation)
        return false;
    fast_->setValue(old_fast);
    entry_confirmation_->setValue(old_confirmation);
    partial_target_->setValue(old_partial_target);
    explain_bar(0);
    if (!explanation_->text().contains(QStringLiteral("Нет рассчитанной свечи")))
        return false;
    explain_bar(100);
    {
        QEventLoop loop;
        connect(worker_, &QFutureWatcher<LabResult>::finished, &loop, &QEventLoop::quit);
        tabs_->setCurrentIndex(1);
        next_->click();
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        if (worker_->isRunning() || !worker_->result().error.isEmpty() || tabs_->currentIndex() != 2 ||
            !result_current_)
            return false;
        expert_->setChecked(true);
        expert_->setChecked(false);
        if (!result_current_)
            return false;
    }
    {
        QEventLoop loop;
        connect(worker_, &QFutureWatcher<LabResult>::finished, &loop, &QEventLoop::quit);
        calculate();
        cancel_work();
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        if (worker_->isRunning())
            return false;
    }
    {
        auto *temporary = new ResearchPanel;
        temporary->demo();
        temporary->calculate();
        delete temporary;
    }
    if (!workbench_test())
        return false;
    start_paper();
    for (int i = 0; i < 79; ++i)
        paper_->step(paper_bars_[paper_index_++]);
    step_paper();
    stop_paper(QStringLiteral("Тест остановки"));
    status_->setText(
        QStringLiteral("ДЕМО лаборатории • проверки расчёта, отмены и виртуального счёта завершены"));
    return r.correlation.common_returns == 359 && r.walk.folds.size() > 0 && paper_ &&
           paper_->result().halted && features_table_->rowCount() == 100 && higher_test();
}
bool ResearchPanel::higher_test() {
    demo(true);
    higher_factor_->setCurrentIndex(1);
    higher_fast_->setValue(2);
    higher_slow_->setValue(3);
    higher_exit_->setChecked(true);
    const auto saved = configuration();
    if (!apply_configuration(saved) || settings().higher_factor != 2 || !settings().higher_confirm_exit)
        return false;
    auto legacy = saved;
    auto values = legacy.value("values").toObject();
    for (const auto *key : {"study_higher_factor", "study_higher_fast", "study_higher_slow", "study_higher_exit"})
        values.remove(key);
    legacy["values"] = values;
    if (!apply_configuration(legacy) || settings().higher_factor || settings().higher_fast != 2 ||
        settings().higher_slow != 5 || settings().higher_confirm_exit || !apply_configuration(saved))
        return false;
    auto bad = saved;
    values = saved.value("values").toObject();
    values["study_period"] = 1;
    bad["values"] = values;
    if (apply_configuration(bad) || configuration() != saved) return false;
    QEventLoop loop;
    connect(worker_, &QFutureWatcher<LabResult>::finished, &loop, &QEventLoop::quit);
    calculate();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (worker_->isRunning() || !worker_->result().error.isEmpty() || !result_current_ ||
        !last_result_.simulation.higher.ready || last_result_.simulation.trace.empty())
        return false;
    for (const auto &t : last_result_.simulation.trace)
        if (t.higher.last_end > t.time + 3600000)
            return false;
    explain_bar(100);
    const auto frozen = explanation_->text();
    higher_fast_->setValue(3);
    explain_bar(100);
    if (explanation_->text() != frozen)
        return false;
    higher_fast_->setValue(2);
    if (!report_html().contains(QStringLiteral("Старший интервал")))
        return false;
    start_paper();
    if (!paper_ || paper_bars_.size() < 80)
        return false;
    for (int i = 0; i < 79; ++i)
        paper_->step(paper_bars_[paper_index_++]);
    step_paper();
    if (!paper_->result().higher.ready)
        return false;
    stop_paper(QStringLiteral("Тест старшего интервала"));
    status_->setText(QStringLiteral("СИНТЕТИЧЕСКОЕ ДЕМО H1 • старший фильтр 2 часа проверен"));
    return true;
}
} // namespace live
