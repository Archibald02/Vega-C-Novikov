#include "main_window.hpp"
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
namespace live {
namespace {
QString fmt(double v, int decimals = 2) { return std::isfinite(v) ? QString::number(v,'f',decimals) : QStringLiteral("—"); }
QLabel* label(const QString& text, const QString& style = {}) { auto* w = new QLabel(text); if (!style.isEmpty()) w->setStyleSheet(style); return w; }
}
MainWindow::MainWindow(bool test_mode) : test_mode_(test_mode) {
    setWindowTitle(QStringLiteral("Quant Price Live • Российские акции")); resize(1280,920);
    setMinimumSize(1060,860);
    setStyleSheet(R"(
        QMainWindow,QWidget { background:#0b1420; color:#e7f0fa; font-family:'Segoe UI',sans-serif; font-size:13px; }
        QLabel { background:transparent; }
        QLineEdit,QComboBox,QSpinBox { background:#162538; border:1px solid #31465d; border-radius:6px; padding:8px; min-height:20px; }
        QPushButton { background:#1c364f; border:1px solid #365371; border-radius:6px; padding:10px 18px; font-weight:600; }
        QPushButton#connectButton { background:#127d6d; border-color:#1ba38d; }
        QPushButton:disabled { color:#63788e; background:#152233; border-color:#23374a; }
        QPushButton:hover { border-color:#47d5ba; }
        QTableWidget { background:#101c2b; border:1px solid #25394e; gridline-color:#23374a; selection-background-color:#21433f; }
        QHeaderView::section { background:#1a2b3e; color:#a9bfd3; border:none; padding:9px; }
        QTableWidget::item { padding:8px; }
        QSplitter::handle { background:#0b1420; height:12px; }
    )");
    auto* root = new QWidget; setCentralWidget(root); auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(24,20,24,16); layout->setSpacing(14);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label("QUANT / PRICE LIVE","font-size:24px; font-weight:700; letter-spacing:2px;"));
    heading->addStretch(); heading->addWidget(label(QStringLiteral("MOEX • TQBR • C++17 / Qt"),"color:#85a1b9;")); layout->addLayout(heading);
    layout->addWidget(label(QStringLiteral("Котировки российских акций · поток данных · скользящие показатели"),"color:#91a7be;"));
    auto* settings = new QHBoxLayout;
    auto* form_left = new QFormLayout; auto* form_right = new QFormLayout;
    provider_ = new QComboBox; provider_->addItems({QStringLiteral("MOEX ISS — с задержкой, без ключа"),QStringLiteral("АЛОР — WebSocket, боевой аккаунт"),QStringLiteral("ДЕМО — синтетические цены")});
    symbols_ = new QLineEdit("SBER, GAZP, LKOH, YDEX");
    symbols_->setToolTip(QStringLiteral("До 20 тикеров режима TQBR через запятую. Доступность проверяет источник."));
    token_ = new QLineEdit; token_->setEchoMode(QLineEdit::Password); token_->setPlaceholderText(QStringLiteral("Refresh token АЛОР; только в памяти"));
    token_->setText(qEnvironmentVariable("ALOR_REFRESH_TOKEN"));
    window_ = new QSpinBox; window_->setRange(3,2000); window_->setValue(20); window_->setSuffix(QStringLiteral(" цен"));
    frequency_ = new QSpinBox; frequency_->setRange(25,5000); frequency_->setSingleStep(25); frequency_->setValue(250); frequency_->setSuffix(QStringLiteral(" мс"));
    frequency_->setToolTip(QStringLiteral("Интервал отправки АЛОР. Это не гарантия задержки. MOEX опрашивается раз в 5 секунд."));
    form_left->addRow(QStringLiteral("Источник"),provider_); form_left->addRow(QStringLiteral("Тикеры"),symbols_);
    form_right->addRow(QStringLiteral("Токен АЛОР"),token_);
    auto* nums = new QHBoxLayout; nums->addWidget(label("SMA / σ:")); nums->addWidget(window_); nums->addWidget(label(QStringLiteral("Поток:"))); nums->addWidget(frequency_);
    form_right->addRow(nums); settings->addLayout(form_left,1); settings->addSpacing(24); settings->addLayout(form_right,1); layout->addLayout(settings);
    auto* controls = new QHBoxLayout;
    start_ = new QPushButton(QStringLiteral("Подключить")); start_->setObjectName("connectButton");
    stop_ = new QPushButton(QStringLiteral("Отключить")); stop_->setEnabled(false);
    connection_ = label(QStringLiteral("Отключено"),"color:#a6bdd1;"); connection_->setWordWrap(true);
    controls->addWidget(start_); controls->addWidget(stop_); controls->addSpacing(16); controls->addWidget(connection_,1); layout->addLayout(controls);
    banner_ = label(""); banner_->setWordWrap(true); banner_->setStyleSheet("background:#30291a; color:#e9c882; padding:10px; border-radius:6px;"); layout->addWidget(banner_);
    auto update_mode = [this] {
        const int mode = provider_->currentIndex(); token_->setEnabled(mode==1); frequency_->setEnabled(mode==1);
        if (mode==0) banner_->setText(QStringLiteral("ДАННЫЕ С ЗАДЕРЖКОЙ · MOEX ISS: обновление раз в 5 с не устраняет задержку источника. Время в таблице — время снимка ISS, не последней сделки."));
        else if (mode==1) banner_->setText(QStringLiteral("ПОТОК АЛОР · Нужен действующий боевой аккаунт и Refresh token. Цены поступают во время торгов; сетевая задержка не гарантируется. График строится с момента подключения."));
        else banner_->setText(QStringLiteral("ДЕМОНСТРАЦИЯ · Все цены генерируются программой и не являются котировками рынка."));
    };
    connect(provider_,&QComboBox::currentIndexChanged,this,update_mode); update_mode();
    auto* metrics = new QHBoxLayout;
    auto add_metric = [metrics](const QString& title) {
        auto* box = new QFrame; box->setStyleSheet("QFrame { background:#132235; border-radius:8px; }");
        auto* col = new QVBoxLayout(box); col->setContentsMargins(16,12,16,12);
        col->addWidget(label(title,"color:#91a7be; font-size:12px;"));
        auto* value = label(QStringLiteral("—"),"font-size:23px; font-weight:600;"); col->addWidget(value); metrics->addWidget(box,1); return value;
    };
    metric_price_=add_metric(QStringLiteral("ПОСЛЕДНЯЯ ЦЕНА, ₽")); metric_sma_=add_metric(QStringLiteral("SMA ПО ОКНУ, ₽"));
    metric_vol_=add_metric(QStringLiteral("σ ДОХОДНОСТЕЙ ОБНОВЛЕНИЙ, %")); metric_count_=add_metric(QStringLiteral("ЦЕН В ОКНЕ")); layout->addLayout(metrics);
    auto* split = new QSplitter(Qt::Vertical);
    table_=new QTableWidget(0,8); table_->setHorizontalHeaderLabels({QStringLiteral("Тикер"),QStringLiteral("Цена, ₽"),"Bid, ₽","Ask, ₽",QStringLiteral("Изм. дня, %"),QStringLiteral("SMA, ₽"),QStringLiteral("Время данных*"),QStringLiteral("Ответ, с")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table_->verticalHeader()->hide(); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setMinimumHeight(180);
    chart_=new ChartWidget; split->addWidget(table_); split->addWidget(chart_); split->setStretchFactor(0,1); split->setStretchFactor(1,2); layout->addWidget(split,1);
    layout->addWidget(label(QStringLiteral("SMA и σ считаются по последним N принятым ценам, а не по дням. σ не годовая. Время на графике — локальное. История ограничена 2 000 точек на тикер."),"color:#7f99b1; font-size:11px;"));
    connect(start_,&QPushButton::clicked,this,&MainWindow::connect_feed);
    connect(stop_,&QPushButton::clicked,this,[this] { feed_.stop(); set_controls(false); token_->clear(); });
    connect(&feed_,&MarketFeed::quote_received,this,&MainWindow::receive);
    connect(&feed_,&MarketFeed::status_changed,this,[this](const QString& status) { connection_->setText(status); });
    connect(table_,&QTableWidget::itemSelectionChanged,this,&MainWindow::render);
    render_timer_.setInterval(250); connect(&render_timer_,&QTimer::timeout,this,&MainWindow::render); render_timer_.start();
    if (!test_mode_) {
        QSettings s; symbols_->setText(s.value("symbols",symbols_->text()).toString());
        window_->setValue(s.value("window",20).toInt()); frequency_->setValue(s.value("frequency",250).toInt());
        provider_->setCurrentIndex(std::clamp(s.value("provider",0).toInt(),0,2));
    }
}
void MainWindow::save_settings() {
    if (test_mode_) return;
    QSettings s; s.setValue("symbols",symbols_->text()); s.setValue("window",window_->value());
    s.setValue("frequency",frequency_->value()); s.setValue("provider",provider_->currentIndex());
    // Intentionally never persist credentials or market data.
}
void MainWindow::set_controls(bool connected) {
    provider_->setEnabled(!connected); symbols_->setEnabled(!connected); window_->setEnabled(!connected);
    token_->setEnabled(!connected && provider_->currentIndex()==1); frequency_->setEnabled(!connected && provider_->currentIndex()==1);
    start_->setEnabled(!connected); stop_->setEnabled(connected);
}
void MainWindow::connect_feed() {
    try { tickers_=parse_symbols(symbols_->text()); }
    catch (...) { QMessageBox::warning(this,QStringLiteral("Тикеры"),QStringLiteral("Введите от 1 до 20 корректных тикеров через запятую: SBER, GAZP, LKOH")); return; }
    if (provider_->currentIndex()==1 && token_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this,QStringLiteral("Токен"),QStringLiteral("Введите боевой Refresh token АЛОР. Он не сохраняется на диск.")); return;
    }
    save_settings(); rows_.clear(); table_->setRowCount(tickers_.size());
    for (int i=0;i<tickers_.size();++i) {
        rows_.emplace(tickers_[i],Row(static_cast<std::size_t>(window_->value())));
        for (int j=0;j<table_->columnCount();++j) table_->setItem(i,j,new QTableWidgetItem(j==0 ? tickers_[i] : QStringLiteral("—")));
    }
    table_->selectRow(0); set_controls(true);
    MarketFeed::Config config; config.symbols=tickers_; config.refresh_token=token_->text().trimmed(); config.interval_ms=frequency_->value();
    config.mode=provider_->currentIndex()==0 ? MarketFeed::Mode::Moex : provider_->currentIndex()==1 ? MarketFeed::Mode::Alor : MarketFeed::Mode::Demo;
    feed_.start(config);
}
void MainWindow::receive(const Quote& q) {
    auto it=rows_.find(q.symbol); if (it==rows_.end()) return; auto& row=it->second;
    const bool first=row.stats.size()==0;
    if (!first && q.market_time_ms < row.quote.market_time_ms) return;
    row.received=QDateTime::currentMSecsSinceEpoch();
    const bool distinct=first || q.market_time_ms!=row.quote.market_time_ms || q.last!=row.quote.last;
    row.quote=q;
    if (distinct) {
        try { row.stats.push(q.last); } catch (...) { return; }
        row.points.emplace_back(static_cast<double>(q.market_time_ms),q.last);
        if (row.points.size()>2000) row.points.pop_front();
    }
}
void MainWindow::render() {
    for (int i=0;i<tickers_.size();++i) {
        auto it=rows_.find(tickers_[i]); if (it==rows_.end() || it->second.stats.size()==0) continue;
        const auto& row=it->second; const auto& q=row.quote;
        const auto age=std::max<qint64>(0,QDateTime::currentMSecsSinceEpoch()-row.received)/1000;
        const QStringList values{q.symbol,fmt(q.last),fmt(q.bid),fmt(q.ask),fmt(q.change_percent),fmt(row.stats.mean_price()),QDateTime::fromMSecsSinceEpoch(q.market_time_ms).toLocalTime().toString("dd.MM HH:mm:ss"),QString::number(age)};
        for (int j=0;j<values.size();++j) table_->item(i,j)->setText(values[j]);
        table_->item(i,4)->setForeground(std::isfinite(q.change_percent) && q.change_percent<0 ? QColor("#f28e97") : QColor("#4dd6b7"));
        table_->item(i,7)->setForeground(age>30 ? QColor("#e6bb6b") : QColor("#a9bfd3"));
    }
    const int selected=table_->currentRow();
    if (selected<0 || selected>=tickers_.size()) return;
    const auto it=rows_.find(tickers_[selected]); if (it==rows_.end()) return; const auto& row=it->second;
    metric_price_->setText(row.stats.size() ? fmt(row.quote.last) : QStringLiteral("—"));
    metric_sma_->setText(row.stats.size() ? fmt(row.stats.mean_price()) : QStringLiteral("—"));
    metric_count_->setText(QString::number(row.stats.size())+" / "+QString::number(window_->value()));
    metric_vol_->setText(row.stats.size()>=3 ? fmt(row.stats.return_stddev()*100,4) : QStringLiteral("—"));
    const QString detail=provider_->currentIndex()==0 ? QStringLiteral("Снимки ISS с задержкой · с момента подключения") : provider_->currentIndex()==1 ? QStringLiteral("Последняя цена сделки · с момента подключения") : QStringLiteral("Синтетический демонстрационный ряд");
    chart_->set_series(row.points,tickers_[selected]+QStringLiteral(" / RUB"),detail);
}
void MainWindow::start_demo_test() { provider_->setCurrentIndex(2); symbols_->setText("SBER, GAZP, LKOH, YDEX"); connect_feed(); }
void MainWindow::start_moex_test() { provider_->setCurrentIndex(0); symbols_->setText("SBER, GAZP"); connect_feed(); }
bool MainWindow::has_demo_data() const {
    if (rows_.size()!=4 || table_->rowCount()!=4) return false;
    for (const auto& row : rows_) if (row.second.stats.size()<3 || !std::isfinite(row.second.stats.mean_price())) return false;
    for (int i=0;i<4;++i) if (!table_->item(i,1) || table_->item(i,1)->text()==QStringLiteral("—")) return false;
    return true;
}
bool MainWindow::has_any_data() const { for (const auto& row : rows_) if (row.second.stats.size()>0) return true; return false; }
}
