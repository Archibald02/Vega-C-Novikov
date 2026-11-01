#include "quote_parser.hpp"
#include "research_panel.hpp"
#include "ui_helpers.hpp"
#include <QEventLoop>
#include <QCoreApplication>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QToolButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTcpServer>
#include <QTcpSocket>
namespace live {
namespace {
QString time_text(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString("dd.MM.yyyy HH:mm 'UTC'");
}
QLabel *paragraph(const QString &text) {
    auto *l = new QLabel(text);
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(true);
    return l;
}
} // namespace
void ResearchPanel::begin_history_load(bool force) {
    cancel_work();
    force_history_ = force;
    reuse_history_ = false;
    data_.clear();
    history_origins_.clear();
    result_current_ = false;
    diagnostics_.clear();
    selected_->clear();
    try {
        requested_ = parse_symbols(symbols_->text());
        if (requested_.size() > 6)
            throw std::invalid_argument("six");
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
    refresh_history_manager();
    update_journey();
    load_next();
}
void ResearchPanel::prepare_history_set(const QString &symbol, QDate from, QDate till, int interval) {
    cancel_work();
    const QSignalBlocker a(symbols_), b(period_), c(custom_dates_), d(from_), e(till_);
    symbols_->setText(symbol);
    period_->setCurrentIndex(interval == 60 ? 2 : 1);
    custom_dates_->setChecked(true);
    from_->setDate(from);
    till_->setDate(till);
    for (auto *w : {from_, till_}) {
        w->setEnabled(true);
        for (auto *form : data_page_->findChildren<QFormLayout *>()) {
            int row = -1;
            QFormLayout::ItemRole role;
            form->getWidgetPosition(w, &row, &role);
            if (row >= 0)
                form->setRowVisible(row, true);
        }
    }
    data_.clear();
    history_origins_.clear();
    diagnostics_.clear();
    selected_->clear();
    result_current_ = false;
    synthetic_ = false;
    local_project_ = false;
    source_dirty_ = false;
    requested_ = QStringList{symbol};
    request_index_ = 0;
    loading_ = true;
    force_history_ = false;
    reuse_history_ = true;
    load_->setEnabled(false);
    period_->setEnabled(false);
    update_journey();
}
void ResearchPanel::setup_history_manager() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(paragraph(
        QStringLiteral("До 8 наборов ×5000 свечей в RAM и одна контрольная точка загрузки (30 минут). "
                       "На диск ничего не пишется. Повторный выбор из RAM не проверяет сеть и может "
                       "содержать старые данные. "
                       "Редакция описывает принятые свечи, не всю историю биржи. Старые полные редакции "
                       "здесь не архивируются.")));
    history_sets_ = new QComboBox;
    layout->addWidget(history_sets_);
    history_details_ = paragraph({});
    layout->addWidget(history_details_);
    auto *row = new QHBoxLayout;
    history_restore_ = new QPushButton(QStringLiteral("Открыть набор из RAM"));
    history_refresh_ = new QPushButton(QStringLiteral("Перепроверить весь период"));
    auto *clear = new QPushButton(QStringLiteral("Очистить RAM-кэш и точку"));
    row->addWidget(history_restore_);
    row->addWidget(history_refresh_);
    layout->addLayout(row);
    layout->addWidget(clear);
    history_checkpoint_ = paragraph({});
    layout->addWidget(history_checkpoint_);
    history_resume_ = new QPushButton(QStringLiteral("Продолжить незавершённый набор"));
    auto *discard = new QPushButton(QStringLiteral("Удалить контрольную точку"));
    layout->addWidget(history_resume_);
    layout->addWidget(discard);
    layout->addWidget(paragraph(
        QStringLiteral("Возобновление проверяет последнюю принятую страницу. При её изменении весь период "
                       "загружается заново. "
                       "Более ранние страницы при продолжении не перепроверяются. Ответы ISS не являются "
                       "атомарным снимком биржи; для полной перепроверки используйте отдельную кнопку. "
                       "Открытие/продолжение заменяет исследовательский набор одной акцией; готовые наборы "
                       "остаются в RAM-кэше. "
                       "Очистка кэша не удаляет уже показанные результаты; для этого есть общая очистка "
                       "приватных данных.")));
    auto *fold=ui::fold(QStringLiteral("Менеджер истории • RAM и возобновление"),page);
    fold->findChild<QToolButton*>()->setObjectName("historyManagerToggle");
    data_layout_->addWidget(fold);
    connect(&loader_, &MarketHistory::history_changed, this, &ResearchPanel::refresh_history_manager);
    connect(&loader_, &MarketHistory::state_message, this, [this](const QString &message) {
        if (loading_)
            status_->setText(message);
    });
    connect(history_sets_, &QComboBox::currentIndexChanged, this, [this] { refresh_history_manager(); });
    connect(history_restore_, &QPushButton::clicked, this, [this] {
        const auto key = history_sets_->currentData().toString();
        for (const auto &info : loader_.datasets())
            if (info.key == key) {
                prepare_history_set(info.symbol, info.from, info.till, info.interval);
                if (!loader_.restore(key) && loading_) {
                    loading_ = false;
                    load_->setEnabled(true);
                    period_->setEnabled(true);
                }
                refresh_history_manager();
                return;
            }
    });
    connect(history_refresh_, &QPushButton::clicked, this, [this] {
        const auto key = history_sets_->currentData().toString();
        for (const auto &info : loader_.datasets())
            if (info.key == key) {
                prepare_history_set(info.symbol, info.from, info.till, info.interval);
                begin_history_load(true);
                return;
            }
    });
    connect(history_resume_, &QPushButton::clicked, this, [this] {
        const auto c = loader_.checkpoint();
        if (!c.available) {
            refresh_history_manager();
            return;
        }
        prepare_history_set(c.symbol, c.from, c.till, c.interval);
        if (!loader_.resume() && loading_) {
            loading_ = false;
            load_->setEnabled(true);
            period_->setEnabled(true);
        }
        refresh_history_manager();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        cancel_work();
        loader_.clear();
        refresh_history_manager();
    });
    connect(discard, &QPushButton::clicked, this, [this] {
        cancel_work();
        loader_.discard_checkpoint();
        refresh_history_manager();
    });
    refresh_history_manager();
}
void ResearchPanel::refresh_history_manager() {
    if (!history_sets_)
        return;
    const auto key = history_sets_->currentData().toString();
    const QSignalBlocker blocker(history_sets_);
    history_sets_->clear();
    const auto sets = loader_.datasets();
    for (const auto &info : sets)
        history_sets_->addItem(QStringLiteral("%1 • %2—%3 • %4 • %5 свечей • ред. %6")
                                   .arg(info.symbol, info.from.toString(Qt::ISODate),
                                        info.till.toString(Qt::ISODate),
                                        info.interval == 60 ? QStringLiteral("H1") : QStringLiteral("D1"))
                                   .arg(info.bars)
                                   .arg(info.revision),
                               info.key);
    if (history_sets_->findData(key) >= 0)
        history_sets_->setCurrentIndex(history_sets_->findData(key));
    const int selected = history_sets_->currentIndex();
    if (selected >= 0) {
        const auto &info = sets[selected];
        history_details_->setText(
            QStringLiteral("Собрано %1 → %2. SHA-256 %3\n%4. %5%6")
                .arg(time_text(info.started_ms), time_text(info.fetched_ms), info.sha256,
                     info.comparable ? QStringLiteral("К предыдущему набору: +%1 / изменено %2 / удалено %3")
                                           .arg(info.added)
                                           .arg(info.changed)
                                           .arg(info.removed)
                                     : QStringLiteral("Предыдущего набора для сравнения нет"),
                     info.resumed
                         ? QStringLiteral("Есть возобновлённые страницы; ранняя часть не перепроверялась. ")
                         : QString(),
                     info.limited ? QStringLiteral("Лимит 5000: период может быть неполным.") : QString()));
    } else
        history_details_->setText(QStringLiteral("Готовых наборов в RAM нет."));
    const auto c = loader_.checkpoint();
    history_checkpoint_->setText(
        c.available
            ? QStringLiteral("Пауза: %1 • %2—%3 • %4 свечей; следующий start=%5. Сохранено %6.")
                  .arg(c.symbol, c.from.toString(Qt::ISODate), c.till.toString(Qt::ISODate))
                  .arg(c.bars)
                  .arg(c.next_offset)
                  .arg(time_text(c.saved_ms))
            : QStringLiteral(
                  "Действующей контрольной точки нет. Она появляется после полной принятой страницы."));
    const bool idle = !loading_ && !loader_.busy() && (!worker_ || !worker_->isRunning());
    history_restore_->setEnabled(idle && selected >= 0);
    history_refresh_->setEnabled(idle && selected >= 0);
    history_resume_->setEnabled(idle && c.available);
}
bool ResearchPanel::history_manager_test() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return false;
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            auto bytes = std::make_shared<QByteArray>();
            auto done = std::make_shared<bool>(false);
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes, done] {
                bytes->append(socket->readAll());
                if (*done || !bytes->contains("\r\n\r\n"))
                    return;
                *done = true;
                ++requests;
                QJsonArray rows;
                for (int i = 0; i < 40; ++i)
                    rows.append(QJsonArray{QDate(2024, 1, 1).addDays(i).toString("yyyy-MM-dd") + " 00:00:00",
                                           100, 101, 99, 100, 10});
                const auto body =
                    QJsonDocument(
                        QJsonObject{{"candles", QJsonObject{{"columns", QJsonArray{"begin", "open", "high",
                                                                                   "low", "close", "volume"}},
                                                            {"data", rows}}}})
                        .toJson();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    ResearchPanel temporary(nullptr, QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
    QEventLoop loop;
    connect(&temporary.loader_, &MarketHistory::loaded, &loop, &QEventLoop::quit);
    temporary.loader_.load_range("SBER", QDate(2024, 1, 1), QDate(2024, 3, 1), 24);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (temporary.loader_.busy() || requests != 1 || temporary.history_sets_->count() != 1 ||
        !temporary.history_restore_->isEnabled())
        return false;
    temporary.history_restore_->click();
    if (requests != 1 || temporary.data_.size() != 1 || temporary.selected_bars().size() != 40 ||
        !temporary.history_origins_.value("SBER").contains("RAM"))
        return false;
    temporary.clear_private_state();
    const bool cleared=temporary.loader_.datasets().isEmpty() && !temporary.loader_.checkpoint().available &&
           temporary.history_sets_->count() == 0 && temporary.history_origins_.isEmpty();
    if(!cleared)return false;
    tabs_->setCurrentIndex(6);
    if(auto *button=findChild<QToolButton*>("historyManagerToggle"))button->setChecked(true);
    if(auto *scroll=qobject_cast<QScrollArea*>(tabs_->widget(6))) {
        scroll->widget()->layout()->activate();scroll->ensureWidgetVisible(history_details_,0,0);
    }
    return true;
}
} // namespace live
