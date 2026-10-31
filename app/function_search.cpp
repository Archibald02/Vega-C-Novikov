#include "function_search.hpp"
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
namespace live {
const QVector<FunctionDestination> &function_destinations() {
    static const QVector<FunctionDestination> entries{
        {QStringLiteral("Начало"), QStringLiteral("Выбор задачи и первые шаги"), "home start", 8},
        {QStringLiteral("Котировки и график"), QStringLiteral("Поток LAST; индикаторы по обновлениям цены"),
         "цены last quotes", 0},
        {QStringLiteral("Свечи MOEX"), QStringLiteral("Просмотр биржевых OHLCV отдельно от текущих цен"),
         "история ohlcv", 5},
        {QStringLiteral("Сравнение акций"), QStringLiteral("Изменения цен выбранных акций"), "compare", 6},
        {QStringLiteral("Учебный прогноз"),
         QStringLiteral("Эксперимент по обновлениям цены; не торговый сигнал"), "forecast прогнозы", 1},
        {QStringLiteral("Размер позиции и пороги"), QStringLiteral("Калькулятор риска и уведомления по цене"),
         "лот капитал риск", 2},
        {QStringLiteral("Данные исследования"), QStringLiteral("Закрытые свечи, даты и выбранные акции"),
         "загрузка синхронизация сессии временные зоны", 7, 6},
        {QStringLiteral("Качество данных"),
         QStringLiteral("Пропуски, дубли, устаревание и отклонённые ответы"), "quality разрывы", 7, 4},
        {QStringLiteral("Индикаторы по свечам"),
         QStringLiteral("SMA, EMA, RSI, MACD, Bollinger, ATR/NATR и Z-score"), "волатильность zscore", 7, 0,
         true},
        {QStringLiteral("Доходности и распределение"),
         QStringLiteral("Простые и логарифмические доходности, гистограмма и квантили"), "returns статистика",
         7, 0},
        {QStringLiteral("Корреляции"), QStringLiteral("Матрица согласованных доходностей"),
         "correlation взаимосвязь", 7, 0, true},
        {QStringLiteral("ACF и PACF"), QStringLiteral("Автокорреляция и частная автокорреляция"),
         "лаги зависимости", 7, 0, true},
        {QStringLiteral("ADF и KPSS"), QStringLiteral("Тесты стационарности и ограничения гипотез"),
         "stationarity стационарность", 7, 0, true},
        {QStringLiteral("Конструктор стратегии"),
         QStringLiteral("Правила входа/выхода, комиссии, спред и исполнение"),
         "алгоритм backtest бэктест проскальзывание подтверждение пауза "
         "удержание задержка формула скобки "
         "вложенные частичный выход старший интервал H1 H4",
         7, 1},
        {QStringLiteral("Результат бэктеста"),
         QStringLiteral("Капитал, просадка, сделки, Sharpe/Sortino/Calmar и базы"),
         "отчет отчёт метрики benchmark", 7, 2},
        {QStringLiteral("Устойчивость и walk-forward"),
         QStringLiteral("Проверка параметров и отдельного периода"), "переобучение robustness holdout", 7, 5},
        {QStringLiteral("Эксперименты и проекты"),
         QStringLiteral("Гипотезы, сравнение запусков и явный экспорт отчёта"), "сохранение qpe qps снимок",
         7, 7},
        {QStringLiteral("Виртуальный счёт стратегии"),
         QStringLiteral("Paper trading, журнал решений и лимиты риска"), "аварийная остановка сделки", 7, 3},
        {QStringLiteral("Портфель"),
         QStringLiteral("Общий виртуальный капитал, ребалансировка, тренажёр и "
                        "состояние счёта"),
         "qpp qpa paper replay несколько акций", 10},
        {QStringLiteral("Учебные задания"),
         QStringLiteral("Автономные примеры комиссий, гэпа и переобучения"), "уроки демо обучение", 9},
        {QStringLiteral("Интерфейс и производительность"),
         QStringLiteral("Размер текста, колонки, частота отрисовки и предел истории"),
         "настройки cpu ram память масштаб", 3},
        {QStringLiteral("API, приватность и помощь"),
         QStringLiteral("Официальные сайты источников и очистка локальных данных"),
         "пароль ключ регистрация подключение", 4}};
    return entries;
}
QVector<int> find_functions(const QString &query) {
    const auto words = query.left(160).simplified().toCaseFolded().split(' ', Qt::SkipEmptyParts);
    QVector<int> matches;
    const auto &entries = function_destinations();
    for (int i = 0; i < entries.size(); ++i) {
        const auto corpus =
            (entries[i].title + ' ' + entries[i].detail + ' ' + entries[i].keywords).toCaseFolded();
        if (std::all_of(words.begin(), words.end(),
                        [&](const QString &word) { return corpus.contains(word); }))
            matches.push_back(i);
    }
    return matches;
}
FunctionSearch::FunctionSearch(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Поиск функций • Ctrl+K"));
    setModal(false);
    resize(660, 480);
    auto *layout = new QVBoxLayout(this);
    query_ = new QLineEdit;
    query_->setMaxLength(160);
    query_->setPlaceholderText(QStringLiteral("Например: корреляции, ATR, комиссии, портфель…"));
    query_->setAccessibleName(QStringLiteral("Поиск функций"));
    query_->installEventFilter(this);
    layout->addWidget(query_);
    count_ = new QLabel;
    count_->setTextFormat(Qt::PlainText);
    layout->addWidget(count_);
    results_ = new QListWidget;
    results_->setAccessibleName(QStringLiteral("Результаты поиска функций"));
    results_->setWordWrap(true);
    layout->addWidget(results_, 1);
    auto *note =
        new QLabel(QStringLiteral("↑/↓ — выбор • Enter — открыть раздел • Esc — закрыть\nПереход не нажимает "
                                  "«Рассчитать» и не подключает источник. Запрос не сохраняется."));
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(query_, &QLineEdit::textChanged, this, &FunctionSearch::filter);
    connect(results_, &QListWidget::itemActivated, this, [this] {
        if (selected_destination() >= 0)
            accept();
    });
    filter({});
    query_->setFocus();
}
void FunctionSearch::filter(const QString &query) {
    results_->clear();
    for (int index : find_functions(query)) {
        const auto &entry = function_destinations()[index];
        auto *item = new QListWidgetItem(entry.title + '\n' + entry.detail, results_);
        item->setData(Qt::UserRole, index);
    }
    count_->setText(results_->count()
                        ? QStringLiteral("Найдено разделов: %1").arg(results_->count())
                        : QStringLiteral("Ничего не найдено. Попробуйте другое слово или сокращение."));
    if (results_->count())
        results_->setCurrentRow(0);
}
int FunctionSearch::selected_destination() const {
    return results_->currentItem() ? results_->currentItem()->data(Qt::UserRole).toInt() : -1;
}
bool FunctionSearch::eventFilter(QObject *object, QEvent *event) {
    if (object == query_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (selected_destination() >= 0)
                accept();
            return true;
        }
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            if (results_->count())
                results_->setCurrentRow(
                    std::clamp(results_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1), 0,
                               static_cast<int>(results_->count()) - 1));
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}
} // namespace live
