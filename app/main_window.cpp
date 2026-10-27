#include "main_window.hpp"
#include "catalog_dialog.hpp"
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QScrollArea>
#include <QTabBar>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace live {
namespace {
QString fmt(double v,int decimals=2){return std::isfinite(v)?QString::number(v,'f',decimals):QStringLiteral("—");}
QLabel* label(const QString& text,const QString& style={}){auto*w=new QLabel(text);w->setTextFormat(Qt::PlainText);if(!style.isEmpty())w->setStyleSheet(style);return w;}
void text_if_changed(QLabel* item,const QString& text){if(item->text()!=text)item->setText(text);}
QDoubleSpinBox* number(double value,double low=0.01,double high=1e12,int decimals=2){auto*w=new QDoubleSpinBox;w->setRange(low,high);w->setDecimals(decimals);w->setValue(value);w->setGroupSeparatorShown(true);return w;}
}
MainWindow::MainWindow(bool test_mode):test_mode_(test_mode){
    setWindowTitle(QStringLiteral("Quant Price Live 6 • Российские акции"));resize(1440,1000);setMinimumSize(1000,720);
    setStyleSheet(R"(
      QMainWindow,QWidget{background:#0b1420;color:#e7f0fa;font-family:'Segoe UI',sans-serif;font-size:12px;}
      QLabel{background:transparent;} QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox{background:#162538;border:1px solid #31465d;border-radius:5px;padding:6px;min-height:20px;}
      QPushButton{background:#1c364f;border:1px solid #365371;border-radius:5px;padding:8px 14px;font-weight:600;}
      QPushButton#connectButton{background:#127d6d;border-color:#1ba38d;} QPushButton:disabled{color:#63788e;background:#152233;border-color:#23374a;}
      QPushButton:hover{border-color:#47d5ba;} QTableWidget,QListWidget{background:#101c2b;border:1px solid #25394e;gridline-color:#23374a;selection-background-color:#21433f;}
      QHeaderView::section{background:#1a2b3e;color:#a9bfd3;border:none;padding:8px;} QTableWidget::item{padding:6px;}
      QTabBar::tab{background:#152438;padding:9px 12px;color:#98b0c6;} QTabBar::tab:selected{color:#e7f0fa;background:#21433f;}
      QTabWidget::pane{border:1px solid #25394e;} QSplitter::handle{background:#0b1420;height:8px;}
    )");
    auto* root=new QWidget;auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(root);setCentralWidget(scroll);auto* layout=new QVBoxLayout(root);layout->setContentsMargins(20,16,20,12);layout->setSpacing(10);
    auto* heading=new QHBoxLayout;heading->addWidget(label("QUANT / PRICE LIVE 6","font-size:23px;font-weight:700;letter-spacing:2px;"));heading->addStretch();
    heading->addWidget(label(QStringLiteral("MOEX • TQBR • C++17 / Qt"),"color:#85a1b9;"));layout->addLayout(heading);
    auto* settings=new QHBoxLayout;auto* left=new QFormLayout;auto* right=new QFormLayout;
    provider_=new QComboBox;provider_->addItems({QStringLiteral("MOEX ISS — с задержкой, без ключа"),QStringLiteral("АЛОР — WebSocket, боевой аккаунт"),QStringLiteral("ДЕМО — синтетические цены")});
    symbols_=new QLineEdit("SBER, GAZP, LKOH, YDEX");symbols_->setMaxLength(512);symbols_->setToolTip(QStringLiteral("До 20 инструментов TQBR. Выбирайте из каталога или вводите тикеры."));
    token_=new QLineEdit;token_->setEchoMode(QLineEdit::Password);token_->setPlaceholderText(QStringLiteral("Refresh token АЛОР; только в памяти"));token_->setMaxLength(8192);token_->setObjectName("optionalAlorToken");
    window_=new QSpinBox;window_->setRange(3,500);window_->setValue(20);window_->setSuffix(QStringLiteral(" цен"));
    frequency_=new QSpinBox;frequency_->setRange(100,5000);frequency_->setSingleStep(50);frequency_->setValue(250);frequency_->setSuffix(QStringLiteral(" мс"));
    frequency_->setToolTip(QStringLiteral("АЛОР: интервал выдачи от 100 мс; не гарантия задержки до экрана."));
    left->addRow(QStringLiteral("Источник"),provider_);left->addRow(QStringLiteral("Тикеры"),symbols_);token_label_=label(QStringLiteral("Токен АЛОР (необязательно)"));right->addRow(token_label_,token_);
    auto* nums=new QHBoxLayout;nums->addWidget(label(QStringLiteral("SMA / EMA / σ:")));nums->addWidget(window_);nums->addWidget(label(QStringLiteral("Поток:")));nums->addWidget(frequency_);right->addRow(nums);
    settings->addLayout(left,1);settings->addSpacing(20);settings->addLayout(right,1);layout->addLayout(settings);
    auto* buttons=new QHBoxLayout;start_=new QPushButton(QStringLiteral("Подключить"));start_->setObjectName("connectButton");stop_=new QPushButton(QStringLiteral("Отключить"));stop_->setEnabled(false);
    catalog_button_=new QPushButton(QStringLiteral("Каталог акций"));auto* api=new QPushButton(QStringLiteral("Получить API / помощь"));
    connection_=label(QStringLiteral("Отключено"),"color:#a6bdd1;");connection_->setWordWrap(true);
    for(auto*b:{start_,stop_,catalog_button_,api})buttons->addWidget(b);buttons->addWidget(connection_,1);layout->addLayout(buttons);
    banner_=label("");banner_->setWordWrap(true);banner_->setStyleSheet("background:#30291a;color:#e9c882;padding:9px;border-radius:5px;");layout->addWidget(banner_);
    auto mode_changed=[this]{const int mode=provider_->currentIndex();token_->setEnabled(mode==1);token_->setVisible(mode==1);token_label_->setVisible(mode==1);if(mode!=1)token_->clear();frequency_->setEnabled(mode==1);
        if(mode==0)banner_->setText(QStringLiteral("MOEX ISS • ДАННЫЕ С ЗАДЕРЖКОЙ. Время в таблице — время снимка ISS, а не последней сделки. Опрос не устраняет задержку источника."));
        else if(mode==1)banner_->setText(QStringLiteral("АЛОР • Нужен боевой аккаунт и Refresh token. Последняя цена меняется во время торгов. История начинается при подключении; сетевая задержка не гарантируется."));
        else banner_->setText(QStringLiteral("ДЕМО • Таблица и потоковые графики синтетические. Свечи MOEX загружаются отдельно из реального API с задержкой."));};
    connect(provider_,&QComboBox::currentIndexChanged,this,mode_changed);mode_changed();
    auto* metrics=new QHBoxLayout;
    auto metric=[metrics](const QString& title){auto*box=new QFrame;box->setStyleSheet("QFrame{background:#132235;border-radius:7px;}");auto*col=new QVBoxLayout(box);col->setContentsMargins(14,9,14,9);col->addWidget(label(title,"color:#91a7be;font-size:11px;"));auto*v=label(QStringLiteral("—"),"font-size:22px;font-weight:600;");col->addWidget(v);metrics->addWidget(box,1);return v;};
    metric_price_=metric(QStringLiteral("ПОСЛЕДНЯЯ ЦЕНА, ₽"));metric_sma_=metric(QStringLiteral("SMA ПО ОКНУ, ₽"));metric_vol_=metric(QStringLiteral("σ ДОХОДНОСТЕЙ ОБНОВЛЕНИЙ, %"));metric_count_=metric(QStringLiteral("ЦЕН В ОКНЕ"));layout->addLayout(metrics);
    indicator_text_=label(QStringLiteral("EMA • RSI 14 • MACD 12/26/9 • Bollinger 20/2: ожидаем наблюдения"),"color:#a9bfd3;");indicator_text_->setWordWrap(true);layout->addWidget(indicator_text_);
    auto* split=new QSplitter(Qt::Vertical);table_=new QTableWidget(0,9);
    table_->setHorizontalHeaderLabels({QStringLiteral("Тикер"),QStringLiteral("Цена, ₽"),"Bid, ₽","Ask, ₽",QStringLiteral("Спред, б.п."),QStringLiteral("Изм. дня, %"),QStringLiteral("SMA, ₽"),QStringLiteral("Время данных*"),QStringLiteral("Ответ, с")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table_->verticalHeader()->hide();table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setSelectionMode(QAbstractItemView::SingleSelection);table_->setEditTriggers(QAbstractItemView::NoEditTriggers);table_->setMinimumHeight(165);
    tabs_=new QTabWidget;tabs_->tabBar()->setUsesScrollButtons(true);tabs_->tabBar()->setExpanding(false);chart_=new ChartWidget;auto* chart_page=new QWidget;auto* chart_layout=new QVBoxLayout(chart_page);chart_layout->setContentsMargins(0,0,0,0);
    auto* chart_options=new QHBoxLayout;show_sma_=new QCheckBox("SMA");show_sma_->setChecked(true);show_ema_=new QCheckBox("EMA");show_bands_=new QCheckBox("Bollinger 20/2");
    oscillator_=new QComboBox;oscillator_->addItems({QStringLiteral("Без осциллятора"),"RSI 14","MACD 12/26/9"});oscillator_->setCurrentIndex(1);
    auto* reset=new QPushButton(QStringLiteral("Сбросить масштаб"));for(auto* c:{show_sma_,show_ema_,show_bands_})chart_options->addWidget(c);chart_options->addWidget(oscillator_);chart_options->addStretch();chart_options->addWidget(label(QStringLiteral("Колесо: масштаб • курсор: цена"),"color:#7f99b1;"));chart_options->addWidget(reset);chart_layout->addLayout(chart_options);chart_layout->addWidget(chart_,1);
    tabs_->addTab(chart_page,QStringLiteral("График и индикаторы"));tabs_->addTab(forecast_panel(),QStringLiteral("Учебные прогнозы"));tabs_->addTab(risk_panel(),QStringLiteral("Калькулятор и пороги"));tabs_->addTab(performance_panel(),QStringLiteral("Производительность"));tabs_->addTab(help_panel(),QStringLiteral("API и помощь"));
    candles_=new HistoryPanel;tabs_->addTab(candles_,QStringLiteral("Свечи MOEX"));tabs_->addTab(comparison_panel(),QStringLiteral("Сравнение"));
    lab_=new ResearchPanel;tabs_->addTab(lab_,QStringLiteral("Лаборатория"));
    // Each pane scrolls independently, so the tallest settings page cannot force the whole window off screen.
    for(int i=0;i<tabs_->count();++i){auto* page=tabs_->widget(i);const auto title=tabs_->tabText(i);tabs_->removeTab(i);auto* pane=new QScrollArea;pane->setWidgetResizable(true);pane->setFrameShape(QFrame::NoFrame);pane->setWidget(page);tabs_->insertTab(i,pane,title);}
    tabs_->setCurrentIndex(0);
    split->addWidget(table_);split->addWidget(tabs_);split->setStretchFactor(0,1);split->setStretchFactor(1,3);layout->addWidget(split,1);
    resource_text_=label("","color:#7f99b1;font-size:11px;");layout->addWidget(resource_text_);
    layout->addWidget(label(QStringLiteral("Индикаторы по принятым ценам, не по биржевым свечам. Горизонт прогноза — число обновлений. σ не годовая. Автоматического сохранения ключей и котировок нет."),"color:#7f99b1;font-size:11px;"));
    connect(start_,&QPushButton::clicked,this,&MainWindow::connect_feed);connect(stop_,&QPushButton::clicked,this,[this]{feed_.stop();set_controls(false);token_->clear();});
    connect(catalog_button_,&QPushButton::clicked,this,&MainWindow::show_catalog);connect(api,&QPushButton::clicked,this,[this]{tabs_->setCurrentIndex(4);});
    connect(&feed_,&MarketFeed::quote_received,this,&MainWindow::receive);connect(&feed_,&MarketFeed::status_changed,this,[this](const QString&s){text_if_changed(connection_,s);});
    connect(&feed_,&MarketFeed::diagnostic,this,[this](int kind,int count){if(kind==0)quality_invalid_+=count;else if(kind==1)quality_network_+=count;else quality_reconnect_+=count;});
    connect(&feed_,&MarketFeed::running_changed,this,[this](bool running){set_controls(running);if(!running)token_->clear();});
    connect(table_,&QTableWidget::itemSelectionChanged,this,&MainWindow::selection_changed);
    auto options=[this]{chart_->set_options(show_sma_->isChecked(),show_ema_->isChecked(),show_bands_->isChecked(),oscillator_->currentIndex());};
    for(auto*c:{show_sma_,show_ema_,show_bands_})connect(c,&QCheckBox::toggled,this,options);connect(oscillator_,&QComboBox::currentIndexChanged,this,options);connect(reset,&QPushButton::clicked,chart_,&ChartWidget::reset_view);
    connect(tabs_,&QTabWidget::currentChanged,this,[this]{selection_changed();});
    render_timer_.setSingleShot(true);render_timer_.setTimerType(Qt::CoarseTimer);connect(&render_timer_,&QTimer::timeout,this,&MainWindow::render);
    maintenance_timer_.setInterval(1000);maintenance_timer_.setTimerType(Qt::CoarseTimer);connect(&maintenance_timer_,&QTimer::timeout,this,&MainWindow::maintenance);maintenance_timer_.start();
    if(!test_mode_){QSettings s;symbols_->setText(s.value("symbols",symbols_->text()).toString());window_->setValue(s.value("window",20).toInt());frequency_->setValue(s.value("frequency",250).toInt());provider_->setCurrentIndex(0);performance_->setCurrentIndex(std::clamp(s.value("performance",0).toInt(),0,2));history_->setCurrentIndex(std::clamp(s.value("history",1).toInt(),0,2));poll_seconds_->setValue(s.value("poll_seconds",5).toInt());}
}
QWidget* MainWindow::forecast_panel(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);
    auto* note=label(QStringLiteral("ЭКСПЕРИМЕНТ • прогноз по текущей сессии • горизонт в обновлениях, не минутах • не торговая рекомендация"));note->setWordWrap(true);layout->addWidget(note);
    forecast_enabled_=new QCheckBox(QStringLiteral("Включить расчёт (до 1 раза/с)"));
    lookback_=new QSpinBox;lookback_->setRange(10,300);lookback_->setValue(30);horizon_=new QSpinBox;horizon_->setRange(1,20);horizon_->setValue(5);
    forecast_model_=new QComboBox;forecast_model_->addItems({QStringLiteral("Линейный тренд"),QStringLiteral("Наивная: последняя цена")});
    auto* options=new QHBoxLayout;options->addWidget(forecast_enabled_);options->addWidget(label(QStringLiteral("Окно")));options->addWidget(lookback_);options->addWidget(label(QStringLiteral("Горизонт")));options->addWidget(horizon_);options->addWidget(forecast_model_);layout->addLayout(options);
    forecast_text_=label(QStringLiteral("Расчёт выключен."));forecast_text_->setWordWrap(true);layout->addWidget(forecast_text_);
    forecast_chart_=new VisualChart;forecast_chart_->clear(QStringLiteral("Включите эксперимент после накопления данных"));layout->addWidget(forecast_chart_,1);
    auto* foot=label(QStringLiteral("MAE/RMSE в рублях. Линии к прогнозу — визуальный ориентир; ±2 RMSE показывает прошлую ошибку, не доверительный интервал. Будущих цен здесь нет."),"color:#91a7be;");foot->setWordWrap(true);layout->addWidget(foot);
    connect(forecast_enabled_,&QCheckBox::toggled,this,[this]{forecast_dirty_=true;update_forecast();});
    connect(lookback_,&QSpinBox::valueChanged,this,[this]{forecast_dirty_=true;});connect(horizon_,&QSpinBox::valueChanged,this,[this]{forecast_dirty_=true;});connect(forecast_model_,&QComboBox::currentIndexChanged,this,[this]{forecast_dirty_=true;});return page;
}
QWidget* MainWindow::risk_panel(){
    auto* page=new QWidget;auto* layout=new QHBoxLayout(page);layout->setContentsMargins(20,16,20,16);
    auto* calc=new QVBoxLayout;calc->addWidget(label(QStringLiteral("Модель размера длинной позиции • без отправки заявок"),"font-weight:600;"));auto* form=new QFormLayout;
    capital_=number(100000);risk_=number(1,0.01,100);entry_=number(100);stop_price_=number(95);target_=number(110);lot_=new QSpinBox;lot_->setRange(1,1000000);lot_->setValue(1);
    form->addRow(QStringLiteral("Капитал, ₽"),capital_);form->addRow(QStringLiteral("Бюджет риска, %"),risk_);form->addRow(QStringLiteral("Вход, ₽ / акция"),entry_);form->addRow(QStringLiteral("Стоп, ₽ / акция"),stop_price_);form->addRow(QStringLiteral("Цель, ₽ / акция"),target_);form->addRow(QStringLiteral("Размер биржевого лота"),lot_);calc->addLayout(form);
    auto* fill=new QPushButton(QStringLiteral("Подставить выбранную цену и лот"));calc->addWidget(fill);risk_text_=label("");risk_text_->setWordWrap(true);calc->addWidget(risk_text_);calc->addWidget(label(QStringLiteral("Расчёт ограничен капиталом. Не учитывает комиссии,\nпроскальзывание, гэпы и исполнение стопа."),"color:#91a7be;"));calc->addStretch();layout->addLayout(calc,1);
    auto* alerts=new QVBoxLayout;alerts->addWidget(label(QStringLiteral("Одноразовый ценовой порог выбранной акции"),"font-weight:600;"));alert_level_=number(100);alert_direction_=new QComboBox;alert_direction_->addItems({QStringLiteral("Цена не ниже порога ≥"),QStringLiteral("Цена не выше порога ≤")});alerts->addWidget(alert_direction_);alerts->addWidget(alert_level_);auto* arm=new QPushButton(QStringLiteral("Включить порог"));auto* disarm=new QPushButton(QStringLiteral("Снять порог"));alerts->addWidget(arm);alerts->addWidget(disarm);
    alert_text_=label(QStringLiteral("Пороги не включены."));alert_text_->setWordWrap(true);alerts->addWidget(alert_text_);alerts->addWidget(label(QStringLiteral("Сигнал появляется внутри приложения один раз.\nОн не размещает сделку и не гарантирует цену исполнения.\nПри новом подключении пороги очищаются."),"color:#91a7be;"));alerts->addStretch();layout->addSpacing(30);layout->addLayout(alerts,1);
    for(auto* field:{capital_,risk_,entry_,stop_price_,target_})connect(field,&QDoubleSpinBox::valueChanged,this,[this]{calculate_risk();});connect(lot_,&QSpinBox::valueChanged,this,[this]{calculate_risk();});
    connect(fill,&QPushButton::clicked,this,[this]{auto*row=selected_row();if(row&&row->stats.size()){entry_->setValue(row->quote.last);const int known_lot=catalog_.lot_size(selected_symbol());if(known_lot>0)lot_->setValue(known_lot);else risk_text_->setText(risk_text_->text()+QStringLiteral("\nЛот не загружен: откройте каталог и проверьте размер."));}});
    connect(arm,&QPushButton::clicked,this,&MainWindow::arm_alert);connect(disarm,&QPushButton::clicked,this,[this]{if(auto*row=selected_row())row->alert_armed=false;alert_text_->setText(QStringLiteral("Порог выбранной акции снят."));});calculate_risk();return page;
}
QWidget* MainWindow::performance_panel(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(20,16,20,16);auto* form=new QFormLayout;
    performance_=new QComboBox;performance_->addItems({QStringLiteral("Экономичный — до 2 кадров/с"),QStringLiteral("Обычный — до 4 кадров/с"),QStringLiteral("Плавный — до 10 кадров/с")});
    history_=new QComboBox;history_->addItems({QStringLiteral("500 точек / акция"),QStringLiteral("2 000 точек / акция"),QStringLiteral("5 000 точек / акция")});history_->setCurrentIndex(1);
    poll_seconds_=new QSpinBox;poll_seconds_->setRange(5,60);poll_seconds_->setValue(5);poll_seconds_->setSuffix(QStringLiteral(" с"));
    form->addRow(QStringLiteral("Перерисовка"),performance_);form->addRow(QStringLiteral("Лимит истории (до подключения)"),history_);form->addRow(QStringLiteral("MOEX ISS: период опроса"),poll_seconds_);layout->addLayout(form);
    auto* text=label(QStringLiteral("• Один пакетный HTTP-запрос вместо отдельного запроса на каждый тикер.\n• История заранее выделена и не растёт после лимита; 20 акций максимум.\n• Нет копирования истории в график и перерисовки без новых данных.\n• Свёрнутое окно не рисует график. Индикаторы продолжают получать данные.\n• Прогнозы выключены по умолчанию и работают только на открытой вкладке.\n\nCPU внизу — доля одного логического ядра, RAM — рабочий набор всего процесса с Qt.\nЦифры зависят от ОС, драйверов, числа тикеров и частоты источника."));text->setWordWrap(true);layout->addWidget(text);layout->addStretch();
    connect(performance_,&QComboBox::currentIndexChanged,this,[this](int mode){redraw_ms_=mode==0?500:mode==1?250:100;save_settings();});return page;
}
QWidget* MainWindow::help_panel(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(20,16,20,16);
    auto* text=label(QStringLiteral("Основной режим MOEX ISS: регистрация, логин, пароль и ключ НЕ нужны. Свечи также загружаются напрямую из публичного API.\nНет аналитики посещений, рекламы, облачного аккаунта приложения и запросов к портфелю. IP-адрес видит сервер API, как при любом HTTPS-запросе.\nНастройки хранят только тикеры и параметры интерфейса; токены не сохраняются; цены, капитал и исследования можно явно экспортировать локально во вкладке лаборатории.\n\nДополнительный режим АЛОР: зарегистрируйтесь на портале разработчика, привяжите боевой торговый аккаунт и выпустите Refresh token.\nОдной регистрации разработчика недостаточно. Тестовый контур не даёт реальные котировки.\n\nMOEX ISS: API-ключ для публичного режима не нужен; данные имеют задержку.\nПриложение не выполняет торговых операций. Токен вводится только в главном окне и не сохраняется."));text->setWordWrap(true);layout->addWidget(text);
    auto add=[layout,this](const QString& name,const char* url){auto*b=new QPushButton(name);layout->addWidget(b);connect(b,&QPushButton::clicked,this,[url]{QDesktopServices::openUrl(QUrl(QString::fromLatin1(url)));});};
    add(QStringLiteral("АЛОР — получить Refresh token"),"https://alor.dev/open-api-tokens");add(QStringLiteral("АЛОР — инструкция подключения боевого аккаунта"),"https://alor.dev/docs/api/quick-start/prod-env");add(QStringLiteral("MOEX ISS — описание API и условий доступа"),"https://www.moex.com/a8531");add(QStringLiteral("Документация приложения"),"https://archibald02.github.io/Vega-C-Novikov/");auto* clear=new QPushButton(QStringLiteral("Отключиться и очистить настройки / данные сессии"));clear->setObjectName("resetLocalData");layout->addWidget(clear);connect(clear,&QPushButton::clicked,this,&MainWindow::reset_local_data);layout->addStretch();return page;
}
QWidget* MainWindow::comparison_panel(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* note=label(QStringLiteral("Первые шесть выбранных акций • изменение от первой сохранённой цены каждой акции, % • только текущая сессия"));note->setWordWrap(true);layout->addWidget(note);
    comparison_=new VisualChart;comparison_->clear(QStringLiteral("Подключите источник для сравнения динамики"));layout->addWidget(comparison_,1);
    auto* foot=label(QStringLiteral("Начальные моменты могут различаться. При заполнении кольцевого буфера база сдвигается. Это не доходность портфеля; дивиденды и комиссии не учтены."));foot->setWordWrap(true);layout->addWidget(foot);return page;
}
void MainWindow::update_comparison(){
    if(tabs_->currentIndex()!=6||!comparison_dirty_)return;comparison_dirty_=false;QVector<PlotLine> lines;
    const QStringList colors{"#39d2b0","#829eff","#efc571","#f08c98","#b99fe8","#6ec8e5"};
    for(int i=0;i<std::min(6,static_cast<int>(tickers_.size()));++i){const auto& points=rows_.at(tickers_[i]).points;if(points.size()<2)continue;PlotLine line{tickers_[i],QColor(colors[i]),{},false};const double base=points[0].price;const auto step=std::max<std::size_t>(1,(points.size()+498)/499);for(std::size_t j=0;j<points.size();j+=step)line.points.push_back(QPointF((points[j].time_ms-session_start_ms_)/1000.0,(points[j].price/base-1)*100));const auto& last=points.back();if(line.points.size()>=500)line.points.removeLast();line.points.push_back(QPointF((last.time_ms-session_start_ms_)/1000.0,(last.price/base-1)*100));lines.push_back(line);}
    if(lines.isEmpty())comparison_->clear(QStringLiteral("Нужно минимум два обновления цены"));else comparison_->set_lines(lines,QStringLiteral("Сравнение динамики • %"),QStringLiteral("Секунды с подключения по временным меткам источника • до 500 точек на линию"));
}
void MainWindow::reset_local_data(){
    feed_.stop();render_timer_.stop();chart_->set_series(nullptr,"","",0);rows_.clear();tickers_.clear();table_->setRowCount(0);token_->clear();candles_->clear_private_state();lab_->clear_private_state();comparison_->clear();forecast_chart_->clear();forecast_enabled_->setChecked(false);forecast_text_->setText(QStringLiteral("Расчёт выключен."));
    metric_price_->setText(QStringLiteral("—"));metric_sma_->setText(QStringLiteral("—"));metric_vol_->setText(QStringLiteral("—"));metric_count_->setText(QStringLiteral("—"));indicator_text_->setText(QStringLiteral("Данные сессии очищены."));alert_text_->setText(QStringLiteral("Пороги очищены."));
    symbols_->setText("SBER, GAZP, LKOH, YDEX");provider_->setCurrentIndex(0);capital_->setValue(100000);risk_->setValue(1);entry_->setValue(100);stop_price_->setValue(95);target_->setValue(110);lot_->setValue(1);alert_level_->setValue(100);window_->setValue(20);frequency_->setValue(250);performance_->setCurrentIndex(0);history_->setCurrentIndex(1);poll_seconds_->setValue(5);lookback_->setValue(30);horizon_->setValue(5);oscillator_->setCurrentIndex(1);show_ema_->setChecked(false);show_bands_->setChecked(false);
    bool settings_cleared=true;if(!test_mode_){QSettings settings;settings.clear();settings.sync();settings_cleared=settings.status()==QSettings::NoError;}connection_->setText(settings_cleared?QStringLiteral("Отключено • данные и настройки очищены"):QStringLiteral("Данные сессии очищены, но системные настройки удалить не удалось."));
}
void MainWindow::save_settings(){if(test_mode_)return;QSettings s;s.setValue("symbols",symbols_->text());s.setValue("window",window_->value());s.setValue("frequency",frequency_->value());s.setValue("provider",provider_->currentIndex());s.setValue("performance",performance_->currentIndex());s.setValue("history",history_->currentIndex());s.setValue("poll_seconds",poll_seconds_->value());}
void MainWindow::set_controls(bool connected){provider_->setEnabled(!connected);symbols_->setEnabled(!connected);window_->setEnabled(!connected);catalog_button_->setEnabled(!connected);history_->setEnabled(!connected);poll_seconds_->setEnabled(!connected);token_->setEnabled(!connected&&provider_->currentIndex()==1);frequency_->setEnabled(!connected&&provider_->currentIndex()==1);start_->setEnabled(!connected);stop_->setEnabled(connected);}
void MainWindow::show_catalog(){QStringList selected;try{selected=parse_symbols(symbols_->text());}catch(...){}CatalogDialog dialog(catalog_,selected,this);if(dialog.exec()==QDialog::Accepted){symbols_->setText(dialog.selection().join(", "));save_settings();}}
void MainWindow::connect_feed(){
    try{tickers_=parse_symbols(symbols_->text());}catch(...){QMessageBox::warning(this,QStringLiteral("Тикеры"),QStringLiteral("Введите от 1 до 20 корректных тикеров или выберите из каталога."));return;}
    if(provider_->currentIndex()==1&&token_->text().trimmed().isEmpty()){tabs_->setCurrentIndex(4);QMessageBox::warning(this,QStringLiteral("Токен"),QStringLiteral("Введите боевой Refresh token АЛОР. Ссылки получения доступны во вкладке API и помощь."));return;}
    save_settings();session_start_ms_=QDateTime::currentMSecsSinceEpoch();comparison_dirty_=true;comparison_->clear();forecast_chart_->clear();render_timer_.stop();chart_->set_series(nullptr,"","",0);rows_.clear();
    const QSignalBlocker blocked(table_);table_->setRowCount(tickers_.size());const int capacities[]={500,2000,5000};
    for(int i=0;i<tickers_.size();++i){rows_.emplace(tickers_[i],Row(static_cast<std::size_t>(window_->value()),static_cast<std::size_t>(capacities[history_->currentIndex()])));for(int j=0;j<table_->columnCount();++j)table_->setItem(i,j,new QTableWidgetItem(j==0?tickers_[i]:QStringLiteral("—")));}
    table_->selectRow(0);forecast_dirty_=true;alert_text_->setText(QStringLiteral("Пороги не включены."));
    MarketFeed::Config config;config.symbols=tickers_;config.refresh_token=provider_->currentIndex()==1?token_->text().trimmed():QString();config.interval_ms=frequency_->value();config.poll_interval_ms=poll_seconds_->value()*1000;
    config.mode=provider_->currentIndex()==0?MarketFeed::Mode::Moex:provider_->currentIndex()==1?MarketFeed::Mode::Alor:MarketFeed::Mode::Demo;
    feed_.start(config);candles_->set_symbol(tickers_.front());schedule_render();
}
void MainWindow::receive(const Quote&q){
    auto it=rows_.find(q.symbol);if(it==rows_.end())return;auto& row=it->second;
    ++quality_rx_;
    if(!std::isfinite(q.last)||q.last<=0||q.last>1e12||q.market_time_ms<=0||q.market_time_ms>QDateTime::currentMSecsSinceEpoch()+300000){++quality_invalid_;return;}if(row.stats.size()&&q.market_time_ms<row.quote.market_time_ms){++quality_old_;return;}
    const bool distinct=row.stats.size()==0||q.market_time_ms!=row.quote.market_time_ms||q.last!=row.quote.last;
    if(!distinct)++quality_dup_;
    if(distinct){try{row.stats.push(q.last);const auto indicators=row.indicators.push(q.last);row.points.push({q.market_time_ms,q.last,indicators});++row.revision;comparison_dirty_=true;}catch(...){return;}}
    row.received=QDateTime::currentMSecsSinceEpoch();row.quote=q;row.dirty=true;
    if(row.alert_armed && (row.alert_above?q.last>=row.alert_level:q.last<=row.alert_level)){row.alert_armed=false;alert_text_->setText(QStringLiteral("ПОРОГ ДОСТИГНУТ • %1 • %2 ₽ • %3").arg(q.symbol,fmt(q.last),QDateTime::currentDateTime().toString("HH:mm:ss")));alert_text_->setStyleSheet("color:#f1c879;font-weight:600;");}
    schedule_render();
}
void MainWindow::schedule_render(){if(isVisible()&&!isMinimized()&&!render_timer_.isActive())render_timer_.start(redraw_ms_);}
QString MainWindow::selected_symbol()const{const int i=table_->currentRow();return i>=0&&i<tickers_.size()?tickers_[i]:QString();}
MainWindow::Row* MainWindow::selected_row(){const auto it=rows_.find(selected_symbol());return it==rows_.end()?nullptr:&it->second;}
void MainWindow::selection_changed(){table_->setVisible(tabs_->currentIndex()!=7);forecast_dirty_=true;comparison_dirty_=true;if(candles_)candles_->set_symbol(selected_symbol());if(lab_)lab_->set_symbol(selected_symbol());render();update_forecast();}
void MainWindow::render(){
    if(!isVisible()||isMinimized())return;
    for(int i=0;i<tickers_.size();++i){auto&row=rows_.at(tickers_[i]);if(!row.dirty||!row.stats.size())continue;row.dirty=false;const auto&q=row.quote;
        const double spread=std::isfinite(q.bid)&&std::isfinite(q.ask)&&q.ask>=q.bid&&q.bid>0?(q.ask-q.bid)/((q.ask+q.bid)/2)*10000:quant::unavailable();
        const QStringList values{q.symbol,fmt(q.last),fmt(q.bid),fmt(q.ask),fmt(spread),fmt(q.change_percent),fmt(row.stats.mean_price()),QDateTime::fromMSecsSinceEpoch(q.market_time_ms).toLocalTime().toString("dd.MM HH:mm:ss"),"0"};
        for(int j=0;j<values.size();++j)if(table_->item(i,j)->text()!=values[j])table_->item(i,j)->setText(values[j]);
        table_->item(i,5)->setForeground(std::isfinite(q.change_percent)&&q.change_percent<0?QColor("#f28e97"):QColor("#4dd6b7"));
    }
    const auto*row=selected_row();if(!row)return;
    text_if_changed(metric_price_,row->stats.size()?fmt(row->quote.last):QStringLiteral("—"));text_if_changed(metric_sma_,row->stats.size()?fmt(row->stats.mean_price()):QStringLiteral("—"));text_if_changed(metric_count_,QString::number(row->stats.size())+" / "+QString::number(window_->value()));text_if_changed(metric_vol_,row->stats.size()>=3?fmt(row->stats.return_stddev()*100,4):QStringLiteral("—"));
    if(row->points.size()){const auto&v=row->points.back().indicators;text_if_changed(indicator_text_,QStringLiteral("EMA %1: %2 ₽   |   RSI 14: %3   |   MACD: %4 / сигнал %5   |   Bollinger 20/2: %6 … %7 ₽").arg(window_->value()).arg(fmt(v.ema),fmt(v.rsi,1),fmt(v.macd,4),fmt(v.signal,4),fmt(v.lower),fmt(v.upper)));}
    const QString detail=provider_->currentIndex()==0?QStringLiteral("Снимки ISS с задержкой • по принятым обновлениям"):provider_->currentIndex()==1?QStringLiteral("Последняя цена сделки • по принятым обновлениям"):QStringLiteral("Демонстрационные синтетические цены");
    chart_->set_series(&row->points,selected_symbol()+" / RUB",detail,row->revision);
    update_comparison();
}
void MainWindow::maintenance(){
    if(!isVisible()||isMinimized())return;
    const auto now=QDateTime::currentMSecsSinceEpoch();std::size_t count=0,bytes=0;bool dirty=false;
    for(int i=0;i<tickers_.size();++i){auto&row=rows_.at(tickers_[i]);count+=row.points.size();bytes+=row.points.allocated_bytes();dirty|=row.dirty;if(!row.received)continue;const auto age=std::max<qint64>(0,now-row.received)/1000;auto*item=table_->item(i,8);item->setToolTip(QStringLiteral("Возраст последнего ответа, не задержка биржевых данных. При отключении значение продолжает расти."));const auto value=QString::number(age);if(item->text()!=value)item->setText(value);item->setForeground(age>std::max(30,provider_->currentIndex()==0?poll_seconds_->value()*3:30)?QColor("#e6bb6b"):QColor("#a9bfd3"));}
    lab_->set_feed_quality(QStringLiteral("Поток текущего запуска: принято %1; повторов цена+время %2; устаревших %3; некорректных %4; сетевых/серверных ошибок %5; попыток восстановления %6.\nСостояние: %7").arg(quality_rx_).arg(quality_dup_).arg(quality_old_).arg(quality_invalid_).arg(quality_network_).arg(quality_reconnect_).arg(connection_->text()));
    if(dirty)schedule_render();update_forecast();const auto resource=resources_.sample();
    text_if_changed(resource_text_,QStringLiteral("CPU: %1% ядра   •   RAM процесса: %2 МиБ   •   Точек: %3   •   Буфер истории: %4 МиБ   •   Перерисовок графика: %5").arg(resource.cpu_percent,0,'f',1).arg(resource.memory_mib,0,'f',1).arg(count).arg(bytes/(1024.0*1024),0,'f',2).arg(chart_->rebuild_count()));
}
void MainWindow::update_forecast(){
    if(!forecast_enabled_->isChecked()){text_if_changed(forecast_text_,QStringLiteral("Расчёт выключен."));forecast_chart_->clear(QStringLiteral("Учебный прогноз выключен"));return;}
    if(tabs_->currentIndex()!=1||!isVisible()||isMinimized())return;
    const auto*row=selected_row();if(!row||row->points.size()<static_cast<std::size_t>(lookback_->value())){text_if_changed(forecast_text_,QStringLiteral("Нужно накопить минимум %1 цен выбранной акции.").arg(lookback_->value()));forecast_chart_->clear(QStringLiteral("Недостаточно наблюдений для прогноза"));return;}
    if(!forecast_dirty_&&forecast_symbol_==selected_symbol()&&forecast_revision_==row->revision)return;
    forecast_dirty_=false;forecast_symbol_=selected_symbol();forecast_revision_=row->revision;
    std::vector<double> prices;const auto n=std::min<std::size_t>(row->points.size(),500);prices.reserve(n);for(std::size_t i=row->points.size()-n;i<row->points.size();++i)prices.push_back(row->points[i].price);
    quant::ForecastResult f;try{f=quant::experimental_forecast(prices,lookback_->value(),horizon_->value());}catch(const std::exception&){text_if_changed(forecast_text_,QStringLiteral("Некорректные данные для модели; расчёт пропущен."));forecast_chart_->clear();return;}const bool trend=forecast_model_->currentIndex()==0;
    QString text=QStringLiteral("%1 • на %2 следующих обновлений\nТренд: %3 ₽    |    Последняя цена: %4 ₽\nРетропроверка горизонта %2: %5 наблюдений").arg(selected_symbol()).arg(horizon_->value()).arg(fmt(f.trend),fmt(f.naive)).arg(f.evaluations);
    if(f.evaluations>=20){const auto error=trend?f.trend_rmse:quant::unavailable();text+=QStringLiteral("\nMAE тренда: %1 ₽    |    MAE наивной: %2 ₽\nВыбранный ориентир: %3 ₽").arg(fmt(f.trend_mae,4),fmt(f.naive_mae,4),fmt(trend?f.trend:f.naive));if(trend)text+=QStringLiteral("\nКоридор прошлой ошибки ±2 RMSE: %1 … %2 ₽ (без гарантии)").arg(fmt(f.trend-2*error),fmt(f.trend+2*error));text+=f.trend_mae<f.naive_mae?QStringLiteral("\nНа этом отрезке MAE тренда ниже наивной модели."):QStringLiteral("\nТренд не улучшил наивную модель на этом отрезке.");}
    else text+=QStringLiteral("\nДля оценки ошибки нужно минимум 20 проверочных наблюдений.");
    text_if_changed(forecast_text_,text);
    QVector<PlotLine> lines;
    PlotLine actual{QStringLiteral("Принятые цены"),QColor("#39d2b0"),{},false};
    const int start=std::max(0,static_cast<int>(prices.size())-80);for(int i=start;i<static_cast<int>(prices.size());++i)actual.points.push_back(QPointF(i-start,prices[i]));
    const double x=actual.points.back().x(), last=prices.back(), end=x+horizon_->value();lines.push_back(actual);
    lines.push_back({QStringLiteral("К тренду"),QColor("#829eff"),{{x,last},{end,f.trend}},true});
    lines.push_back({QStringLiteral("Цена без изменения"),QColor("#edc572"),{{x,last},{end,f.naive}},true});
    if(f.evaluations>=20){lines.push_back({QStringLiteral("±2 RMSE (эвристика)"),QColor("#a47fab"),{{x,last},{end,f.trend+2*f.trend_rmse}},true});lines.push_back({QString(),QColor("#a47fab"),{{x,last},{end,f.trend-2*f.trend_rmse}},true});}
    forecast_chart_->set_lines(lines,selected_symbol()+QStringLiteral(" • цены и учебная экстраполяция"),QStringLiteral("Сплошная линия — принятые цены; пунктир — расчёт, не реальные будущие данные"));
}
void MainWindow::calculate_risk(){try{const auto p=quant::size_long_position(capital_->value(),risk_->value(),entry_->value(),stop_price_->value(),target_->value(),lot_->value());risk_text_->setText(QStringLiteral("Лотов: %1 • акций: %2\nСтоимость: %3 ₽ • модель риска: %4 ₽\nРезультат у цели: %5 ₽ • прибыль/риск: %6").arg(p.lots).arg(p.units).arg(fmt(p.cost),fmt(p.risk),fmt(p.reward),fmt(p.reward_risk)));}catch(...){risk_text_->setText(QStringLiteral("Для длинной позиции: 0 < стоп < вход < цель.\nПроверьте капитал, риск и размер лота."));}}
void MainWindow::arm_alert(){auto*row=selected_row();if(!row){alert_text_->setText(QStringLiteral("Сначала подключите источник и выберите строку."));return;}row->alert_level=alert_level_->value();row->alert_above=alert_direction_->currentIndex()==0;row->alert_armed=true;alert_text_->setStyleSheet("color:#91a7be;");alert_text_->setText(QStringLiteral("Порог включён: %1 %2 %3 ₽").arg(selected_symbol(),row->alert_above?QStringLiteral("≥"):QStringLiteral("≤"),fmt(row->alert_level)));}
void MainWindow::start_demo_test(int count){provider_->setCurrentIndex(2);QStringList list;for(int i=0;i<count;++i)list<<(i==0?"SBER":i==1?"GAZP":i==2?"LKOH":i==3?"YDEX":"TEST"+QString::number(i));symbols_->setText(list.join(", "));connect_feed();}
void MainWindow::start_soak_test(){history_->setCurrentIndex(0);start_demo_test(20);}
bool MainWindow::soak_ready() const {if(rows_.size()!=20)return false;for(const auto& row:rows_)if(row.second.points.capacity()!=500||row.second.points.size()!=500||!std::isfinite(row.second.points.back().price))return false;return true;}
void MainWindow::start_moex_test(){provider_->setCurrentIndex(0);symbols_->setText("SBER, GAZP");connect_feed();}
bool MainWindow::has_demo_data()const{if(tabs_->currentIndex()!=0)return false;if(rows_.empty()||rows_.size()!=static_cast<std::size_t>(table_->rowCount()))return false;for(const auto&row:rows_)if(row.second.stats.size()<3||!std::isfinite(row.second.stats.mean_price()))return false;return true;}
bool MainWindow::has_any_data()const{for(const auto&row:rows_)if(row.second.stats.size())return true;return false;}
void MainWindow::stop_for_test(){feed_.stop();render_timer_.stop();}
bool MainWindow::exercise_tools_test(){
    stop_for_test();if(rows_.empty())return false;chart_->set_series(nullptr,"","",0);for(auto& row:rows_)row.second=Row(window_->value(),row.second.points.capacity());
    const qint64 base=QDateTime::currentMSecsSinceEpoch();for(int i=1;i<=120;++i)for(const auto&symbol:tickers_){const auto k=tickers_.indexOf(symbol);Quote q;q.symbol=symbol;q.last=200+std::sin(i*0.1+k)*3+i*(0.01+k*0.01);q.market_time_ms=base+i;q.bid=q.last-0.01;q.ask=q.last+0.01;receive(q);}
    render();show_ema_->setChecked(true);show_bands_->setChecked(true);oscillator_->setCurrentIndex(2);tabs_->setCurrentIndex(1);forecast_enabled_->setChecked(true);update_forecast();
    if(forecast_text_->text().contains(QStringLiteral("нужно"),Qt::CaseInsensitive)||!forecast_text_->text().contains(QStringLiteral("MAE")))return false;
    capital_->setValue(100000);risk_->setValue(1);entry_->setValue(100);stop_price_->setValue(95);target_->setValue(110);lot_->setValue(10);calculate_risk();
    if(!risk_text_->text().contains(QStringLiteral("Лотов: 20")))return false;
    alert_level_->setValue(1);arm_alert();auto*row=selected_row();Quote q=row->quote;q.market_time_ms+=1000;receive(q);if(!alert_text_->text().contains(QStringLiteral("ДОСТИГНУТ")))return false;
    tabs_->setCurrentIndex(0);render();return true;
}
bool MainWindow::exercise_lab_test(){stop_for_test();tabs_->setCurrentIndex(7);return lab_->self_test();}
bool MainWindow::exercise_visuals_test(){
    if(!exercise_tools_test())return false;
    QVector<Candle> bars;const auto base=QDateTime::currentMSecsSinceEpoch()-90LL*86400000;
    for(int i=0;i<90;++i){const double open=100+0.15*i+std::sin(i*0.25)*4, close=open+std::sin(i*0.73)*1.5;bars.push_back({base+i*86400000LL,open,std::max(open,close)+0.9,std::min(open,close)-0.8,close,1000000.0+(i%9)*200000});}
    candles_->show_test_bars(bars);tabs_->setCurrentIndex(6);update_comparison();tabs_->setCurrentIndex(1);update_forecast();
    for(const auto& size:{QSize(1000,720),QSize(1280,800),QSize(1440,1000)}){resize(size);for(int tab=0;tab<tabs_->count();++tab){tabs_->setCurrentIndex(tab);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);}}
    for(auto* widget:findChildren<QWidget*>())if(dynamic_cast<VisualChart*>(widget)||widget==chart_){
        for(int i=0;i<25;++i){QWheelEvent wheel(QPointF(130,120),QPointF(130,120),QPoint(),QPoint(0,i%2?120:-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(widget,&wheel);}
        QMouseEvent hover(QEvent::MouseMove,QPointF(150,120),QPointF(150,120),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(widget,&hover);
        QEvent leave(QEvent::Leave);QApplication::sendEvent(widget,&leave);
    }
    showMinimized();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);showNormal();resize(1440,1000);tabs_->setCurrentIndex(5);return true;
}
bool MainWindow::exercise_privacy_test(){
    if(!lab_->privacy_test())return false;
    if(!token_->text().isEmpty()||provider_->currentIndex()!=0||!token_->isHidden())return false;
    provider_->setCurrentIndex(1);token_->setText("SYNTHETIC_TEST_TOKEN");save_settings();
    {QSettings settings;settings.sync();const QStringList allowed{"symbols","window","frequency","provider","performance","history","poll_seconds"};for(const auto& key:settings.allKeys())if(!allowed.contains(key)||settings.value(key).toString().contains("SYNTHETIC_TEST_TOKEN")||settings.value(key).toString().contains("ENVIRONMENT_CANARY"))return false;}
    provider_->setCurrentIndex(0);if(!token_->text().isEmpty())return false;
    for(int i=0;i<30;++i){start_demo_test(20);stop_for_test();}reset_local_data();{QSettings settings;if(!settings.allKeys().isEmpty())return false;}return rows_.empty()&&tickers_.isEmpty()&&token_->text().isEmpty()&&table_->rowCount()==0;
}
void MainWindow::select_visual_test(int index){tabs_->setCurrentIndex(index);if(index==1){forecast_dirty_=true;update_forecast();}}
QString MainWindow::performance_status() const {return resource_text_->text();}
bool MainWindow::stress_test(){
    stop_for_test();std::size_t allocated=0;for(const auto&row:rows_)allocated+=row.second.points.allocated_bytes();const qint64 base=QDateTime::currentMSecsSinceEpoch();
    for(int i=1;i<=25000;++i)for(const auto&symbol:tickers_){Quote q;q.symbol=symbol;q.last=100+(i%1000)*0.001;q.market_time_ms=base+i;receive(q);}
    std::size_t after=0;for(const auto&row:rows_){if(row.second.points.size()!=row.second.points.capacity())return false;after+=row.second.points.allocated_bytes();}
    render();return allocated==after;
}
}
