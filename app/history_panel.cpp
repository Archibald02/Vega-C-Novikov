#include "history_panel.hpp"
#include "quote_parser.hpp"
#include <QHBoxLayout>
#include <QVBoxLayout>
namespace live {
HistoryPanel::HistoryPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *controls = new QHBoxLayout;
    auto *instrument = new QHBoxLayout;
    instrument->addWidget(new QLabel(QStringLiteral("Акция MOEX")));
    symbol_edit_ = new QLineEdit("SBER");
    symbol_edit_->setMaxLength(32);
    symbol_edit_->setAccessibleName(QStringLiteral("Тикер для загрузки свечей"));
    symbol_edit_->setPlaceholderText(QStringLiteral("Например: SBER"));
    instrument->addWidget(symbol_edit_);
    layout->addLayout(instrument);
    period_ = new QComboBox;
    period_->addItems({QStringLiteral("Дневные • 90 дней"), QStringLiteral("Дневные • 365 дней"),
                       QStringLiteral("Часовые • 7 дней")});
    load_ = new QPushButton(QStringLiteral("Загрузить свечи без регистрации"));
    auto *refresh = new QPushButton(QStringLiteral("Обновить"));
    auto *cancel = new QPushButton(QStringLiteral("Отменить"));
    controls->addWidget(period_);
    controls->addWidget(load_);
    controls->addWidget(refresh);
    controls->addWidget(cancel);
    controls->addStretch();
    layout->addLayout(controls);
    status_ =
        new QLabel(QStringLiteral("Выбран SBER. Запрос только по нажатию кнопки; ключ и пароль не нужны."));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    summary_ = new QLabel;
    summary_->setTextFormat(Qt::PlainText);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    chart_ = new VisualChart;
    chart_->clear(QStringLiteral("Свечи MOEX • введите тикер и нажмите загрузку"));
    layout->addWidget(chart_, 1);
    auto *note = new QLabel(
        QStringLiteral("Официальные OHLCV • данные задержаны • последняя свеча может быть незавершённой • "
                       "жёлтая линия SMA 20 свечей\nКолесо — последние 20–500 свечей; курсор — OHLCV. Объём "
                       "в единицах ответа MOEX. Кэш только в RAM: до 8 серий, 5 минут."));
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(symbol_edit_, &QLineEdit::textEdited, this, [this] {
        loader_.cancel();
        load_->setEnabled(true);
        summary_->clear();
        chart_->clear(QStringLiteral("Акция изменена — нажмите загрузку свечей"));
        status_->setText(QStringLiteral("Выбрана другая акция. Новая история появится после загрузки."));
    });
    connect(load_, &QPushButton::clicked, this, [this] { load(false); });
    connect(refresh, &QPushButton::clicked, this, [this] { load(true); });
    connect(cancel, &QPushButton::clicked, this, [this] {
        loader_.cancel();
        load_->setEnabled(true);
        status_->setText(QStringLiteral("Запрос отменён."));
    });
    connect(period_, &QComboBox::currentIndexChanged, this, [this] {
        loader_.cancel();
        load_->setEnabled(true);
        chart_->clear(QStringLiteral("Период изменён — нажмите загрузку"));
        summary_->clear();
    });
    connect(&loader_, &MarketHistory::loaded, this,
            [this](const QString &symbol, int preset, const QVector<Candle> &bars, bool cached) {
                if (symbol != symbol_ || preset != period_->currentIndex())
                    return;
                load_->setEnabled(true);
                status_->setText(QStringLiteral("%1 • MOEX ISS • %2 свечей • %3 • с задержкой")
                                     .arg(symbol)
                                     .arg(bars.size())
                                     .arg(cached ? QStringLiteral("кэш RAM") : QStringLiteral("ответ API")));
                const auto &first = bars.front();
                const auto &last = bars.back();
                const double change = (last.close / first.open - 1) * 100;
                summary_->setText(QStringLiteral("Начало: %1 ₽ → последняя: %2 ₽ • изменение: %3% • это цены "
                                                 "без поправок на дивиденды и сплиты")
                                      .arg(first.open, 0, 'f', 2)
                                      .arg(last.close, 0, 'f', 2)
                                      .arg(change, 0, 'f', 2));
                chart_->set_candles(bars, symbol + QStringLiteral(" • свечи и объём"),
                                    period_->currentText() +
                                        QStringLiteral(" • MOEX / TQBR • задержанные данные"));
            });
    connect(&loader_, &MarketHistory::failed, this, [this](const QString &message) {
        load_->setEnabled(true);
        status_->setText(message);
    });
}
void HistoryPanel::set_symbol(const QString &symbol) {
    if (symbol.isEmpty() || symbol == symbol_)
        return;
    symbol_ = symbol;
    symbol_edit_->setText(symbol);
    loader_.cancel();
    load_->setEnabled(true);
    summary_->clear();
    chart_->clear(symbol + QStringLiteral(" • нажмите загрузку свечей"));
    status_->setText(symbol + QStringLiteral(" • без регистрации; история ещё не загружена"));
}
void HistoryPanel::load(bool force) {
    try {
        const auto list = parse_symbols(symbol_edit_->text());
        if (list.size() != 1)
            throw std::invalid_argument("one symbol");
        symbol_ = list.front();
        symbol_edit_->setText(symbol_);
    } catch (...) {
        status_->setText(QStringLiteral("Введите один тикер, например SBER или GAZP."));
        return;
    }
    load_->setEnabled(false);
    chart_->clear(QStringLiteral("Загрузка свечей…"));
    summary_->clear();
    status_->setText(symbol_ + QStringLiteral(" • загрузка из открытого API MOEX…"));
    loader_.load(symbol_, period_->currentIndex(), force);
}
void HistoryPanel::clear_private_state() {
    loader_.clear();
    chart_->clear();
    summary_->clear();
    status_->setText(QStringLiteral("Кэш истории очищен."));
    load_->setEnabled(true);
}
void HistoryPanel::show_test_bars(const QVector<Candle> &bars) {
    chart_->set_candles(bars, QStringLiteral("SBER • ДЕМО свечи и объём"),
                        QStringLiteral("Синтетические OHLCV для проверки интерфейса"));
    status_->setText(QStringLiteral("ДЕМО • без сетевых запросов"));
}
} // namespace live
