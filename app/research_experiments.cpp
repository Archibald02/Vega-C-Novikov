#include "research_panel.hpp"
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>
namespace live {
namespace {
constexpr qint64 archive_limit = 2 * 1024 * 1024;
QString parameter_name(QString key) {
    key.remove(QRegularExpression("^study_"));
    static const QMap<QString, QString> labels{{"window", QStringLiteral("Окно индикаторов")},
                                               {"lags", QStringLiteral("Лаги ACF/PACF")},
                                               {"adf", QStringLiteral("Лаги ADF")},
                                               {"kpss", QStringLiteral("Окно KPSS")},
                                               {"fast", QStringLiteral("Быстрая SMA")},
                                               {"slow", QStringLiteral("Медленная SMA")},
                                               {"lot", QStringLiteral("Размер лота")},
                                               {"annual", QStringLiteral("Периодов в году")},
                                               {"train", QStringLiteral("Обучающие свечи")},
                                               {"test", QStringLiteral("Проверочные свечи")},
                                               {"max_entries", QStringLiteral("Входов за сессию")},
                                               {"cash", QStringLiteral("Начальный капитал")},
                                               {"commission", QStringLiteral("Комиссия, %")},
                                               {"fixed", QStringLiteral("Фиксированная комиссия")},
                                               {"spread", QStringLiteral("Спред, б.п.")},
                                               {"slippage", QStringLiteral("Проскальзывание, б.п.")},
                                               {"allocation", QStringLiteral("Доля капитала, %")},
                                               {"stop", QStringLiteral("Стоп, %")},
                                               {"target", QStringLiteral("Цель, %")},
                                               {"risk", QStringLiteral("Риск сделки, %")},
                                               {"max_dd", QStringLiteral("Лимит просадки, %")},
                                               {"daily", QStringLiteral("Лимит потерь за сессию, %")},
                                               {"participation", QStringLiteral("Доля объёма, %")},
                                               {"tick", QStringLiteral("Шаг цены")},
                                               {"gap", QStringLiteral("Разрыв выборок")},
                                               {"period", QStringLiteral("Период свечей")},
                                               {"ambiguity", QStringLiteral("Порядок стопа и цели")},
                                               {"entry_all", QStringLiteral("Вход: все условия")},
                                               {"exit_all", QStringLiteral("Выход: все условия")},
                                               {"reserve", QStringLiteral("Резерв последних 20%")},
                                               {"manual_metadata", QStringLiteral("Ручные метаданные")},
                                               {"custom_dates", QStringLiteral("Собственные даты")},
                                               {"from", QStringLiteral("Начало запроса")},
                                               {"till", QStringLiteral("Конец запроса")},
                                               {"symbols", QStringLiteral("Инструменты")},
                                               {"data_begin_utc", QStringLiteral("Начало данных UTC, мс")},
                                               {"data_end_utc", QStringLiteral("Конец данных UTC, мс")},
                                               {"bar_count", QStringLiteral("Число свечей")}};
    if (labels.contains(key))
        return labels.value(key);
    const auto match = QRegularExpression("^rule_([01])_([0-9]+)_(enabled|lhs|rhs|op|value)$").match(key);
    if (match.hasMatch()) {
        const QMap<QString, QString> fields{{"enabled", QStringLiteral("включено")},
                                            {"lhs", QStringLiteral("левый показатель")},
                                            {"rhs", QStringLiteral("правый показатель")},
                                            {"op", QStringLiteral("сравнение")},
                                            {"value", QStringLiteral("порог")}};
        return QStringLiteral("%1, условие %2: %3")
            .arg(match.captured(1) == "0" ? QStringLiteral("Вход") : QStringLiteral("Выход"))
            .arg(match.captured(2).toInt() + 1)
            .arg(fields.value(match.captured(3)));
    }
    return key;
}
QString value_text(const QJsonValue &v) {
    if (v.isUndefined())
        return QStringLiteral("отсутствует");
    if (v.isBool())
        return v.toBool() ? QStringLiteral("да") : QStringLiteral("нет");
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 12);
    return QStringLiteral("недоступно");
}
} // namespace
void ResearchPanel::setup_experiments(QVBoxLayout *layout) {
    runs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runs_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto *buttons = new QHBoxLayout;
    auto *edit = new QPushButton(QStringLiteral("Название и гипотеза…"));
    auto *save = new QPushButton(QStringLiteral("Сохранить журнал с капиталом…"));
    auto *open = new QPushButton(QStringLiteral("Открыть журнал…"));
    for (auto *b : {edit, save, open})
        buttons->addWidget(b);
    layout->addLayout(buttons);
    experiment_detail_ = new QLabel;
    experiment_detail_->setWordWrap(true);
    experiment_detail_->setTextFormat(Qt::PlainText);
    experiment_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(experiment_detail_);
    connect(runs_table_, &QTableWidget::itemSelectionChanged, this, &ResearchPanel::compare_experiments);
    connect(edit, &QPushButton::clicked, this, [this] {
        if (worker_->isRunning() || loading_) {
            status_->setText(QStringLiteral("Дождитесь расчёта или отмените его."));
            return;
        }
        const auto rows = runs_table_->selectionModel()->selectedRows();
        if (rows.size() != 1) {
            status_->setText(QStringLiteral("Выберите один запуск."));
            return;
        }
        const int i = rows.front().row();
        if (i < 0 || static_cast<std::size_t>(i) >= runs_.size())
            return;
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Название эксперимента"),
                                  QStringLiteral("До 120 символов"), QLineEdit::Normal, runs_[i].label, &ok)
                .trimmed();
        if (!ok)
            return;
        if (name.isEmpty() || name.size() > 120) {
            status_->setText(QStringLiteral("Название: 1–120 символов."));
            return;
        }
        const auto hypothesis = QInputDialog::getMultiLineText(
            this, QStringLiteral("Гипотеза и заметки"),
            QStringLiteral("До 2000 символов. Не вводите пароли и токены."), runs_[i].hypothesis, &ok);
        if (!ok)
            return;
        if (hypothesis.size() > 2000) {
            status_->setText(QStringLiteral("Заметка слишком длинная."));
            return;
        }
        runs_[i].label = name;
        runs_[i].hypothesis = hypothesis;
        refresh_runs();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Журнал содержит виртуальный капитал, параметры и заметки"), {},
            "Quant experiments (*.qpe.json)");
        if (!path.isEmpty())
            status_->setText(save_experiments(path) ? QStringLiteral("Журнал сохранён локально.")
                                                    : QStringLiteral("Не удалось сохранить журнал."));
    });
    connect(open, &QPushButton::clicked, this, [this] {
        if (worker_->isRunning() || loading_) {
            status_->setText(QStringLiteral("Дождитесь расчёта или отмените его."));
            return;
        }
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Открытие заменяет список сравнений; текущие данные и правила сохраняются"),
            {}, "Quant experiments (*.qpe.json)");
        if (!path.isEmpty())
            status_->setText(
                open_experiments(path)
                    ? QStringLiteral("Журнал открыт как локальный, непроверенный источник результатов.")
                    : QStringLiteral("Журнал отклонён: размер, версия или содержимое некорректны. Текущий "
                                     "список сохранён."));
    });
    compare_experiments();
}
void ResearchPanel::compare_experiments() {
    if (!experiment_detail_)
        return;
    const auto rows = runs_table_->selectionModel()->selectedRows();
    if (rows.isEmpty() || rows.size() > 2) {
        experiment_detail_->setText(QStringLiteral(
            "Выберите один запуск для просмотра или два с Ctrl для сравнения настроек. Журнал сохраняется "
            "только вручную; содержит виртуальный капитал и заметки, но не исходные свечи. Импортированные "
            "результаты не пересчитаны. Для воспроизведения нужны исходные данные и версия программы."));
        return;
    }
    for (const auto &row : rows)
        if (row.row() < 0 || static_cast<std::size_t>(row.row()) >= runs_.size())
            return;
    const auto &a = runs_[rows[0].row()];
    QStringList text{a.label, a.source, QStringLiteral("SHA-256: ") + a.hash, a.hypothesis};
    if (rows.size() == 2) {
        const auto &b = runs_[rows[1].row()];
        text << QStringLiteral("Сравнение с: ") + b.label;
        text << (a.hash == b.hash
                     ? QStringLiteral("Отпечатки данных совпадают; проверьте источник и параметры.")
                     : QStringLiteral(
                           "РАЗНЫЕ ДАННЫЕ: различие результатов нельзя приписывать только настройкам."));
        const auto av = a.parameters.value("values").toObject(), bv = b.parameters.value("values").toObject();
        QSet<QString> keys;
        for (auto i = av.begin(); i != av.end(); ++i)
            keys.insert(i.key());
        for (auto i = bv.begin(); i != bv.end(); ++i)
            keys.insert(i.key());
        auto sorted = keys.values();
        sorted.sort();
        int changes = 0;
        for (const auto &key : sorted)
            if (av.value(key) != bv.value(key)) {
                auto describe = [&](const QJsonValue &v) {
                    auto *combo = findChild<QComboBox *>(key);
                    return combo && v.isDouble() && v.toInt(-1) >= 0 && v.toInt() < combo->count()
                               ? combo->itemText(v.toInt())
                               : value_text(v);
                };
                text << parameter_name(key) + ": " + describe(av.value(key)) + " → " +
                            describe(bv.value(key));
                ++changes;
            }
        for (const auto &key : {"from", "till", "symbols", "data_begin_utc", "data_end_utc", "bar_count"})
            if (a.parameters.value(key) != b.parameters.value(key)) {
                text << parameter_name(QString::fromLatin1(key)) + ": " +
                            value_text(a.parameters.value(key)) + " → " + value_text(b.parameters.value(key));
                ++changes;
            }
        if (!changes)
            text << QStringLiteral("Различий сохранённых параметров нет.");
    }
    experiment_detail_->setText(text.join('\n'));
}
bool ResearchPanel::save_experiments(const QString &path) const {
    QJsonArray entries;
    for (const auto &run : runs_) {
        QJsonArray equity;
        for (double v : run.equity)
            equity.append(v);
        const auto &m = run.metrics;
        entries.append(QJsonObject{
            {"name", run.label},
            {"hypothesis", run.hypothesis},
            {"hash", run.hash},
            {"source", run.source},
            {"parameters", run.parameters},
            {"equity", equity},
            {"metrics", QJsonArray{m.total_return, m.max_drawdown, m.turnover, m.fees, m.execution_cost}},
            {"oos", std::isfinite(run.oos) ? QJsonValue(run.oos) : QJsonValue(QJsonValue::Null)}});
    }
    const auto bytes =
        QJsonDocument(QJsonObject{{"format", "QuantPriceLiveExperiments"}, {"schema", 1}, {"runs", entries}})
            .toJson();
    if (bytes.size() > archive_limit)
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool ResearchPanel::open_experiments(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > archive_limit)
        return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.read(archive_limit + 1), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const auto root = doc.object();
    if (root.size() != 3 || root.value("format") != "QuantPriceLiveExperiments" ||
        root.value("schema") != 1 || !root.value("runs").isArray())
        return false;
    const auto entries = root.value("runs").toArray();
    if (entries.size() > 10)
        return false;
    std::vector<SavedRun> restored;
    for (const auto &entry : entries) {
        if (!entry.isObject())
            return false;
        const auto object = entry.toObject();
        if (object.size() != 8)
            return false;
        for (const auto &key : {"name", "hypothesis", "hash", "source"})
            if (!object.value(key).isString())
                return false;
        SavedRun run;
        run.label = object.value("name").toString();
        run.hypothesis = object.value("hypothesis").toString();
        run.hash = object.value("hash").toString();
        run.source = object.value("source").toString();
        if (run.label.isEmpty() || run.label.size() > 120 || run.hypothesis.size() > 2000 ||
            run.source.size() > 256 || !QRegularExpression("^[0-9a-f]{64}$").match(run.hash).hasMatch())
            return false;
        if (!object.value("parameters").isObject())
            return false;
        run.parameters = object.value("parameters").toObject();
        const QSet<QString> allowed{"values",         "symbols",      "from",     "till",
                                    "data_begin_utc", "data_end_utc", "bar_count"};
        for (auto i = run.parameters.begin(); i != run.parameters.end(); ++i)
            if (!allowed.contains(i.key()))
                return false;
        if (!apply_configuration(run.parameters, true))
            return false;
        const auto values = object.value("metrics").toArray();
        if (values.size() != 5)
            return false;
        for (const auto &v : values)
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 1e18)
                return false;
        run.metrics.total_return = values[0].toDouble();
        run.metrics.max_drawdown = values[1].toDouble();
        run.metrics.turnover = values[2].toDouble();
        run.metrics.fees = values[3].toDouble();
        run.metrics.execution_cost = values[4].toDouble();
        if (!object.value("equity").isArray())
            return false;
        const auto equity = object.value("equity").toArray();
        if (equity.isEmpty() || equity.size() > 5000)
            return false;
        for (const auto &v : equity) {
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 1e18)
                return false;
            run.equity.push_back(v.toDouble());
        }
        if (run.equity.front() <= 0)
            return false;
        const auto oos = object.value("oos");
        if (!oos.isNull() &&
            (!oos.isDouble() || !std::isfinite(oos.toDouble()) || std::abs(oos.toDouble()) > 1e18))
            return false;
        run.oos = oos.isNull() ? quant::unavailable() : oos.toDouble();
        run.imported = true;
        restored.push_back(std::move(run));
    }
    runs_ = std::move(restored);
    refresh_runs();
    return true;
}
bool ResearchPanel::experiments_test() {
    const auto saved = runs_;
    if (saved.empty())
        return false;
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const auto path = dir.filePath("runs.qpe.json");
    runs_.back().label = QStringLiteral("Проверка опыта");
    runs_.back().hypothesis = QStringLiteral("<b>Простой текст</b>");
    const auto expected = runs_;
    const auto controls = configuration();
    if (!save_experiments(path) || !open_experiments(path) || configuration() != controls ||
        runs_.size() != expected.size() || runs_.back().parameters != expected.back().parameters ||
        runs_.back().equity != expected.back().equity ||
        runs_.back().hypothesis != expected.back().hypothesis)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const auto valid_root = root;
    root.insert("schema", 999);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson());
    file.close();
    if (open_experiments(path) || runs_.back().label != expected.back().label)
        return false;
    auto reject = [&](const QJsonObject &bad) {
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        out.write(QJsonDocument(bad).toJson());
        out.close();
        return !open_experiments(path) && runs_.size() == expected.size() &&
               runs_.back().equity == expected.back().equity && configuration() == controls;
    };
    auto list = valid_root.value("runs").toArray();
    auto bad_entry = list[0].toObject();
    bad_entry.insert("metrics", QJsonArray{QStringLiteral("NaN"), 0, 0, 0, 0});
    list[0] = bad_entry;
    root = valid_root;
    root.insert("runs", list);
    if (!reject(root))
        return false;
    list = valid_root.value("runs").toArray();
    while (list.size() < 11)
        list.append(list[0]);
    root.insert("runs", list);
    if (!reject(root))
        return false;
    list = valid_root.value("runs").toArray();
    bad_entry = list[0].toObject();
    auto parameters = bad_entry.value("parameters").toObject();
    auto values = parameters.value("values").toObject();
    values.insert("study_external_url", "https://example.invalid");
    parameters.insert("values", values);
    bad_entry.insert("parameters", parameters);
    list[0] = bad_entry;
    root.insert("runs", list);
    if (!reject(root))
        return false;
    list = valid_root.value("runs").toArray();
    bad_entry = list[0].toObject();
    QJsonArray oversized;
    for (int i = 0; i < 5001; ++i)
        oversized.append(100.0);
    bad_entry.insert("equity", oversized);
    list[0] = bad_entry;
    root.insert("runs", list);
    if (!reject(root))
        return false;
    runs_ = saved;
    refresh_runs();
    return true;
}
} // namespace live
