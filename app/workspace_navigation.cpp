#include "main_window.hpp"
#include "ui_helpers.hpp"
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
namespace live {
QWidget *MainWindow::home_panel() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 18, 24, 24);
    auto *title = ui::paragraph(QStringLiteral("С чего начнём?"));
    title->setStyleSheet("font-size:26px;font-weight:600;");
    layout->addWidget(title);
    layout->addWidget(
        ui::paragraph(QStringLiteral("Выберите задачу. Для публичных данных MOEX не нужны логин, пароль или "
                                     "API-ключ. Данные биржи поступают с задержкой.")));
    auto *cards = new QHBoxLayout;
    cards->setSpacing(14);
    layout->addLayout(cards);
    auto card = [&](const QString &heading, const QString &description, const QString &action,
                    auto callback) {
        auto *box = new QFrame;
        box->setObjectName("startCard");
        box->setStyleSheet(
            "QFrame#startCard{background:#132235;border:1px solid #2b4358;border-radius:8px;}");
        auto *column = new QVBoxLayout(box);
        column->setContentsMargins(20, 16, 20, 16);
        auto *h = ui::paragraph(heading);
        h->setStyleSheet("font-size:19px;font-weight:600;border:0;");
        column->addWidget(h);
        column->addWidget(ui::paragraph(description));
        auto *button = new QPushButton(action);
        button->setObjectName("connectButton");
        column->addWidget(button, 0, Qt::AlignLeft);
        connect(button, &QPushButton::clicked, this, callback);
        cards->addWidget(box, 1);
    };
    card(QStringLiteral("Посмотреть цены акций"),
         QStringLiteral(
             "Таблица и график текущих обновлений. Выберите акции из каталога, затем подключите источник."),
         QStringLiteral("Открыть котировки"), [this] {
             tabs_->setCurrentIndex(0);
             symbols_->setFocus();
         });
    card(QStringLiteral("Исследовать данные и стратегию"),
         QStringLiteral("Закрытые свечи → качество данных → анализ → правила → результат → устойчивость. Все "
                        "сделки виртуальные."),
         QStringLiteral("Начать исследование"), [this] {
             tabs_->setCurrentIndex(7);
             lab_->set_test_view(6);
         });
    card(QStringLiteral("Попробовать без подключения"),
         QStringLiteral("Готовый синтетический пример. Можно пройти все шаги и увидеть отчёт, не "
                        "регистрируясь и не отправляя сетевых запросов."),
         QStringLiteral("Открыть учебное ДЕМО"), [this] {
             tabs_->setCurrentIndex(7);
             lab_->show_demo();
         });
    layout->addWidget(ui::paragraph(QStringLiteral(
        "Ctrl+1 — начало   •   Ctrl+2 — котировки   •   Ctrl+3 — исследование • Ctrl+4 — задания\nПроекты и "
        "отчёты сохраняются только по вашему действию. Токены не записываются на диск.")));
    auto *lessons =
        new QPushButton(QStringLiteral("Понять комиссии, риск и подбор параметров — учебные задания"));
    connect(lessons, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(9); });
    layout->addWidget(lessons, 0, Qt::AlignLeft);
    auto *portfolio = new QPushButton(QStringLiteral("Исследовать портфель нескольких акций"));
    connect(portfolio, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(10); });
    layout->addWidget(portfolio, 0, Qt::AlignLeft);
    layout->addStretch();
    return page;
}
void MainWindow::update_workspace() {
    const int index = tabs_->currentIndex();
    const bool market = index == 0 || index == 1 || index == 2 || index == 5 || index == 6;
    quote_controls_->setVisible(market && index != 5);
    quote_metrics_->setVisible(market && index != 5);
    table_->setVisible(market && index != 5);
    stream_footer_->setVisible(market && index != 5);
    home_nav_->setChecked(index == 8);
    quotes_nav_->setChecked(market);
    lab_nav_->setChecked(index == 7);
    learning_nav_->setChecked(index == 9);
    portfolio_nav_->setChecked(index == 10);
    const QStringList names{
        QStringLiteral("Котировки • график текущих обновлений"),
        QStringLiteral("Котировки • учебный прогноз"),
        QStringLiteral("Котировки • размер позиции и уведомления"),
        QStringLiteral("Настройки производительности"),
        QStringLiteral("Подключения и помощь"),
        QStringLiteral("Свечи MOEX • загрузка отдельно от потока котировок"),
        QStringLiteral("Котировки • сравнение акций"),
        QStringLiteral("Исследование • отдельный набор закрытых свечей, только виртуальные сделки"),
        QStringLiteral("Начало • выберите задачу"),
        QStringLiteral("Учебные задания • автономные синтетические примеры"),
        QStringLiteral("Портфель • общий виртуальный капитал и синхронные свечи")};
    workspace_context_->setText(names.value(index));
}
} // namespace live
