#include "portfolio_panel.hpp"
#include <QDateTime>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <map>
namespace live {
namespace {
QString n(double v) { return std::isfinite(v) ? QString::number(v, 'f', 2) : QStringLiteral("—"); }
QDoubleSpinBox *decimal(double value, double low, double high, int digits = 2) {
    auto *w = new QDoubleSpinBox;
    w->setDecimals(digits);
    w->setRange(low, high);
    w->setValue(value);
    w->setKeyboardTracking(false);
    return w;
}
QSpinBox *integer(int value, int low, int high) {
    auto *w = new QSpinBox;
    w->setRange(low, high);
    w->setValue(value);
    w->setKeyboardTracking(false);
    return w;
}
QTableWidget *table(const QStringList &headers) {
    auto *w = new QTableWidget(0, headers.size());
    w->setHorizontalHeaderLabels(headers);
    w->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    w->horizontalHeader()->setStretchLastSection(true);
    w->setEditTriggers(QAbstractItemView::NoEditTriggers);
    w->setMinimumHeight(160);
    return w;
}
void row(QTableWidget *w, const QStringList &cells) {
    int at = w->rowCount();
    w->insertRow(at);
    for (int j = 0; j < cells.size(); ++j)
        w->setItem(at, j, new QTableWidgetItem(cells[j]));
}
PlotLine line(const std::vector<double> &values, const QString &name, const QColor &color) {
    PlotLine out{name, color, {}, false};
    const auto stride = std::max<std::size_t>(1, (values.size() + 498) / 499);
    for (std::size_t i = 0; i < values.size(); i += stride)
        out.points.push_back({static_cast<double>(i), values[i]});
    if (!values.empty() && (values.size() - 1) % stride)
        out.points.push_back({static_cast<double>(values.size() - 1), values.back()});
    return out;
}
QString reason(const std::string &s) {
    const std::map<std::string, QString> names{
        {"RULE_ON", QStringLiteral("Правило входа включило целевую долю")},
        {"RULE_OFF", QStringLiteral("Правило выхода выключило целевую долю")},
        {"TARGET_NEXT_OPEN",
         QStringLiteral("Целевые количества зафиксированы для следующего общего открытия")},
        {"BUY_REBALANCE", QStringLiteral("Покупка по плану, с учётом общих денег и лимитов")},
        {"SELL_REBALANCE", QStringLiteral("Продажа по плану ребалансировки")},
        {"BUY_BLOCKED_CASH_LIMIT", QStringLiteral("Покупка не исполнена: деньги, лот или лимит веса")},
        {"SELL_BLOCKED_FEE", QStringLiteral("Выход не исполнен: не хватает средств на комиссию")},
        {"SELL_BLOCKED_PRICE", QStringLiteral("Выход не исполнен: после округления нет положительной цены")},
        {"RISK_EXIT", QStringLiteral("Выход после остановки риска")},
        {"DRAWDOWN", QStringLiteral("Лимит просадки: выход при следующем общем открытии")},
        {"DAILY_LOSS", QStringLiteral("Лимит дневного убытка по МСК: выход при следующем общем открытии")},
        {"MANUAL_STOP", QStringLiteral("Учебная ручная остановка: выход при следующем общем открытии")}};
    const auto i = names.find(s);
    return i == names.end() ? QString::fromStdString(s) : i->second;
}
} // namespace
PortfolioPanel::PortfolioPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(ui::paragraph(QStringLiteral("Портфель • только виртуальный общий счёт • без плеча")));
    layout->addWidget(ui::paragraph(QStringLiteral("Целевые доли или правила SMA для нескольких акций. "
                                                   "Сигнал рассчитывается на "
                                                   "закрытии, сделки — на следующем общем открытии. Продажи "
                                                   "выполняются первыми, покупки "
                                                   "используют один остаток денег в порядке тикеров.")));
    auto *actions = new QHBoxLayout;
    demo_ = new QPushButton(QStringLiteral("Открыть ДЕМО портфеля"));
    import_ = new QPushButton(QStringLiteral("Взять свечи из исследования"));
    equal_ = new QPushButton(QStringLiteral("Распределить поровну"));
    for (auto *b : {demo_, import_, equal_})
        actions->addWidget(b);
    layout->addLayout(actions);
    auto *files = new QHBoxLayout;
    save_ = new QPushButton(QStringLiteral("Сохранить портфель…"));
    open_ = new QPushButton(QStringLiteral("Открыть портфель…"));
    files->addWidget(save_);
    files->addWidget(open_);
    files->addStretch();
    layout->addLayout(files);
    layout->addWidget(ui::paragraph(QStringLiteral("Файл .qpp.json содержит цены, виртуальный капитал и "
                                                   "настройки. Сохраняется только по вашей "
                                                   "команде, без паролей и токенов. Открывается без сети; "
                                                   "результат нужно рассчитать заново.")));
    source_ = ui::paragraph(QStringLiteral("Начните с синтетического ДЕМО или загрузите закрытые "
                                           "свечи в «Исследование»."));
    source_->setTextFormat(Qt::PlainText);
    layout->addWidget(source_);
    layout->addWidget(ui::paragraph(QStringLiteral("1. Состав и настройки")));
    assets_ = table({QStringLiteral("Вкл."), QStringLiteral("Акция"), QStringLiteral("Доля, %"),
                     QStringLiteral("Лот"), QStringLiteral("Шаг, ₽"), QStringLiteral("Группа"),
                     QStringLiteral("Режим"), QStringLiteral("Свечей")});
    layout->addWidget(assets_);
    auto *form = new QFormLayout;
    capital_ = decimal(100000, 1000, 1e12);
    period_ = integer(20, 0, 5000);
    form->addRow(QStringLiteral("Начальный общий капитал, ₽"), capital_);
    form->addRow(QStringLiteral("Ребалансировка через общие свечи (0 — без периодической)"), period_);
    layout->addLayout(form);
    auto *options = new QWidget;
    auto *advanced = new QFormLayout(options);
    invested_ = decimal(80, 1, 100);
    weight_limit_ = decimal(40, 1, 100);
    group_limit_ = decimal(0, 0, 100);
    commission_ = decimal(.05, 0, 10, 3);
    fixed_ = decimal(0, 0, 1e9);
    spread_ = decimal(2, 0, 1000);
    slippage_ = decimal(2, 0, 1000);
    drawdown_ = decimal(20, .1, 100);
    daily_ = decimal(5, .1, 100);
    fast_ = integer(10, 2, 499);
    slow_ = integer(30, 3, 500);
    manual_stop_ = integer(0, 0, 5000);
    advanced->addRow(QStringLiteral("Максимальная вложенная доля, %"), invested_);
    advanced->addRow(QStringLiteral("Максимальный вес акции, %"), weight_limit_);
    advanced->addRow(QStringLiteral("Лимит ручной группы, % (0 — выключен)"), group_limit_);
    advanced->addRow(QStringLiteral("Комиссия за исполнение, %"), commission_);
    advanced->addRow(QStringLiteral("Фиксированный сбор за исполнение, ₽"), fixed_);
    advanced->addRow(QStringLiteral("Спред, б.п."), spread_);
    advanced->addRow(QStringLiteral("Проскальзывание, б.п."), slippage_);
    advanced->addRow(QStringLiteral("Остановка при просадке, %"), drawdown_);
    advanced->addRow(QStringLiteral("Дневной убыток по МСК, %"), daily_);
    advanced->addRow(QStringLiteral("Быстрая SMA выбранных строк"), fast_);
    advanced->addRow(QStringLiteral("Медленная SMA выбранных строк"), slow_);
    advanced->addRow(QStringLiteral("Учебный стоп после N закрытых свечей (0 — нет)"), manual_stop_);
    manual_metadata_ = new QCheckBox(QStringLiteral("Разрешить ручные лоты/шаги и подтвердить их проверку"));
    advanced->addRow(manual_metadata_);
    layout->addWidget(ui::fold(QStringLiteral("Издержки, ограничения риска и настройки SMA"), options));
    auto *run_row = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Рассчитать портфель"));
    cancel_ = new QPushButton(QStringLiteral("Отменить расчёт"));
    cancel_->setEnabled(false);
    run_row->addWidget(run_);
    run_row->addWidget(cancel_);
    run_row->addStretch();
    layout->addLayout(run_row);
    status_ = ui::paragraph({});
    summary_ = ui::paragraph({});
    summary_->setTextFormat(Qt::PlainText);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    layout->addWidget(ui::paragraph(QStringLiteral("2. Результат общего счёта")));
    layout->addWidget(summary_);
    equity_ = new VisualChart;
    drawdowns_ = new VisualChart;
    for (auto *c : {equity_, drawdowns_})
        c->setToolTip(VisualChart::interaction_help());
    connect(equity_, &VisualChart::range_changed, drawdowns_, &VisualChart::set_range);
    connect(drawdowns_, &VisualChart::range_changed, equity_, &VisualChart::set_range);
    connect(equity_, &VisualChart::cursor_changed, drawdowns_, &VisualChart::set_cursor);
    connect(drawdowns_, &VisualChart::cursor_changed, equity_, &VisualChart::set_cursor);
    layout->addWidget(equity_);
    layout->addWidget(ui::fold(QStringLiteral("Просадка портфеля"), drawdowns_));
    positions_ = table({QStringLiteral("Акция"), QStringLiteral("Акций"), QStringLiteral("Стоимость, ₽"),
                        QStringLiteral("Вес, %"), QStringLiteral("Вклад в результат, ₽"),
                        QStringLiteral("Комиссии, ₽"), QStringLiteral("Исключено свечей")});
    layout->addWidget(positions_);
    journal_ = table({QStringLiteral("Время UTC"), QStringLiteral("Акция"), QStringLiteral("Количество ±"),
                      QStringLiteral("Цена, ₽"), QStringLiteral("Комиссия, ₽"),
                      QStringLiteral("Общие деньги, ₽"), QStringLiteral("Причина")});
    layout->addWidget(ui::fold(
        QStringLiteral("Журнал: первые 10000 событий, на экране последние 500 сохранённых"), journal_));
    layout->addWidget(
        ui::fold(QStringLiteral("Как читать результат и ограничения модели"),
                 ui::paragraph(QStringLiteral("Используются только точные общие начала/концы свечей; "
                                              "остальные интервалы исключаются, без "
                                              "заполнения. Целевые количества округляются по лотам на "
                                              "закрытии. На следующем общем открытии "
                                              "гэпы и издержки могут уменьшить исполнение; остаток "
                                              "отменяется. Сумма неактивных долей остаётся "
                                              "деньгами. Группы — ручные, это не проверенная отраслевая "
                                              "классификация. Лимиты веса "
                                              "ограничивают новые увеличения позиции; движение цен может "
                                              "изменить существующие веса. Лимиты "
                                              "просадки/сессии наблюдаются на закрытиях и не гарантируют "
                                              "размер убытка. Остановка запрещает "
                                              "новые входы; выход ждёт следующего общего открытия. Если "
                                              "данных больше нет или комиссии/цена "
                                              "мешают выходу, позиция остаётся в отчёте. Итог — оценка по "
                                              "последним закрытиям, без выдуманной "
                                              "финальной продажи. Во всех отмеченных строках применяется "
                                              "одна пара SMA; неотмеченные строки "
                                              "сохраняют целевые доли. Объём торгов не ограничивает "
                                              "исполнение, биржевая очередь не "
                                              "моделируется. Индивидуальных стопов, плеча, налогов, "
                                              "дивидендов и реальных заявок в этом "
                                              "режиме нет. База — отдельный счёт равных долей с теми же "
                                              "издержками и ограничениями веса/денег, "
                                              "без повторных ребалансировок и остановок просадки/сессии. "
                                              "Годовые Sharpe/Sortino/Calmar здесь "
                                              "не рассчитываются."))));
    layout->addStretch();
    worker_ = new QFutureWatcher<PortfolioOutcome>(this);
    file_worker_ = new QFutureWatcher<PortfolioFileOutcome>(this);
    guard_ = new ui::WheelGuard(this);
    ui::prepare_controls(this);
    connect(demo_, &QPushButton::clicked, this, [this] { demo(); });
    connect(equal_, &QPushButton::clicked, this, [this] { equal_weights(); });
    connect(run_, &QPushButton::clicked, this, [this] { calculate(); });
    connect(save_, &QPushButton::clicked, this, [this] { project_file(true); });
    connect(open_, &QPushButton::clicked, this, [this] { project_file(false); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (cancelled_)
            cancelled_->store(true);
        status_->setText(file_worker_->isRunning()
                             ? QStringLiteral("Операция с файлом отменена. Уже завершённое сохранение не "
                                              "удаляется; текущий портфель остаётся в памяти.")
                             : QStringLiteral("Расчёт отменён. Это отмена исследования, а не закрытие "
                                              "виртуальных позиций."));
    });
    connect(manual_metadata_, &QCheckBox::toggled, this, [this](bool on) {
        for (auto &r : rows_) {
            r.lot->setReadOnly(!on);
            r.tick->setReadOnly(!on);
        }
        invalidate();
    });
    for (auto *w : findChildren<QDoubleSpinBox *>())
        connect(w, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { invalidate(); });
    for (auto *w : findChildren<QSpinBox *>())
        connect(w, qOverload<int>(&QSpinBox::valueChanged), this, [this] { invalidate(); });
    connect(worker_, &QFutureWatcher<PortfolioOutcome>::finished, this, [this] {
        auto out = worker_->future().takeResult();
        busy(false);
        if (cancelled_ && cancelled_->load())
            return;
        if (!out.error.isEmpty()) {
            status_->setText(out.error);
            return;
        }
        present(out);
    });
    connect(file_worker_, &QFutureWatcher<PortfolioFileOutcome>::finished, this, [this] {
        auto out = file_worker_->future().takeResult();
        busy(false);
        if (cancelled_ && cancelled_->load())
            return;
        if (!out.error.isEmpty()) {
            status_->setText(out.error);
            return;
        }
        if (out.saved) {
            status_->setText(QStringLiteral("Портфель сохранён. Файл содержит свечи и виртуальный капитал; "
                                            "очистка приложения не удаляет явно сохранённые файлы."));
        } else {
            apply_project(std::move(out.project));
        }
    });
    busy(false);
}
PortfolioPanel::~PortfolioPanel() {
    if (cancelled_)
        cancelled_->store(true);
}
void PortfolioPanel::busy(bool on) {
    assets_->setEnabled(!on);
    for (auto *w : findChildren<QAbstractSpinBox *>())
        w->setEnabled(!on);
    manual_metadata_->setEnabled(!on);
    run_->setEnabled(!on && !input_.assets.empty());
    import_->setEnabled(!on);
    demo_->setEnabled(!on);
    open_->setEnabled(!on);
    save_->setEnabled(!on && !input_.assets.empty());
    equal_->setEnabled(!on && !rows_.empty());
    cancel_->setEnabled(on);
}
bool PortfolioPanel::working() const { return worker_->isRunning() || file_worker_->isRunning(); }
void PortfolioPanel::invalidate() {
    if (working())
        return;
    summary_->clear();
    equity_->clear();
    drawdowns_->clear();
    positions_->setRowCount(0);
    journal_->setRowCount(0);
    status_->setText(QStringLiteral("Настройки изменены. Рассчитайте портфель заново."));
}
void PortfolioPanel::clear_private_state() {
    if (cancelled_)
        cancelled_->store(true);
    input_ = {};
    rows_.clear();
    assets_->setRowCount(0);
    positions_->setRowCount(0);
    journal_->setRowCount(0);
    equity_->clear();
    drawdowns_->clear();
    summary_->clear();
    source_->setText(QStringLiteral("Данные портфеля очищены."));
    status_->clear();
    capital_->setValue(100000);
    commission_->setValue(.05);
    fixed_->setValue(0);
    spread_->setValue(2);
    slippage_->setValue(2);
    invested_->setValue(80);
    weight_limit_->setValue(40);
    group_limit_->setValue(0);
    drawdown_->setValue(20);
    daily_->setValue(5);
    period_->setValue(20);
    fast_->setValue(10);
    slow_->setValue(30);
    manual_stop_->setValue(0);
    manual_metadata_->setChecked(false);
    status_->clear();
    if (!working())
        busy(false);
}
void PortfolioPanel::set_input(PortfolioInput input) {
    if (cancelled_)
        cancelled_->store(true);
    input_ = std::move(input);
    rows_.clear();
    assets_->setRowCount(0);
    manual_metadata_->setChecked(false);
    source_->setText(input_.source +
                     QStringLiteral(" • независимая копия данных; последующие изменения "
                                    "исследования не меняют этот набор. ") +
                     (input_.synthetic ? QStringLiteral("Лоты/шаги — учебные допущения.")
                                       : QStringLiteral("Лоты/шаги из каталога или ручного ввода; "
                                                        "исторические изменения не восстановлены.")));

    for (const auto &a : input_.assets) {
        const int i = assets_->rowCount();
        assets_->insertRow(i);
        AssetRow r;
        r.enabled = new QCheckBox;
        r.enabled->setChecked(a.bars.size() >= 2);
        r.rules = new QComboBox;
        r.rules->addItems({QStringLiteral("Целевые доли"), QStringLiteral("SMA: общие окна"),
                           QStringLiteral("SMA 5/20"), QStringLiteral("SMA 20/60"),
                           QStringLiteral("RSI 14: 30/70")});
        r.rules->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        r.rules->setMinimumContentsLength(13);
        r.rules->setToolTip(r.rules->currentText());
        r.rules->setFocusPolicy(Qt::StrongFocus);
        r.rules->installEventFilter(guard_);
        r.weight = decimal(std::floor(std::min(.8 / input_.assets.size(), .4) * 10000) / 100, 0, 100);
        r.lot = integer(static_cast<int>(a.lot), 1, 1000000);
        r.tick = decimal(a.tick_size, 0, 1e9, 8);
        r.tick->setFixedWidth(130);
        r.group = new QLineEdit;
        r.group->setMaxLength(40);
        r.group->setPlaceholderText(QStringLiteral("Не задана"));
        r.group->setText(QString::fromStdString(a.group));
        r.lot->setReadOnly(true);
        r.tick->setReadOnly(true);
        assets_->setCellWidget(i, 0, r.enabled);
        assets_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(a.symbol)));
        assets_->setCellWidget(i, 2, r.weight);
        assets_->setCellWidget(i, 3, r.lot);
        assets_->setCellWidget(i, 4, r.tick);
        assets_->setCellWidget(i, 5, r.group);
        assets_->setCellWidget(i, 6, r.rules);
        assets_->setItem(i, 7, new QTableWidgetItem(QString::number(a.bars.size())));
        for (auto *w : {static_cast<QWidget *>(r.weight), static_cast<QWidget *>(r.lot),
                        static_cast<QWidget *>(r.tick)}) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(guard_);
            w->setAccessibleName(QString::fromStdString(a.symbol) + QStringLiteral(" • параметр позиции"));
        }
        const auto symbol = QString::fromStdString(a.symbol);
        r.enabled->setAccessibleName(QStringLiteral("Включить %1 в портфель").arg(symbol));
        r.weight->setAccessibleName(symbol + QStringLiteral(" • целевая доля, %"));
        r.lot->setAccessibleName(symbol + QStringLiteral(" • лот"));
        r.tick->setAccessibleName(symbol + QStringLiteral(" • шаг цены"));
        r.rules->setAccessibleName(symbol + QStringLiteral(" • режим стратегии"));
        r.group->setAccessibleName(symbol + QStringLiteral(" • ручная группа"));
        for (auto *w : {static_cast<QWidget *>(r.lot), static_cast<QWidget *>(r.tick)})
            w->setToolTip(QStringLiteral("Для ручного изменения раскройте настройки издержек и риска и "
                                         "включите ручную проверку лотов/шагов."));
        assets_->setRowHeight(i, std::max({r.weight->sizeHint().height(), r.lot->sizeHint().height(),
                                           r.tick->sizeHint().height(), r.rules->sizeHint().height(),
                                           r.group->sizeHint().height()}) +
                                     4);
        connect(r.enabled, &QCheckBox::toggled, this, [this] { invalidate(); });
        connect(r.rules, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, combo = r.rules] {
            combo->setToolTip(combo->currentText());
            invalidate();
        });
        connect(r.weight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { invalidate(); });
        connect(r.tick, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { invalidate(); });
        connect(r.lot, qOverload<int>(&QSpinBox::valueChanged), this, [this] { invalidate(); });
        connect(r.group, &QLineEdit::textChanged, this, [this] { invalidate(); });
        rows_.push_back(r);
    }
    int height = assets_->horizontalHeader()->sizeHint().height() + 4;
    for (int i = 0; i < assets_->rowCount(); ++i)
        height += assets_->rowHeight(i);
    assets_->setMinimumHeight(std::max(160, height));
    invalidate();
    if (!working())
        busy(false);
}
void PortfolioPanel::equal_weights() {
    int count = 0;
    std::map<QString, int> groups;
    int largest = 1;
    for (const auto &r : rows_)
        if (r.enabled->isChecked()) {
            ++count;
            largest = std::max(largest, ++groups[r.group->text().trimmed().toCaseFolded()]);
        }
    if (!count)
        return;
    double value = std::min(invested_->value() / count, weight_limit_->value());
    if (group_limit_->value() > 0)
        value = std::min(value, group_limit_->value() / largest);
    value = std::floor(value * 100) / 100;
    for (auto &r : rows_)
        r.weight->setValue(r.enabled->isChecked() ? value : 0);
}
void PortfolioPanel::demo() {
    PortfolioInput input;
    input.synthetic = input.metadata_known = true;
    input.source = QStringLiteral("ДЕМО • полностью синтетические свечи трёх вымышленных инструментов");
    for (int j = 0; j < 3; ++j) {
        quant::PortfolioAsset a;
        a.symbol = "DEMO_" + std::string(1, static_cast<char>('A' + j));
        a.tick_size = .01;
        for (int i = 0; i < 360; ++i) {
            const auto t = 1700000000000LL + i * 86400000LL;
            const double p = 100 + (.04 + .01 * j) * i + 5 * std::sin(i * (.09 + .02 * j) + j);
            a.bars.push_back({t, t + 86400000, p, p + 1, p - 1, p, 1000000, 10800});
        }
        input.assets.push_back(std::move(a));
    }
    set_input(std::move(input));
}
PortfolioProject PortfolioPanel::project() const {
    PortfolioProject p;
    p.input = input_;
    p.fast = fast_->value();
    p.slow = slow_->value();
    auto &c = p.config;
    c.initial_cash = capital_->value();
    c.rebalance_every = period_->value();
    c.max_invested = invested_->value() / 100;
    c.max_weight = weight_limit_->value() / 100;
    c.group_limit = group_limit_->value() / 100;
    c.commission_rate = commission_->value() / 100;
    c.fixed_fee = fixed_->value();
    c.spread_bps = spread_->value();
    c.slippage_bps = slippage_->value();
    c.max_drawdown = drawdown_->value() / 100;
    c.daily_loss = daily_->value() / 100;
    c.manual_stop_after = manual_stop_->value();
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto &r = rows_[i];
        auto &a = p.input.assets[i];
        a.weight = r.weight->value() / 100;
        a.lot = r.lot->value();
        a.tick_size = r.tick->value();
        a.group = r.group->text().toStdString();
        p.enabled.push_back(r.enabled->isChecked());
        p.modes.push_back(r.rules->currentIndex());
    }
    return p;
}
void PortfolioPanel::apply_project(PortfolioProject p) {
    const auto original_source = p.input.source;
    set_input(std::move(p.input));
    // Keep the original bounded source in the next export, without accumulating
    // file prefixes.
    source_->setText(QStringLiteral("Локальный файл • происхождение не проверено. ") +
                     (input_.synthetic ? QStringLiteral("ДЕМО заявлено автором файла. ") : QString()) +
                     original_source);
    const auto &c = p.config;
    capital_->setValue(c.initial_cash);
    period_->setValue(c.rebalance_every);
    invested_->setValue(c.max_invested * 100);
    weight_limit_->setValue(c.max_weight * 100);
    group_limit_->setValue(c.group_limit * 100);
    commission_->setValue(c.commission_rate * 100);
    fixed_->setValue(c.fixed_fee);
    spread_->setValue(c.spread_bps);
    slippage_->setValue(c.slippage_bps);
    drawdown_->setValue(c.max_drawdown * 100);
    daily_->setValue(c.daily_loss * 100);
    manual_stop_->setValue(c.manual_stop_after);
    fast_->setValue(p.fast);
    slow_->setValue(p.slow);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto &a = input_.assets[i];
        auto &r = rows_[i];
        r.enabled->setChecked(p.enabled[i]);
        r.weight->setValue(a.weight * 100);
        r.lot->setValue(static_cast<int>(a.lot));
        r.tick->setValue(a.tick_size);
        r.group->setText(QString::fromStdString(a.group));
        r.rules->setCurrentIndex(p.modes[i]);
        if (!input_.synthetic)
            input_.assets[i].tick_size = 0; // force fresh explicit metadata confirmation
    }
    manual_metadata_->setChecked(false);
    invalidate();
    status_->setText(QStringLiteral("Проект открыт без сети. Результаты не загружались: нажмите «Рассчитать "
                                    "портфель». Для рыночных свечей заново проверьте и подтвердите "
                                    "лоты/шаги. "
                                    "Сохранённые файлы не являются непрерывным виртуальным счётом."));
}
void PortfolioPanel::project_file(bool save, QString path) {
    if (working() || (save && input_.assets.empty()))
        return;
    if (path.isEmpty()) {
        QFileDialog dialog(this,
                           save ? QStringLiteral("Сохранить цены, капитал и настройки")
                                : QStringLiteral("Открыть локальный портфель"),
                           {}, QStringLiteral("Портфель Quant (*.qpp.json)"));
        dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
        dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
        dialog.setDefaultSuffix(QStringLiteral("qpp.json"));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        path = dialog.selectedFiles().front();
    }
    auto p = save ? project() : PortfolioProject{};
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto flag = cancelled_;
    busy(true);
    status_->setText(save ? QStringLiteral("Сохраняем портфель в выбранный файл…")
                          : QStringLiteral("Проверяем локальный файл (не более 16 МиБ)…"));
    file_worker_->setFuture(QtConcurrent::run([save, path, p = std::move(p), flag]() mutable {
        PortfolioFileOutcome out;
        try {
            if (save) {
                write_portfolio_project(path, p, *flag);
                out.saved = true;
            } else {
                out.project = read_portfolio_project(path, *flag);
            }
        } catch (const std::exception &) {
            out.error = QStringLiteral("Файл не сохранён или не открыт. Проверьте доступ к папке, формат "
                                       "и версию .qpp.json, контрольную сумму, даты/МСК и лимиты: 16 МиБ, "
                                       "6 инструментов по 5000 свечей. Текущий портфель сохранён в памяти.");
        }
        return out;
    }));
}
void PortfolioPanel::calculate() {
    if (working() || input_.assets.empty())
        return;
    const auto c = project().config;
    std::vector<quant::PortfolioAsset> selected;
    double sum = 0;
    std::map<std::string, double> groups;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto &r = rows_[i];
        if (!r.enabled->isChecked())
            continue;
        auto a = input_.assets[i];
        a.strategy = quant::Strategy{}; // only visible portfolio templates define the model
        a.weight = r.weight->value() / 100;
        a.lot = r.lot->value();
        a.tick_size = r.tick->value();
        a.group = r.group->text().trimmed().toCaseFolded().toStdString();
        const int mode = r.rules->currentIndex();
        a.use_rules = mode != 0;
        if (mode == 1) {
            a.strategy.fast = fast_->value();
            a.strategy.slow = slow_->value();
        }
        if (mode == 2) {
            a.strategy.fast = 5;
            a.strategy.slow = 20;
        }
        if (mode == 3) {
            a.strategy.fast = 20;
            a.strategy.slow = 60;
        }
        if (mode == 4) {
            a.strategy.entry = {
                {quant::Rule{quant::Feature::Rsi, quant::Feature::Constant, quant::Compare::Below, 30, true}},
                true};
            a.strategy.exit = {
                {quant::Rule{quant::Feature::Rsi, quant::Feature::Constant, quant::Compare::Above, 70, true}},
                true};
        }
        if (a.strategy.fast >= a.strategy.slow) {
            status_->setText(QStringLiteral("Быстрая SMA должна быть меньше медленной."));
            return;
        }
        sum += a.weight;
        groups[a.group] += a.weight;
        if (a.weight > c.max_weight + 1e-12 || a.bars.size() < 2 || (c.group_limit > 0 && a.group.empty())) {
            status_->setText(QStringLiteral("Проверьте вес акции, минимум две свечи и названия "
                                            "групп при включённом лимите."));
            return;
        }
        if (!input_.synthetic &&
            ((input_.assets[i].tick_size <= 0 && !manual_metadata_->isChecked()) || a.tick_size <= 0)) {
            status_->setText(
                QStringLiteral("Для рыночных данных нужны подтверждённые лоты и положительные шаги "
                               "цены. При "
                               "отсутствии каталога заполните их и включите ручную проверку."));
            return;
        }
        selected.push_back(std::move(a));
    }
    if (selected.empty() || sum <= 0 || sum > c.max_invested + 1e-12) {
        status_->setText(
            QStringLiteral("Выберите акции; сумма долей должна быть положительной и не превышать "
                           "вложенный "
                           "лимит, fast < slow. Есть кнопка равного распределения."));
        return;
    }
    if (c.group_limit > 0)
        for (const auto &g : groups)
            if (g.second > c.group_limit + 1e-12) {
                status_->setText(QStringLiteral("Сумма долей одной из ручных групп превышает лимит."));
                return;
            }
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto flag = cancelled_;
    const auto source = source_->text();
    busy(true);
    status_->setText(QStringLiteral("Согласуем общие интервалы и считаем единый счёт в фоне…"));
    worker_->setFuture(QtConcurrent::run([selected = std::move(selected), c, flag, source]() mutable {
        PortfolioOutcome out;
        out.source = source;
        try {
            out.result = quant::portfolio_backtest(selected, c, flag.get());
            out.baseline = quant::portfolio_baseline(std::move(selected), c, flag.get());
        } catch (const std::exception &) {
            out.error = QStringLiteral("Расчёт не выполнен. Проверьте лоты, шаги, пределы издержек и "
                                       "наличие хотя бы "
                                       "двух точных общих интервалов. Отсутствующие свечи не заполняются.");
        }
        return out;
    }));
}
void PortfolioPanel::present(const PortfolioOutcome &out) {
    const auto &r = out.result;
    double contribution = 0;
    std::size_t open = 0;
    QStringList removed, cold;
    positions_->setRowCount(0);
    for (std::size_t i = 0; i < r.positions.size(); ++i) {
        const auto &p = r.positions[i];
        contribution += p.pnl;
        if (!p.rules_ready)
            cold << QString::fromStdString(p.symbol);
        if (p.units)
            ++open;
        row(positions_, {QString::fromStdString(p.symbol), QString::number(p.units), n(p.value),
                         n(r.equity.back() > 0 ? 100 * p.value / r.equity.back() : quant::unavailable()),
                         n(p.pnl), n(p.fees), QString::number(r.excluded[i])});
        if (r.excluded[i])
            removed << QString::fromStdString(p.symbol) + ": " + QString::number(r.excluded[i]);
    }
    const auto from = QDateTime::fromMSecsSinceEpoch(r.start_time).toUTC().toString(Qt::ISODate),
               till = QDateTime::fromMSecsSinceEpoch(r.times.back()).toUTC().toString(Qt::ISODate);
    summary_->setText(QStringLiteral("%1\nОбщих свечей: %2 • UTC %3 — %4\nКапитал: %5 ₽ • "
                                     "деньги: %6 ₽ • результат по "
                                     "ценам без дивидендов: %7% • "
                                     "просадка: %8%\nКомиссии: %9 ₽ • модельный "
                                     "спред/проскальзывание/округление: %10 ₽ • "
                                     "исполнений: %11 • оборот/начальный капитал: %12\nСумма "
                                     "вкладов: %13 ₽ • база равных "
                                     "долей: %14%\nОстановка: %15 • открытых позиций на конце: "
                                     "%16\nИсключено вне общих "
                                     "интервалов: %17 • не сохранено событий сверх лимита: "
                                     "%18\nНеполный прогрев или "
                                     "выключены условия: %19\nМодель: индикаторы по общим "
                                     "наблюдениям; объём/очередь "
                                     "торгов, налоги и дивиденды не моделируются.")
                          .arg(out.source)
                          .arg(r.common_bars)
                          .arg(from, till, n(r.equity.back()), n(r.cash), n(r.total_return * 100),
                               n(r.max_drawdown * 100), n(r.fees), n(r.execution_cost))
                          .arg(r.fills)
                          .arg(n(r.turnover), n(contribution), n(out.baseline.total_return * 100),
                               r.halted ? reason(r.halt_reason) : QStringLiteral("нет"))
                          .arg(open)
                          .arg(removed.isEmpty() ? QStringLiteral("нет") : removed.join("; "))
                          .arg(r.dropped_events)
                          .arg(cold.isEmpty() ? QStringLiteral("нет") : cold.join("; ")));
    equity_->set_lines({line(r.equity, QStringLiteral("Портфель"), QColor("#39d2b0")),
                        line(out.baseline.equity, QStringLiteral("База равных долей"), QColor("#efc571")),
                        line(std::vector<double>(r.equity.size(), r.equity.front()),
                             QStringLiteral("Деньги без процентов"), QColor("#829eff"))},
                       QStringLiteral("Общий виртуальный капитал, ₽"),
                       QStringLiteral("Ось X — номер общей свечи; точка 0 — начальный капитал. Финальные "
                                      "позиции оценены по закрытию."));
    std::vector<double> dd = r.drawdowns;
    for (auto &v : dd)
        v *= 100;
    drawdowns_->set_lines({line(dd, QStringLiteral("Просадка, %"), QColor("#ef8996"))},
                          QStringLiteral("Просадка от общего пика"), {});
    journal_->setRowCount(0);
    const auto start = r.journal.size() > 500 ? r.journal.size() - 500 : 0;
    for (std::size_t i = start; i < r.journal.size(); ++i) {
        const auto &e = r.journal[i];
        row(journal_, {QDateTime::fromMSecsSinceEpoch(e.time).toUTC().toString(Qt::ISODate),
                       QString::fromStdString(e.symbol), QString::number(e.quantity),
                       e.price > 0 ? QString::number(e.price, 'g', 12) : QStringLiteral("—"), n(e.fee),
                       n(e.cash), reason(e.reason)});
    }
    status_->setText(QStringLiteral("Расчёт завершён. Общие деньги не дублируются между позициями. Издержки "
                                    "уже включены в капитал и вклады; не вычитайте их повторно."));
    QTimer::singleShot(0, this, [this] {
        for (auto *p = parentWidget(); p; p = p->parentWidget())
            if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
                scroll->ensureWidgetVisible(summary_, 0, 20);
                break;
            }
    });
}
bool PortfolioPanel::self_test() {
    demo();
    if (input_.assets.size() != 3 || rows_.size() != 3)
        return false;
    auto wait = [this] {
        QEventLoop loop;
        const auto c =
            connect(worker_, &QFutureWatcher<PortfolioOutcome>::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(c);
        return !worker_->isRunning();
    };
    run_->click();
    if (!wait() || positions_->rowCount() != 3 || !summary_->text().contains(QStringLiteral("ДЕМО")))
        return false;
    capital_->setValue(120000);
    if (!summary_->text().isEmpty())
        return false;
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const auto path = dir.filePath(QStringLiteral("portfolio.qpp.json"));
    auto wait_file = [this] {
        QEventLoop loop;
        const auto c =
            connect(file_worker_, &QFutureWatcher<PortfolioFileOutcome>::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(c);
        return !file_worker_->isRunning();
    };
    rows_[1].enabled->setChecked(false);
    rows_[0].group->setText(QStringLiteral("Учебная группа"));
    rows_[0].rules->setCurrentIndex(4);
    fast_->setValue(7);
    slow_->setValue(21);
    std::atomic<bool> check_cancel{false};
    const auto saved = encode_portfolio_project(project(), check_cancel);
    auto values = [this] {
        QStringList result;
        for (int row = 0; row < positions_->rowCount(); ++row)
            for (int col = 0; col < positions_->columnCount(); ++col)
                result.push_back(positions_->item(row, col)->text());
        return result;
    };
    run_->click();
    if (!wait() || positions_->rowCount() != 2)
        return false;
    const auto reference = values();
    project_file(true, path);
    if (run_->isEnabled() || open_->isEnabled() || !wait_file() ||
        !status_->text().contains(QStringLiteral("сохранён")))
        return false;
    clear_private_state();
    project_file(false, path);
    if (!wait_file() || encode_portfolio_project(project(), check_cancel) != saved ||
        !summary_->text().isEmpty() || manual_metadata_->isChecked())
        return false;
    run_->click();
    if (!wait() || values() != reference)
        return false;
    project_file(false, dir.filePath(QStringLiteral("absent")));
    if (!wait_file() || encode_portfolio_project(project(), check_cancel) != saved)
        return false;
    project_file(false, path);
    clear_private_state();
    if (!wait_file() || !input_.assets.empty() || assets_->rowCount() != 0)
        return false;
    project_file(false, path);
    if (!wait_file())
        return false;
    // A local market-data file must not inherit catalog trust or metadata
    // consent.
    auto market = project();
    market.input.synthetic = false;
    apply_project(market);
    run_->click();
    if (worker_->isRunning() || manual_metadata_->isChecked() || !summary_->text().isEmpty())
        return false;
    demo();
    rows_[0].rules->setCurrentIndex(1);
    run_->click();
    cancel_->click();
    if (!wait() || !summary_->text().isEmpty())
        return false;
    clear_private_state();
    if (!input_.assets.empty() || assets_->rowCount() != 0 || positions_->rowCount() != 0)
        return false;
    demo();
    run_->click();
    return wait() && positions_->rowCount() == 3;
}
} // namespace live
