#include "research_panel.hpp"
#include "quote_parser.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardItemModel>
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
    return {QStringLiteral("Константа"),
            "Close",
            "SMA fast",
            "SMA slow",
            "EMA",
            "RSI",
            "MACD",
            "MACD signal",
            "Bollinger upper",
            "Bollinger lower",
            "ATR 14",
            "NATR 14",
            "Z-score",
            "Volume"};
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
    if (v.startsWith("BUY"))
        return QStringLiteral("Покупка на следующем открытии; остаток отменён");
    if (v.startsWith("SELL"))
        return QStringLiteral("Выход по сигналу на следующем открытии");
    if (v.startsWith("SIGNAL: entry"))
        return QStringLiteral("Условия входа выполнены; ждём следующую свечу");
    if (v.startsWith("SIGNAL: exit"))
        return QStringLiteral("Условия выхода выполнены; ждём следующую свечу");
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
ResearchPanel::ResearchPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    symbols_ = new QLineEdit("SBER, GAZP, LKOH");
    symbols_->setMaxLength(256);
    symbols_->setToolTip(QStringLiteral("До 6 инструментов TQBR. Для корреляций нужны общие интервалы."));
    period_ = new QComboBox;
    period_->addItems({QStringLiteral("День • 90 дней"), QStringLiteral("День • 365 дней"),
                       QStringLiteral("Час • 7 дней")});
    period_->setCurrentIndex(1);
    load_ = new QPushButton(QStringLiteral("Загрузить MOEX"));
    auto *cancel = new QPushButton(QStringLiteral("Отменить"));
    auto *demo_button = new QPushButton(QStringLiteral("ДЕМО лаборатории"));
    top->addWidget(symbols_, 2);
    top->addWidget(period_);
    top->addWidget(load_);
    top->addWidget(cancel);
    top->addWidget(demo_button);
    layout->addLayout(top);
    status_ = text(QStringLiteral(
        "Публичный MOEX ISS • без регистрации • до 6×5000 закрытых свечей • данные задержаны"));
    layout->addWidget(status_);
    auto *dates = new QHBoxLayout;
    custom_dates_ = new QCheckBox(QStringLiteral("Свои даты"));
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    from_ = new QDateEdit(today.addYears(-3));
    till_ = new QDateEdit(today);
    for (auto *w : {from_, till_}) {
        w->setCalendarPopup(true);
        w->setDisplayFormat("dd.MM.yyyy");
        w->setDateRange(QDate(1990, 1, 1), today);
    }
    dates->addWidget(custom_dates_);
    dates->addWidget(from_);
    dates->addWidget(till_);
    dates->addStretch();
    layout->addLayout(dates);
    auto *controls = new QHBoxLayout;
    selected_ = new QComboBox;
    window_ = integer(20, 3, 200);
    lags_ = integer(20, 1, 50);
    adf_lags_ = integer(2, 0, 10);
    kpss_lags_ = integer(4, 0, 50);
    calculate_ = new QPushButton(QStringLiteral("Рассчитать / проверить стратегию"));
    controls->addWidget(selected_, 1);
    controls->addWidget(text("Окно"));
    controls->addWidget(window_);
    controls->addWidget(text("ACF"));
    controls->addWidget(lags_);
    controls->addWidget(text("ADF lag"));
    controls->addWidget(adf_lags_);
    controls->addWidget(text("KPSS lag"));
    controls->addWidget(kpss_lags_);
    controls->addWidget(calculate_);
    layout->addLayout(controls);
    tabs_ = new QTabWidget;
    tabs_->addTab(analytics_page(), QStringLiteral("Аналитика"));
    tabs_->addTab(strategy_page(), QStringLiteral("Конструктор"));
    tabs_->addTab(report_page(), QStringLiteral("Бэктест и walk-forward"));
    tabs_->addTab(paper_page(), QStringLiteral("Виртуальный счёт"));
    auto *quality_page = new QWidget;
    auto *qlayout = new QVBoxLayout(quality_page);
    quality_ = text(QStringLiteral("История ещё не загружена."));
    live_quality_ = text(QStringLiteral("Поток котировок не подключён."));
    qlayout->addWidget(quality_);
    qlayout->addWidget(live_quality_);
    qlayout->addWidget(text(QStringLiteral(
        "UTC внутри расчётов, Europe/Moscow в таблицах. Дневные ключи — дата торговой свечи; часовые — "
        "точное начало интервала. Ничего не заполняется будущими значениями.\nРазрывы календаря могут быть "
        "штатными неторговыми периодами: встроенного полного календаря биржевых исключений нет. Корреляции "
        "используют пересечение одинаковых пар начала/конца доходности, а не растягивают одну доходность на "
        "разные интервалы.\nНа текущей дате дневная свеча исключается целиком; часовая — до завершения часа. "
        "Пропуски не превращаются в нулевые доходности. Цены не скорректированы на дивиденды/сплиты.")));
    qlayout->addStretch();
    tabs_->addTab(quality_page, QStringLiteral("Качество и время"));
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *page = tabs_->widget(i);
        const auto title = tabs_->tabText(i);
        tabs_->removeTab(i);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs_->insertTab(i, scroll, title);
    }
    tabs_->addTab(workbench_page(), QStringLiteral("Исследования и выпуск"));
    setup_workbench();
    tabs_->setCurrentIndex(0);
    layout->addWidget(tabs_, 1);
    worker_ = new QFutureWatcher<LabResult>(this);
    connect(worker_, &QFutureWatcher<LabResult>::finished, this, [this] {
        calculate_->setEnabled(true);
        if (cancel_ && cancel_->load())
            return;
        const auto result = worker_->result();
        if (!result.error.isEmpty()) {
            status_->setText(result.error);
            return;
        }
        last_result_ = result;
        present(result);
        record_run(result);
        status_->setText(synthetic_
                             ? QStringLiteral("ДЕМО • расчёт завершён, все цены синтетические")
                             : QStringLiteral("MOEX ISS • расчёт закрытых задержанных свечей завершён"));
    });
    connect(load_, &QPushButton::clicked, this, [this] {
        cancel_work();
        data_.clear();
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
        if (!loading_)
            cancel_work();
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
        connect(w, &QCheckBox::toggled, this, dirty);
    for (auto *w : findChildren<QComboBox *>())
        if (w != selected_ && w != period_ && w != distribution_mode_)
            connect(w, &QComboBox::currentIndexChanged, this, dirty);
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
    if (cancel_)
        cancel_->store(true);
    loader_.cancel();
    loading_ = false;
    load_->setEnabled(true);
    period_->setEnabled(true);
    paper_timer_.stop();
    live_timer_.stop();
    live_loader_.cancel();
    live_paper_ = false;
    reference_loader_.cancel();
    ++study_generation_;
    if (dividend_reply_)
        dividend_reply_->abort();
}
void ResearchPanel::clear_private_state() {
    cancel_work();
    data_.clear();
    runs_.clear();
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
    paper_status_->setText(QStringLiteral("Счёт очищен."));
    for (auto *t : {correlations_, folds_, journal_, features_table_, backtest_journal_})
        t->setRowCount(0);
    for (auto *c :
         {distribution_chart_, acf_chart_, price_chart_, equity_chart_, drawdown_chart_, paper_chart_})
        c->clear();
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
    load_->setEnabled(true);
    period_->setEnabled(true);
    for (const auto &kv : data_)
        if (kv.second.size() >= 3)
            selected_->addItem(kv.first);
    quality_->setText(diagnostics_.join('\n'));
    catalog_.load();
    preflight();
    status_->setText(QStringLiteral(
        "История загружена. Выберите инструмент и нажмите расчёт. Незавершённые свечи исключены."));
}
QWidget *ResearchPanel::analytics_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    statistics_ = text(QStringLiteral("Загрузите историю или ДЕМО лаборатории."));
    tests_ = text("");
    feature_text_ = text("");
    layout->addWidget(statistics_);
    distribution_mode_ = new QComboBox;
    distribution_mode_->addItems({QStringLiteral("Гистограмма простых доходностей, %"),
                                  QStringLiteral("Простые доходности, %"),
                                  QStringLiteral("Логарифмические доходности, %")});
    layout->addWidget(distribution_mode_);
    auto *charts = new QHBoxLayout;
    distribution_chart_ = new VisualChart;
    acf_chart_ = new VisualChart;
    charts->addWidget(distribution_chart_, 1);
    charts->addWidget(acf_chart_, 1);
    layout->addLayout(charts);
    layout->addWidget(feature_text_);
    layout->addWidget(tests_);
    price_chart_ = new VisualChart;
    layout->addWidget(price_chart_);
    features_table_ = table({QStringLiteral("Свеча МСК"), "Close", "SMA fast", "SMA slow", "EMA", "RSI",
                             "MACD", "Signal", "BB upper", "BB lower", "ATR", "NATR %", "Z-score"});
    layout->addWidget(features_table_);
    correlations_ = table({});
    layout->addWidget(
        text(QStringLiteral("Корреляции логдоходностей: только одинаковые интервалы для всех серий")));
    layout->addWidget(correlations_);
    connect(distribution_mode_, &QComboBox::currentIndexChanged, this, [this] { show_distribution(); });
    return page;
}
QWidget *ResearchPanel::rules_box(const QString &title, std::vector<RuleUi> &controls, QCheckBox *&all,
                                  bool entry) {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(text(title));
    all = new QCheckBox(QStringLiteral("Все включённые условия (иначе: хотя бы одно)"));
    all->setChecked(true);
    layout->addWidget(all);
    for (int i = 0; i < 2; ++i) {
        RuleUi r;
        r.enabled = new QCheckBox;
        r.enabled->setChecked(i == 0);
        r.lhs = new QComboBox;
        r.lhs->addItems(feature_names());
        if (auto *model = qobject_cast<QStandardItemModel *>(r.lhs->model()))
            model->item(0)->setEnabled(false);
        r.lhs->setCurrentIndex(i == 0 ? 2 : 5);
        r.op = new QComboBox;
        r.op->addItems({">", "<", QStringLiteral("Пересёк ↑"), QStringLiteral("Пересёк ↓")});
        r.op->setCurrentIndex(i == 0 ? (entry ? 2 : 3) : (entry ? 1 : 0));
        r.rhs = new QComboBox;
        r.rhs->addItems(feature_names());
        r.rhs->setCurrentIndex(i == 0 ? 3 : 0);
        r.value = number(entry ? 70 : 75, -1e9, 1e9, 4);
        r.value->setEnabled(i != 0);
        connect(r.rhs, &QComboBox::currentIndexChanged, r.value,
                [v = r.value](int index) { v->setEnabled(index == 0); });
        auto *row = new QHBoxLayout;
        row->addWidget(r.enabled);
        row->addWidget(r.lhs);
        row->addWidget(r.op);
        row->addWidget(r.rhs);
        row->addWidget(r.value);
        layout->addLayout(row);
        controls.push_back(r);
    }
    return page;
}
QWidget *ResearchPanel::strategy_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(text(QStringLiteral(
        "Только виртуальные покупки без плеча. Правило проверяется по закрытой свече; рыночное исполнение — "
        "на следующем открытии. При прогреве индикатора условие ложно.")));
    auto *periods = new QHBoxLayout;
    fast_ = integer(10, 2, 100);
    slow_ = integer(30, 3, 200);
    periods->addWidget(text("SMA fast"));
    periods->addWidget(fast_);
    periods->addWidget(text("SMA slow"));
    periods->addWidget(slow_);
    periods->addStretch();
    layout->addLayout(periods);
    layout->addWidget(rules_box(QStringLiteral("ВХОД"), entry_rules_, entry_all_, true));
    layout->addWidget(rules_box(QStringLiteral("ВЫХОД"), exit_rules_, exit_all_, false));
    auto *forms = new QHBoxLayout;
    auto *left = new QFormLayout;
    auto *right = new QFormLayout;
    cash_ = number(100000, 100, 1e12, 2);
    lot_ = integer(1, 1, 1000000);
    commission_ = number(.05, 0, 10, 4);
    fixed_ = number(0, 0, 100000, 2);
    spread_ = number(2, 0, 500);
    slippage_ = number(2, 0, 500);
    allocation_ = number(20, .1, 100);
    participation_ = number(0, 0, 100);
    left->addRow(QStringLiteral("Виртуальный капитал, ₽"), cash_);
    left->addRow(QStringLiteral("Лот (проверьте у источника)"), lot_);
    left->addRow(QStringLiteral("Комиссия за сторону, %"), commission_);
    left->addRow(QStringLiteral("Фикс. комиссия за исполнение, ₽"), fixed_);
    left->addRow(QStringLiteral("Полный спред, б.п."), spread_);
    left->addRow(QStringLiteral("Проскальзывание за сторону, б.п."), slippage_);
    left->addRow(QStringLiteral("Макс. доля капитала, %"), allocation_);
    left->addRow(QStringLiteral("Лимит от объёма ПРЕДЫДУЩЕЙ свечи, % (0=нет)"), participation_);
    stop_ = number(2, 0, 99);
    target_ = number(4, 0, 1000);
    risk_ = number(1, .01, 100);
    max_dd_ = number(20, .01, 100);
    daily_ = number(5, .01, 100);
    max_entries_ = integer(10, 1, 1000);
    ambiguity_ = new QComboBox;
    ambiguity_->addItems(
        {QStringLiteral("Консервативно: сначала стоп"), QStringLiteral("Оптимистично: сначала цель")});
    annual_ = integer(0, 0, 100000);
    right->addRow(QStringLiteral("Стоп, % (0=нет)"), stop_);
    right->addRow(QStringLiteral("Цель, % (0=нет)"), target_);
    right->addRow(QStringLiteral("Модель риска входа, % капитала"), risk_);
    right->addRow(QStringLiteral("Аварийная просадка, %"), max_dd_);
    right->addRow(QStringLiteral("Лимит потери сессии, %"), daily_);
    right->addRow(QStringLiteral("Макс. входов в сессию"), max_entries_);
    right->addRow(QStringLiteral("Стоп и цель внутри одной свечи"), ambiguity_);
    right->addRow(QStringLiteral("Годовой масштаб Sharpe N (0=на свечу)"), annual_);
    forms->addLayout(left, 1);
    forms->addLayout(right, 1);
    layout->addLayout(forms);
    layout->addWidget(text(QStringLiteral(
        "Сессия риска — календарная дата Europe/Moscow, гэп включён относительно предыдущего капитала. "
        "Лимиты проверяются на закрытии, выход откладывается до следующего доступного открытия: они не "
        "гарантируют максимальный убыток. Стоп/цель — рыночные триггеры в модели. Лимит объёма использует "
        "только прошлую свечу и не воспроизводит очередь биржи. Нулевая ставка сравнения доходности. N "
        "задаётся явно; по умолчанию Sharpe/Sortino на свечу, без годового пересчёта.")));
    return page;
}
QWidget *ResearchPanel::report_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    train_ = integer(120, 60, 300);
    test_ = integer(40, 10, 100);
    row->addWidget(text(QStringLiteral("Walk-forward: обучение, свечей")));
    row->addWidget(train_);
    row->addWidget(text(QStringLiteral("Проверка, свечей")));
    row->addWidget(test_);
    row->addStretch();
    layout->addLayout(row);
    report_ = text(QStringLiteral("Нажмите общий расчёт после настройки правил."));
    layout->addWidget(report_);
    equity_chart_ = new VisualChart;
    drawdown_chart_ = new VisualChart;
    layout->addWidget(equity_chart_);
    layout->addWidget(drawdown_chart_);
    folds_ = table({QStringLiteral("Обучение"), QStringLiteral("Тест"), "Fast / slow",
                    QStringLiteral("Обучение, %"), QStringLiteral("Вне выборки, %")});
    layout->addWidget(folds_);
    backtest_journal_ = table({QStringLiteral("Свеча МСК"), QStringLiteral("Акций ±"), QStringLiteral("Цена"),
                               QStringLiteral("Деньги"), QStringLiteral("Журнал бэктеста: причина")});
    layout->addWidget(backtest_journal_);
    layout->addWidget(text(QStringLiteral(
        "Сетка walk-forward: fast {5,10,20}, slow {20,40,60}, fast<slow; выбирается чистая доходность "
        "обучения после издержек. Другие правила фиксированы. Каждый тест начинается без позиции и "
        "заканчивается ликвидацией; капитал переносится между тестами. Остаток истории короче окна теста не "
        "используется. Это не независимый финальный holdout при многократном ручном подборе.\nБаза «держать» "
        "использует ту же долю капитала, комиссии и спред; покупка на первом открытии, без стопов и лимитов "
        "стратегии. Денежная база без процентов. CAGR/Calmar показываются только при горизонте не менее 30 "
        "календарных дней; короткая годовая экстраполяция ненадёжна.")));
    return page;
}
QWidget *ResearchPanel::paper_page() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(
        text(QStringLiteral("ВИРТУАЛЬНЫЙ СЧЁТ • проигрывание загруженных закрытых свечей • реальных денег и "
                            "торговых API нет. Параметры фиксируются при открытии счёта.")));
    auto *controls = new QHBoxLayout;
    auto *start = new QPushButton(QStringLiteral("Новый счёт"));
    auto *step = new QPushButton(QStringLiteral("Следующая свеча"));
    auto *play = new QPushButton(QStringLiteral("Авто / пауза"));
    auto *kill = new QPushButton(QStringLiteral("АВАРИЙНЫЙ СТОП"));
    kill->setStyleSheet("background:#703645;");
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
    connect(play, &QPushButton::clicked, this, [this] {
        if (paper_timer_.isActive())
            paper_timer_.stop();
        else if (paper_)
            paper_timer_.start();
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
    auto rules = [](const std::vector<RuleUi> &fields, QCheckBox *all) {
        quant::RuleGroup group;
        group.all = all->isChecked();
        for (const auto &r : fields)
            group.rules.push_back({static_cast<quant::Feature>(r.lhs->currentIndex()),
                                   static_cast<quant::Feature>(r.rhs->currentIndex()),
                                   static_cast<quant::Compare>(r.op->currentIndex()), r.value->value(),
                                   r.enabled->isChecked()});
        return group;
    };
    c.strategy.entry = rules(entry_rules_, entry_all_);
    c.strategy.exit = rules(exit_rules_, exit_all_);
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
    return c;
}
std::vector<quant::Bar> ResearchPanel::selected_bars() const {
    const auto it = data_.find(selected_->currentText());
    return it == data_.end() ? std::vector<quant::Bar>{} : it->second;
}
void ResearchPanel::calculate() {
    if (loading_ || worker_->isRunning())
        return;
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
        status_->setText(QStringLiteral("Проверьте параметры: SMA fast должна быть меньше slow."));
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
    status_->setText(QStringLiteral("Расчёт в фоновом потоке…"));
    worker_->setFuture(
        QtConcurrent::run([bars, complete_bars, series, config, window, lags, adf, kpss, train, test,
                           cancelled, parameters, hash, source, holdout, split, embargo] {
            LabResult r;
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
                r.simulation = quant::backtest(bars, config);
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
}
void ResearchPanel::demo() {
    data_.clear();
    selected_->clear();
    diagnostics_.clear();
    synthetic_ = true;
    local_project_ = false;
    period_->setCurrentIndex(1);
    const QStringList names{"SBER", "GAZP", "LKOH"};
    for (int k = 0; k < 3; ++k) {
        auto date = QDate::currentDate().addDays(-550);
        for (int i = 0; i < 360;) {
            date = date.addDays(1);
            if (date.dayOfWeek() > 5)
                continue;
            const auto start = QDateTime(date, QTime(0, 0), QTimeZone("Europe/Moscow")).toMSecsSinceEpoch();
            const double open = (100 + k * 70) *
                                std::exp(.0005 * i + .035 * std::sin(i * .11 + k) + .009 * std::sin(i * .73)),
                         close = open * (1 + .005 * std::sin(i * .41 + k));
            data_[names[k]].push_back({start, start + 86400000, open, std::max(open, close) * 1.006,
                                       std::min(open, close) * .994, close, 10000.0 + (i % 17) * 1000,
                                       QDateTime::fromMSecsSinceEpoch(start)
                                           .toTimeZone(QTimeZone("Europe/Moscow"))
                                           .offsetFromUtc()});
            ++i;
        }
        selected_->addItem(names[k]);
    }
    quality_->setText(QStringLiteral("ДЕМО: 3×360 синтетических закрытых дневных свечей, условный календарь "
                                     "Пн–Пт. Это не биржевые данные."));
    status_->setText(QStringLiteral("ДЕМО готово. Настройте правила и нажмите расчёт."));
}
void ResearchPanel::start_paper() {
    paper_timer_.stop();
    live_timer_.stop();
    live_loader_.cancel();
    live_paper_ = false;
    try {
        paper_bars_ = selected_bars();
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
        update_paper();
        if (paper_index_ == paper_bars_.size())
            paper_timer_.stop();
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
    paper_chart_->set_lines(
        {{QStringLiteral("Виртуальный капитал"), QColor("#39d2b0"), points(r.equity), false}},
        live_paper_ ? QStringLiteral("Новые закрытые свечи MOEX • задержанный тренажёр")
                    : QStringLiteral("Виртуальный счёт • проигрывание свечей"),
        QStringLiteral("Та же модель исполнения, что в бэктесте. Никаких запросов к торговому API."));
    journal_rows(journal_, r);
}
void ResearchPanel::set_test_view(int index) { tabs_->setCurrentIndex(std::clamp(index, 0, 5)); }
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
    demo();
    const auto bars = selected_bars();
    if (bars.size() != 360)
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
    const auto config = settings();
    r.simulation = quant::backtest(bars, config);
    r.baseline = quant::buy_hold(bars, config);
    r.walk = quant::walk_forward(bars, config);
    last_result_ = r;
    present(r);
    {
        QEventLoop loop;
        connect(worker_, &QFutureWatcher<LabResult>::finished, &loop, &QEventLoop::quit);
        calculate();
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        if (worker_->isRunning() || !worker_->result().error.isEmpty())
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
           paper_->result().halted && features_table_->rowCount() == 100;
}
} // namespace live
