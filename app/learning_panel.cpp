#include "learning_panel.hpp"
#include "ui_helpers.hpp"
#include <QEventLoop>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <exception>
namespace live {
namespace {
QString n(double x) { return QString::number(x, 'f', 2); }
PlotLine curve(const quant::SimResult &r, const QString &name, const QColor &color) {
    PlotLine line{name, color, {}, false};
    for (std::size_t i = 0; i < r.equity.size(); ++i)
        line.points.push_back({static_cast<double>(i), 100 * r.equity[i] / r.equity.front()});
    return line;
}
} // namespace
LearningPanel::LearningPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(ui::paragraph(QStringLiteral("Учебные задания • ДЕМО • только синтетические данные")));
    layout->addWidget(ui::paragraph(QStringLiteral(
        "Предположите результат, измените один параметр и проверьте пример. Для заданий не нужны интернет, "
        "аккаунт и ваши котировки. Расчёты выполняет тот же виртуальный движок, что и лаборатория.")));
    lesson_ = new QComboBox;
    lesson_->addItems({QStringLiteral("1. Комиссии при неизменной цене"),
                       QStringLiteral("2. Стоп и ценовой гэп"),
                       QStringLiteral("3. Подбор параметров и следующий участок")});
    lesson_->setAccessibleName(QStringLiteral("Учебное задание"));
    layout->addWidget(lesson_);
    question_ = ui::paragraph({});
    layout->addWidget(question_);
    auto *form = new QFormLayout;
    input_label_ = new QLabel;
    input_ = new QDoubleSpinBox;
    input_->setKeyboardTracking(false);
    form->addRow(input_label_, input_);
    answer_ = new QComboBox;
    form->addRow(QStringLiteral("Ваше предположение"), answer_);
    layout->addLayout(form);
    auto *actions = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Проверить на синтетическом примере"));
    cancel_ = new QPushButton(QStringLiteral("Отменить"));
    cancel_->setEnabled(false);
    actions->addWidget(run_);
    actions->addWidget(cancel_);
    actions->addStretch();
    layout->addLayout(actions);
    summary_ = ui::paragraph(QStringLiteral("Выберите предположение и нажмите «Проверить»."));
    summary_->setTextFormat(Qt::PlainText);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary_);
    chart_ = new VisualChart;
    chart_->setToolTip(VisualChart::interaction_help());
    layout->addWidget(chart_);
    explanation_ = ui::paragraph({});
    explanation_->setTextFormat(Qt::PlainText);
    layout->addWidget(explanation_);
    layout->addStretch();
    worker_ = new QFutureWatcher<LessonOutcome>(this);
    connect(lesson_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { change_lesson(); });
    connect(input_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { invalidate(); });
    connect(answer_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { invalidate(); });
    connect(run_, &QPushButton::clicked, this, [this] { run(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (cancelled_)
            cancelled_->store(true);
        summary_->setText(QStringLiteral("Отменено. Результат задачи не применяется."));
    });
    connect(worker_, &QFutureWatcher<LessonOutcome>::finished, this, [this] {
        busy(false);
        if (cancelled_ && !cancelled_->load())
            show_result(worker_->result());
    });
    change_lesson();
    ui::prepare_controls(this);
}
LearningPanel::~LearningPanel() {
    if (cancelled_)
        cancelled_->store(true);
}
void LearningPanel::busy(bool on) {
    lesson_->setEnabled(!on);
    input_->setEnabled(!on);
    answer_->setEnabled(!on);
    run_->setEnabled(!on && answer_->currentIndex() > 0);
    cancel_->setEnabled(on);
}
void LearningPanel::invalidate() {
    if (worker_->isRunning())
        return;
    chart_->clear(QStringLiteral("Синтетический пример ещё не рассчитан."));
    explanation_->clear();
    summary_->setText(QStringLiteral("Параметры изменены. Выберите предположение и нажмите «Проверить»."));
    run_->setEnabled(answer_->currentIndex() > 0);
}
void LearningPanel::change_lesson() {
    answer_->clear();
    answer_->addItem(QStringLiteral("Выберите предположение…"));
    if (lesson_->currentIndex() == 0) {
        input_label_->setText(QStringLiteral("Комиссия за одно исполнение, %"));
        input_->setDecimals(2);
        input_->setRange(.01, 1);
        input_->setSingleStep(.05);
        input_->setValue(.1);
        question_->setText(QStringLiteral("Цена всё время 100 ₽. Правила поочерёдно покупают и продают. Что "
                                          "произойдёт с капиталом при положительной комиссии?"));
        answer_->addItems({QStringLiteral("Капитал уменьшится"), QStringLiteral("Капитал не изменится"),
                           QStringLiteral("Капитал обязательно вырастет")});
    } else if (lesson_->currentIndex() == 1) {
        input_label_->setText(QStringLiteral("Бюджет риска сделки, % капитала"));
        input_->setDecimals(1);
        input_->setRange(.1, 5);
        input_->setSingleStep(.1);
        input_->setValue(1);
        question_->setText(
            QStringLiteral("Вход 100 ₽, стоп 98 ₽. Во втором сценарии следующая свеча открывается сразу по "
                           "90 ₽. Ограничит ли стоп фактический убыток заданным бюджетом?"));
        answer_->addItems({QStringLiteral("Да, стоп гарантирует бюджет"),
                           QStringLiteral("Нет, гэп может увеличить убыток"),
                           QStringLiteral("После гэпа сделка исчезнет")});
    } else {
        input_label_->setText(QStringLiteral("Число сравниваемых пар SMA"));
        input_->setDecimals(0);
        input_->setRange(1, 16);
        input_->setSingleStep(1);
        input_->setValue(8);
        question_->setText(
            QStringLiteral("Выбираем лучшую пару SMA по первым 200 свечам, затем фиксируем её для следующих "
                           "200. Гарантирует ли лучший обучающий результат успех на следующем участке?"));
        answer_->addItems({QStringLiteral("Да, лучший результат гарантирован"),
                           QStringLiteral("Чем больше вариантов, тем надёжнее гарантия"),
                           QStringLiteral("Нет, нужна отдельная проверка")});
    }
    input_->setAccessibleName(input_label_->text());
    answer_->setCurrentIndex(0);
    invalidate();
}
void LearningPanel::run() {
    if (worker_->isRunning() || answer_->currentIndex() < 1)
        return;
    LessonOutcome request;
    request.kind = lesson_->currentIndex();
    request.input = input_->value();
    request.answer = answer_->currentIndex();
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto flag = cancelled_;
    busy(true);
    summary_->setText(QStringLiteral("Считаем синтетический пример в фоне…"));
    worker_->setFuture(QtConcurrent::run([request, flag]() mutable {
        try {
            if (flag->load())
                return request;
            if (request.kind == 0)
                request.data = quant::commission_lesson(request.input / 100);
            else if (request.kind == 1)
                request.data = quant::gap_lesson(request.input / 100);
            else
                request.data =
                    quant::selection_lesson(quant::learning_bars(), static_cast<int>(request.input));
        } catch (const std::exception &) {
            request.error = QStringLiteral("Пример не рассчитан: проверьте параметры.");
        }
        return request;
    }));
}
void LearningPanel::show_result(const LessonOutcome &r) {
    if (!r.error.isEmpty()) {
        summary_->setText(r.error);
        return;
    }
    const auto &a = r.data.first, &b = r.data.second;
    const QString verdict = r.answer == r.kind + 1 ? QStringLiteral("Предположение верное.")
                                                   : QStringLiteral("Предположение стоит пересмотреть.");
    QString first, second, detail, summary;
    if (r.kind == 0) {
        first = QStringLiteral("Без комиссии");
        second = QStringLiteral("Комиссия %1% за исполнение").arg(n(r.input));
        summary = QStringLiteral(
                      "Без комиссии: %1 ₽. С комиссией: %2 ₽. Всего комиссий: %3 ₽, завершённых сделок: %4.")
                      .arg(n(a.equity.back()), n(b.equity.back()), n(b.metrics.fees))
                      .arg(b.metrics.closed_trades);
        detail = QStringLiteral(
            "Цена синтетической акции не менялась, поэтому весь убыток этого примера объясняется комиссиями. "
            "Комиссия списывается и на входе, и на выходе. Объём покупки округляется до целой акции и "
            "учитывает доступные деньги. Спред, проскальзывание и фиксированный сбор здесь равны нулю. В "
            "реальных данных результат зависит ещё и от изменения цены; пример не утверждает, что редкая "
            "торговля всегда прибыльнее.");
    } else if (r.kind == 1) {
        first = QStringLiteral("Стоп по 98 ₽");
        second = QStringLiteral("Гэп: выход по 90 ₽");
        summary = QStringLiteral(
                      "Бюджет: %1 ₽. Акций при входе: %2. Убыток при касании стопа: %3 ₽; при гэпе: %4 ₽.")
                      .arg(n(100000 * r.input / 100))
                      .arg(a.journal.front().quantity)
                      .arg(n(100000 - a.equity.back()), n(100000 - b.equity.back()));
        detail = QStringLiteral(
            "Стоп является условием выхода, а не гарантией цены. При гэпе модель закрывает позицию по "
            "доступному открытию. Размер позиции ограничен одновременно бюджетом риска и капиталом; при "
            "большом бюджете ограничителем становится капитал. Комиссии и спред отключены для наглядности. "
            "Реальное исполнение может дополнительно ухудшиться из-за ликвидности и проскальзывания.");
    } else {
        first = QStringLiteral("Обучение выбранной SMA");
        second = QStringLiteral("Следующий участок");
        summary = QStringLiteral("Из %1 пар выбрана SMA %2/%3 только на обучении. Доходность обучения: %4%; "
                                 "следующего участка: %5%.")
                      .arg(r.data.train_scores.size())
                      .arg(r.data.fast)
                      .arg(r.data.slow)
                      .arg(n(a.metrics.total_return * 100), n(b.metrics.total_return * 100));
        detail = QStringLiteral(
            "SMA — средняя последних N закрытых цен. Варианты: fast 2/4/8/16, внутри каждого — slow "
            "20/30/40/60; используются первые N пар, при равенстве сохраняется первая. "
            "Оба участка начинают с отдельных 100 000 ₽, покупки без плеча на доступный капитал. Комиссия — "
            "0,05% за исполнение; спред и "
            "проскальзывание — ноль. Прошлые свечи прогревают индикаторы следующего участка, но будущие не "
            "участвуют в выборе SMA. Добавление кандидатов не ухудшает лучший результат обучения, однако не "
            "гарантирует улучшения проверки. В синтетическом примере также меняется режим цены: один разрыв "
            "результатов сам по себе не доказывает переобучение. Повторный просмотр проверочного участка "
            "делает его частью исследования, а не независимым финальным тестом.");
    }
    summary_->setText(QStringLiteral("ДЕМО • %1\n%2").arg(verdict, summary));
    explanation_->setText(detail);
    chart_->set_lines(
        {curve(a, first, QColor("#39d2b0")), curve(b, second, QColor("#efc571"))},
        QStringLiteral("Виртуальный капитал, начало = 100"),
        r.kind == 2
            ? QStringLiteral("Ось X — номер свечи отдельного участка. Обучение и проверка имеют разные цены "
                             "и даты. Все данные синтетические.")
            : QStringLiteral(
                  "Ось X — номер свечи, точка 0 — начальный капитал. Все цены и сделки синтетические."));
}
bool LearningPanel::self_test() {
    auto wait = [this] {
        QEventLoop loop;
        auto c = connect(worker_, &QFutureWatcher<LessonOutcome>::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(c);
        return !worker_->isRunning();
    };
    for (int i = 0; i < 3; ++i) {
        lesson_->setCurrentIndex(i);
        answer_->setCurrentIndex(i + 1);
        run_->click();
        if (!wait() || !summary_->text().contains(QStringLiteral("Предположение верное")) ||
            worker_->result().data.first.equity.empty())
            return false;
    }
    input_->setValue(16);
    if (!explanation_->text().isEmpty())
        return false;
    answer_->setCurrentIndex(3);
    run_->click();
    cancel_->click();
    if (!wait() || !summary_->text().startsWith(QStringLiteral("Отменено")))
        return false;
    lesson_->setCurrentIndex(1);
    answer_->setCurrentIndex(1);
    run_->click();
    if (!wait() || !summary_->text().contains(QStringLiteral("стоит пересмотреть")))
        return false;
    answer_->setCurrentIndex(2);
    run_->click();
    if (!wait() || !summary_->text().contains(QStringLiteral("Предположение верное")))
        return false;
    return true;
}
} // namespace live
