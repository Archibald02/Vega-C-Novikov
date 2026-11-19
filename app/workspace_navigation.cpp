#include "main_window.hpp"
#include "ui_helpers.hpp"
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>
namespace live {
QWidget *MainWindow::home_panel() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(2, 4, 2, 10);
    layout->setSpacing(14);

    auto *welcome = new QFrame;
    welcome->setObjectName("welcomePanel");
    auto *hero = new QHBoxLayout(welcome);
    hero->setContentsMargins(28, 24, 28, 24);
    hero->setSpacing(28);
    auto *intro = new QVBoxLayout;
    intro->setSpacing(12);
    auto *title = ui::paragraph(QStringLiteral("От цены акции к результату стратегии"));
    title->setObjectName("welcomeTitle");
    intro->addWidget(title);
    auto *description = ui::paragraph(QStringLiteral(
        "Начните с готового примера. В нём есть вымышленные цены и правила, чтобы сразу попробовать анализ и увидеть результат."));
    description->setObjectName("welcomeDescription");
    intro->addWidget(description);
    auto *demo = new QPushButton(QStringLiteral("Начать с ДЕМО"));
    demo->setObjectName("startDemo");
    demo->setProperty("primary", true);
    intro->addWidget(demo, 0, Qt::AlignLeft);
    connect(demo, &QPushButton::clicked, this, [this] {
        tabs_->setCurrentIndex(7);
        lab_->show_demo();
    });
    hero->addLayout(intro, 3);

    auto *guide = new QFrame;
    guide->setObjectName("welcomeGuide");
    auto *steps = new QVBoxLayout(guide);
    steps->setContentsMargins(16, 14, 16, 14);
    steps->setSpacing(7);
    auto *guide_title = ui::paragraph(QStringLiteral("Что вы сделаете"));
    guide_title->setObjectName("cardTitle");
    steps->addWidget(guide_title);
    steps->addWidget(ui::paragraph(QStringLiteral("1. Откроете историю цен")));
    steps->addWidget(ui::paragraph(QStringLiteral("2. Проверите правила на этой истории")));
    steps->addWidget(ui::paragraph(QStringLiteral("3. Посмотрите доходность и риск")));
    hero->addWidget(guide, 2);
    layout->addWidget(welcome);

    auto *cards = new QGridLayout;
    cards->setSpacing(12);
    cards->setColumnStretch(0, 1);
    cards->setColumnStretch(1, 1);
    layout->addLayout(cards);
    auto card = [&](int row, int column, const QString &heading, const QString &description,
                    const QString &action, const char *name, auto callback) {
        auto *box = new QFrame;
        box->setObjectName("startCard");
        auto *content = new QVBoxLayout(box);
        content->setContentsMargins(16, 12, 16, 12);
        content->setSpacing(8);
        auto *title = ui::paragraph(heading);
        title->setObjectName("cardTitle");
        content->addWidget(title);
        auto *details = ui::paragraph(description);
        details->setObjectName("cardDescription");
        content->addWidget(details, 1);
        auto *button = new QPushButton(action);
        button->setObjectName(name);
        content->addWidget(button, 0, Qt::AlignLeft);
        connect(button, &QPushButton::clicked, this, callback);
        cards->addWidget(box, row, column);
    };
    card(0, 0, QStringLiteral("Котировки"), QStringLiteral("Таблица и график цен MOEX с задержкой."),
         QStringLiteral("Открыть котировки"), "startQuotes", [this] {
             tabs_->setCurrentIndex(0);
             start_->setFocus(Qt::OtherFocusReason);
         });
    card(0, 1, QStringLiteral("Исследование"), QStringLiteral("Загрузите историю акций и проверьте стратегию."),
         QStringLiteral("Выбрать акции и период"), "startResearch", [this] {
             tabs_->setCurrentIndex(7);
             lab_->navigate_to(6);
         });
    card(1, 0, QStringLiteral("Учебные задания"), QStringLiteral("Разберите комиссии, риск и подбор параметров."),
         QStringLiteral("Открыть задания"), "startLessons", [this] { tabs_->setCurrentIndex(9); });
    card(1, 1, QStringLiteral("Портфель"), QStringLiteral("Проверьте несколько акций с общим виртуальным капиталом."),
         QStringLiteral("Открыть портфель"), "startPortfolio", [this] { tabs_->setCurrentIndex(10); });

    auto *note = ui::paragraph(QStringLiteral(
        "ДЕМО работает без интернета. Публичные данные MOEX доступны без аккаунта. Сделки в приложении виртуальные."));
    note->setObjectName("cardDescription");
    layout->addWidget(note);
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
    workspace_context_->setVisible(index != 8);
}
} // namespace live
