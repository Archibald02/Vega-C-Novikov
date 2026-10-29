#include "json_http.hpp"
#include "quote_parser.hpp"
#include "research_panel.hpp"
#include "ui_helpers.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
namespace live {
namespace {
QString num(double x, int digits = 3) {
    return std::isfinite(x) ? QString::number(x, 'f', digits) : QStringLiteral("—");
}
QLabel *note(const QString &s) {
    auto *w = new QLabel(s);
    w->setWordWrap(true);
    w->setTextFormat(Qt::PlainText);
    return w;
}
QTableWidget *grid(const QStringList &headers) {
    auto *w = new QTableWidget(0, headers.size());
    w->setHorizontalHeaderLabels(headers);
    w->setEditTriggers(QAbstractItemView::NoEditTriggers);
    w->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    w->horizontalHeader()->setStretchLastSection(true);
    w->setMinimumHeight(170);
    return w;
}
void row(QTableWidget *w, const QStringList &values) {
    const int n = w->rowCount();
    w->insertRow(n);
    for (int i = 0; i < values.size(); ++i)
        w->setItem(n, i, new QTableWidgetItem(values[i]));
}
QJsonArray encode_bars(const std::vector<quant::Bar> &bars) {
    QJsonArray out;
    for (const auto &b : bars)
        out.append(QJsonArray{static_cast<double>(b.time), static_cast<double>(b.end), b.open, b.high, b.low,
                              b.close, b.volume, b.utc_offset_seconds});
    return out;
}
std::vector<quant::Bar> decode_bars(const QJsonArray &array) {
    if (array.size() > 5000)
        throw std::invalid_argument("Bar cap");
    std::vector<quant::Bar> bars;
    for (const auto &item : array) {
        const auto r = item.toArray();
        if (r.size() != 7 && r.size() != 8)
            throw std::invalid_argument("Bar schema");
        for (const auto &v : r)
            if (!v.isDouble() || !std::isfinite(v.toDouble()))
                throw std::invalid_argument("Bar type");
        if (r[0].toDouble() < 1 || r[0].toDouble() > 32503680000000. || r[1].toDouble() < 1 ||
            r[1].toDouble() > 32503852800000.)
            throw std::invalid_argument("Time range");
        const auto offset = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(r[0].toDouble()))
                                .toTimeZone(QTimeZone("Europe/Moscow"))
                                .offsetFromUtc();
        if (r.size() == 8 && r[7].toDouble() != offset)
            throw std::invalid_argument("Session zone");
        bars.push_back({static_cast<qint64>(r[0].toDouble()), static_cast<qint64>(r[1].toDouble()),
                        r[2].toDouble(), r[3].toDouble(), r[4].toDouble(), r[5].toDouble(), r[6].toDouble(),
                        offset});
    }
    if (!bars.empty())
        quant::validate_bars(bars);
    return bars;
}
QString bars_hash(const std::vector<quant::Bar> &bars) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(encode_bars(bars)).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
bool write_report(const QString &html, const QString &path, bool pdf) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (!pdf) {
        const auto bytes = html.toUtf8();
        return file.write(bytes) == bytes.size() && file.commit();
    }
    {
        QPdfWriter writer(&file);
        writer.setResolution(120);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setTitle("Quant Price Live 6.1.0");
        QTextDocument doc;
        doc.setHtml(html);
        doc.print(&writer);
    }
    return file.commit();
}
QVector<QPointF> decimate(const std::vector<double> &values) {
    QVector<QPointF> out;
    const auto step = std::max<std::size_t>(1, (values.size() + 498) / 499);
    for (std::size_t i = 0; i < values.size(); i += step)
        out.push_back({static_cast<double>(i), values[i]});
    if (!values.empty() && (out.isEmpty() || out.back().x() != values.size() - 1))
        out.push_back({static_cast<double>(values.size() - 1), values.back()});
    return out;
}
} // namespace
QWidget *ResearchPanel::workbench_page() {
    auto *page = new QWidget;
    auto *l = new QVBoxLayout(page);
    auto *robust_layout = l;
    expert_ = new QCheckBox(QStringLiteral("Экспертные параметры"));
    expert_->setToolTip(QStringLiteral(
        "Открывает лаги тестов и подробную диагностику. Настройки расчёта при переключении не меняются."));
    preflight_ = note(QStringLiteral("Проверка готовности появится после выбора данных."));
    quality_layout_->addWidget(preflight_);
    auto *metadata_controls = new QWidget;
    auto *metadata_layout = new QVBoxLayout(metadata_controls);
    auto *metadata_row = new QHBoxLayout;
    auto *fetch_metadata = new QPushButton(QStringLiteral("Лот и шаг из MOEX"));
    fetch_metadata->setObjectName("loadMetadata");
    metadata_row->addWidget(fetch_metadata);
    lot_->setPrefix(QStringLiteral("Лот: "));
    metadata_row->addWidget(lot_);
    manual_metadata_ = new QCheckBox(QStringLiteral("Метаданные проверены вручную"));
    metadata_layout->addWidget(manual_metadata_);
    tick_ = new QDoubleSpinBox;
    tick_->setDecimals(8);
    tick_->setRange(0, 1e9);
    tick_->setValue(0);
    tick_->setPrefix(QStringLiteral("Шаг ₽ "));
    metadata_row->addWidget(tick_);
    metadata_layout->insertLayout(0, metadata_row);
    metadata_ = note(QStringLiteral(
        "ДЕМО допускает условный лот/шаг. Для MOEX нужны метаданные или явная ручная проверка."));
    metadata_layout->addWidget(metadata_);
    data_layout_->addWidget(
        ui::fold(QStringLiteral("Лот и шаг цены • загружаются автоматически"), metadata_controls));
    connect(fetch_metadata, &QPushButton::clicked, this, [this] { catalog_.load(true); });
    connect(&catalog_, &InstrumentCatalog::updated, this, &ResearchPanel::update_metadata);
    connect(&catalog_, &InstrumentCatalog::failed, this, [this](const QString &e) { metadata_->setText(e); });
    auto *split = new QHBoxLayout;
    reserve_ = new QCheckBox(QStringLiteral("Оставить последние 20% для финальной проверки"));
    reserve_->setChecked(true);
    gap_ = new QSpinBox;
    gap_->setRange(0, 100);
    gap_->setPrefix(QStringLiteral("Разрыв, свечей "));
    auto *final = new QPushButton(QStringLiteral("Открыть отложенные 20% истории"));
    data_layout_->addWidget(reserve_);
    data_layout_->addWidget(
        note(QStringLiteral("Эта часть истории не участвует в подборе настроек. Откройте её один раз на шаге "
                            "«Устойчивость», когда стратегия уже выбрана.")));
    split->addWidget(gap_);
    split->addWidget(final);
    l->addLayout(split);
    holdout_status_ =
        note(QStringLiteral("Финальные 20% исключены из аналитики, сетки и walk-forward. Повторные просмотры "
                            "отмечаются и перестают быть независимой проверкой."));
    l->addWidget(holdout_status_);
    connect(final, &QPushButton::clicked, this, [this] {
        if (worker_->isRunning() || loading_)
            return;
        const auto b = selected_bars();
        if (b.size() < 100) {
            holdout_status_->setText(QStringLiteral("Для финальной проверки нужно хотя бы 100 свечей."));
            return;
        }
        const auto key = bars_hash(b);
        if (holdout_used_.size() >= 1000 && !holdout_used_.count(key)) {
            holdout_status_->setText(
                QStringLiteral("Лимит 1000 отпечатков исследований; сохраните проект и начните отдельную "
                               "сессию. Независимость нового теста здесь не подтверждается."));
            return;
        }
        const bool used = holdout_used_[key];
        holdout_used_[key] = true;
        holdout_key_ = key;
        evaluate_holdout_ = true;
        reserve_->setChecked(true);
        holdout_status_->setText(
            used ? QStringLiteral("ПОВТОРНЫЙ ПРОСМОТР: независимость holdout утрачена.")
                 : QStringLiteral(
                       "Первое открытие финального участка; параметры выбираются только на первых 80%."));
        calculate();
    });
    stability_ = grid({QStringLiteral("fast / slow"), "slow ×0,8", "slow ×1,0", "slow ×1,2"});
    l->addWidget(note(QStringLiteral(
        "Соседние параметры: 80% / 100% / 120%; цвет результата показывает устойчивость, а не прогноз.")));
    stability_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    l->addWidget(stability_);
    stress_ = grid({QStringLiteral("Стресс-сценарий"), QStringLiteral("Результат, %"),
                    QStringLiteral("Просадка, %"), QStringLiteral("Оборот")});
    l->addWidget(stress_);
    runs_table_ =
        grid({QStringLiteral("Запуск"), QStringLiteral("Чистый результат, %"), QStringLiteral("Просадка, %"),
              QStringLiteral("Оборот"), QStringLiteral("Издержки, ₽"), QStringLiteral("Следующие отрезки, %"),
              QStringLiteral("Код данных")});
    l->addWidget(runs_table_);
    runs_chart_ = new VisualChart;
    l->addWidget(runs_chart_);
    setup_experiments(l);
    l->addWidget(note(QStringLiteral("В памяти — последние 10 запусков; на графике последние 6, капитал "
                                     "нормирован к 100. Сравнивайте одинаковые периоды и источник. Издержки "
                                     "включены в результат, но отдельный налоговый расчёт не выполняется.")));
    project_page_ = new QWidget;
    l = new QVBoxLayout(project_page_);
    auto *project_layout = l;
    l->addWidget(note(QStringLiteral("Проект сохраняет настройки для продолжения работы. Отчёт фиксирует "
                                     "последний завершённый расчёт. Файлы остаются на вашем компьютере.")));
    result_context_ = note(QStringLiteral("Отчёт ещё не рассчитан."));
    l->addWidget(result_context_);
    auto *files = new QHBoxLayout;
    auto *save = new QPushButton(QStringLiteral("Сохранить настройки…"));
    auto *open = new QPushButton(QStringLiteral("Открыть проект…"));
    auto *html = new QPushButton(QStringLiteral("Экспорт HTML…"));
    auto *pdf = new QPushButton(QStringLiteral("Экспорт PDF…"));
    for (auto *b : {save, open, html, pdf})
        files->addWidget(b);
    l->addLayout(files);
    save_cash_ = new QCheckBox(QStringLiteral("Включить настройки виртуального капитала в проект"));
    save_account_ =
        new QCheckBox(QStringLiteral("Включить котировки и архивный виртуальный счёт (локальные данные)"));
    l->addWidget(save_cash_);
    l->addWidget(save_account_);
    connect(save, &QPushButton::clicked, this, [this] {
        const auto p = QFileDialog::getSaveFileName(this, QStringLiteral("Локальный проект"), {},
                                                    "Quant project (*.qpl.json)");
        if (!p.isEmpty())
            status_->setText(
                save_project(p, save_account_->isChecked())
                    ? QStringLiteral("Проект сохранён локально.")
                    : QStringLiteral(
                          "Проект не сохранён; проверьте путь. Для счёта включите сохранение капитала."));
    });
    connect(open, &QPushButton::clicked, this, [this] {
        const auto p = QFileDialog::getOpenFileName(this, QStringLiteral("Открыть локальный проект"), {},
                                                    "Quant project (*.json)");
        if (!p.isEmpty())
            status_->setText(
                open_project(p)
                    ? QStringLiteral("Проект открыт. Результаты нужно пересчитать.")
                    : QStringLiteral(
                          "Проект отклонён: версия, размер, схема или данные не прошли проверку."));
    });
    connect(html, &QPushButton::clicked, this, [this] { export_report(false); });
    connect(pdf, &QPushButton::clicked, this, [this] { export_report(true); });
    auto *live = new QPushButton(QStringLiteral("Новый счёт: новые закрытые свечи MOEX"));
    live->setText(QStringLiteral("Следить за новыми свечами MOEX (с задержкой)"));
    qobject_cast<QVBoxLayout *>(tabs_->widget(3)->layout())->insertWidget(2, live);
    connect(live, &QPushButton::clicked, this, &ResearchPanel::start_live_paper);
    l = quality_layout_;
    auto *reference = new QPushButton(QStringLiteral("Сверить пропуски с торговыми датами MOEX"));
    l->addWidget(reference);
    connect(reference, &QPushButton::clicked, this, &ResearchPanel::fetch_reference);
    reference_status_ = note(QStringLiteral(
        "Календарь сверяется с наблюдаемыми дневными свечами индекса IMOEX; это не полный календарь биржевых "
        "исключений. Дивиденды/сплиты не подменяются догадками и не корректируют цены автоматически."));
    l->addWidget(reference_status_);
    events_ =
        grid({QStringLiteral("Дата МСК"), QStringLiteral("Тип"), QStringLiteral("Наблюдение / источник")});
    l->addWidget(events_);
    about_page_ = new QWidget;
    l = new QVBoxLayout(about_page_);
    l->addWidget(note(QStringLiteral("Короткий словарь")));
    l->addWidget(note(QStringLiteral(
        "Свеча — цены открытия, максимума, минимума и закрытия за час или день.\nSMA / EMA — скользящие "
        "средние, сглаживают колебания цены.\nRSI — индикатор силы движения; значения от 0 до 100.\nПросадка "
        "— падение капитала от предыдущего максимума.\nСпред — разница между ценами покупки и продажи. 1 "
        "б.п. = 0,01%.\nБэктест — виртуальная проверка правил на прошлых свечах.\nWalk-forward — подбор "
        "настроек на одном отрезке и проверка на следующем.\nHoldout — отдельный финальный отрезок, который "
        "не используют при подборе.\nOOS — результат на данных вне обучения.\nSharpe / Sortino — доходность "
        "относительно изменчивости / отрицательных отклонений.\nНедоступно (—) — показатель нельзя корректно "
        "вычислить; это не нулевой результат.")));
    auto *release = new QPushButton(QStringLiteral("Версия и проверка обновлений"));
    l->addWidget(release);
    release_status_ =
        note(QStringLiteral(
                 "Quant Price Live 6.1.0 • C++17 / Qt %1 • %2 • %3\nWindows 10/11 x64; если отсутствует "
                 "MSVCP140.dll/VCRUNTIME140.dll — установите официальный Microsoft Visual C++ "
                 "Redistributable x64. При запуске приложение не проверяет обновления и ничего не загружает.")
                 .arg(qVersion(), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));
    l->addWidget(release_status_);
    connect(release, &QPushButton::clicked, this, [this] {
        if (release_reply_)
            return;
        release_reply_ = request_json(
            study_network_,
            QNetworkRequest(QUrl("https://api.github.com/repos/Archibald02/Vega-C-Novikov/releases/latest")),
            this,
            [this](const HttpResult &r) {
                release_reply_.clear();
                if (!r.ok()) {
                    release_status_->setText(QStringLiteral(
                        "Версия приложения 6.1.0. Публичного релиза нет или API недоступен. Проверенные "
                        "сборки находятся в GitHub Actions; обновление устанавливается вручную."));
                    return;
                }
                const auto tag = r.document.object().value("tag_name").toString().left(80);
                release_status_->setText(
                    QStringLiteral("Установлена 6.1.0; последний опубликованный релиз: %1. Сравните версию "
                                   "на GitHub перед ручной установкой. Автоматической замены файлов нет.")
                        .arg(tag));
            },
            {}, {128 * 1024, 10000});
    });
    auto *links = new QHBoxLayout;
    const QList<QPair<QString, QString>> urls{
        {QStringLiteral("Сборки Windows"), "https://github.com/Archibald02/Vega-C-Novikov/actions"},
        {QStringLiteral("VC++ x64"), "https://aka.ms/vs/17/release/vc_redist.x64.exe"},
        {QStringLiteral("API MOEX"), "https://www.moex.com/a8531"}};
    for (const auto &u : urls) {
        auto *b = new QPushButton(u.first);
        links->addWidget(b);
        connect(b, &QPushButton::clicked, this, [u] { QDesktopServices::openUrl(QUrl(u.second)); });
    }
    l->addLayout(links);
    l->addStretch();
    project_layout->addStretch();
    robust_layout->addStretch();
    return page;
}
void ResearchPanel::setup_workbench() {
    const QList<QPair<QString, QObject *>> parameters{{"window", window_},
                                                      {"lags", lags_},
                                                      {"adf", adf_lags_},
                                                      {"kpss", kpss_lags_},
                                                      {"fast", fast_},
                                                      {"slow", slow_},
                                                      {"lot", lot_},
                                                      {"annual", annual_},
                                                      {"train", train_},
                                                      {"test", test_},
                                                      {"max_entries", max_entries_},
                                                      {"cash", cash_},
                                                      {"commission", commission_},
                                                      {"fixed", fixed_},
                                                      {"spread", spread_},
                                                      {"slippage", slippage_},
                                                      {"allocation", allocation_},
                                                      {"stop", stop_},
                                                      {"target", target_},
                                                      {"risk", risk_},
                                                      {"max_dd", max_dd_},
                                                      {"daily", daily_},
                                                      {"participation", participation_},
                                                      {"tick", tick_},
                                                      {"gap", gap_},
                                                      {"period", period_},
                                                      {"ambiguity", ambiguity_},
                                                      {"entry_all", entry_all_},
                                                      {"exit_all", exit_all_},
                                                      {"reserve", reserve_},
                                                      {"manual_metadata", manual_metadata_},
                                                      {"custom_dates", custom_dates_}};
    for (const auto &p : parameters)
        p.second->setObjectName("study_" + p.first);
    for (int group = 0; group < 2; ++group) {
        auto &rules = group == 0 ? entry_rules_ : exit_rules_;
        for (std::size_t i = 0; i < rules.size(); ++i) {
            const QString prefix = QString("study_rule_%1_%2_").arg(group).arg(i);
            rules[i].enabled->setObjectName(prefix + "enabled");
            rules[i].lhs->setObjectName(prefix + "lhs");
            rules[i].rhs->setObjectName(prefix + "rhs");
            rules[i].op->setObjectName(prefix + "op");
            rules[i].value->setObjectName(prefix + "value");
        }
    }
    connect(expert_, &QCheckBox::toggled, this, [this](bool on) { analytics_expert_->setVisible(on); });
    connect(manual_metadata_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked)
            update_metadata();
    });
    connect(selected_, &QComboBox::currentTextChanged, this, [this] {
        update_metadata();
        preflight();
    });
    connect(reserve_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && !selected_bars().empty()) {
            const auto key = bars_hash(selected_bars());
            if (holdout_used_.size() < 1000 || holdout_used_.count(key))
                holdout_used_[key] = true;
            else
                holdout_status_->setText(QStringLiteral(
                    "Лимит истории открытых участков; независимость новых проверок не подтверждается."));
        }
        preflight();
    });
    for (auto *chart : {price_chart_, equity_chart_, drawdown_chart_}) {
        chart->setToolTip(VisualChart::interaction_help());
        connect(chart, &VisualChart::range_changed, this, [this, chart](double a, double b) {
            for (auto *other : {price_chart_, equity_chart_, drawdown_chart_})
                if (other != chart)
                    other->set_range(a, b);
        });
        connect(chart, &VisualChart::cursor_changed, this, [this, chart](double f) {
            for (auto *other : {price_chart_, equity_chart_, drawdown_chart_})
                if (other != chart)
                    other->set_cursor(f);
        });
    }
    connect(backtest_journal_, &QTableWidget::cellClicked, this, [this](int index, int) {
        if (last_result_.simulation.journal.empty())
            return;
        const auto n = last_result_.simulation.journal.size();
        const auto at = (n > 500 ? n - 500 : 0) + index;
        if (at >= n)
            return;
        const auto bars = selected_bars();
        const auto time = last_result_.simulation.journal[at].time;
        auto it = std::lower_bound(bars.begin(), bars.end(), time,
                                   [](const quant::Bar &b, qint64 t) { return b.time < t; });
        if (it != bars.end()) {
            price_chart_->focus_x(it - bars.begin());
            tabs_->setCurrentIndex(0);
        }
    });
    connect(&loader_, &MarketHistory::progress, this, [this](const QString &s, int n, int limit) {
        status_->setText(
            QStringLiteral("%1: загружено %2 / максимум %3 свечей; можно отменить").arg(s).arg(n).arg(limit));
    });
    connect(&loader_, &MarketHistory::truncated, this, [this](const QString &s) {
        diagnostics_ << s + QStringLiteral(
                                ": достигнут лимит 5000, период усечён справа; выберите более узкие даты.");
    });
    live_timer_.setInterval(60000);
    connect(&live_timer_, &QTimer::timeout, this, &ResearchPanel::refresh_paper);
    connect(&live_loader_, &MarketHistory::truncated, this, [this](const QString &) {
        if (paper_)
            paper_->request_stop(
                "Переподключение превысило лимит 5000 свечей; новые цены не пропускаются молча");
        live_timer_.stop();
        live_paper_ = false;
        update_paper();
    });
    connect(&live_loader_, &MarketHistory::failed, this, [this](const QString &e) {
        if (live_paper_)
            paper_status_->setText(
                QStringLiteral("Новые свечи: %1. Состояние сохранено в RAM; повтор через 60 с.").arg(e));
    });
    connect(
        &live_loader_, &MarketHistory::loaded, this,
        [this](const QString &s, int, const QVector<Candle> &candles, bool) {
            if (!live_paper_ || !paper_ || s != live_symbol_)
                return;
            const auto now = QDateTime::currentMSecsSinceEpoch();
            try {
                for (const auto &c : candles) {
                    const auto end = c.time_ms + (live_interval_ == 60 ? 3600000 : 86400000);
                    // Public feeds may lag. Do not consume a still-publishing interval immediately after
                    // its boundary.
                    if (c.time_ms <= live_last_ || end > now - 20 * 60000)
                        continue;
                    quant::Bar b{
                        c.time_ms,
                        end,
                        c.open,
                        c.high,
                        c.low,
                        c.close,
                        c.volume,
                        QDateTime::fromMSecsSinceEpoch(c.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).offsetFromUtc()};
                    if (paper_bars_.size() >= 5000)
                        throw std::length_error("Session cap");
                    paper_->step(b);
                    paper_bars_.push_back(b);
                    paper_index_ = paper_bars_.size();
                    live_last_ = c.time_ms;
                }
                update_paper();
            } catch (...) {
                paper_->request_stop("Лимит 5000 свечей или ошибка новых данных");
                live_timer_.stop();
                update_paper();
            }
        });
}
QJsonObject ResearchPanel::configuration() const {
    QJsonObject values;
    for (auto *w : findChildren<QWidget *>())
        if (w->objectName().startsWith("study_")) {
            if (auto *d = qobject_cast<QDoubleSpinBox *>(w))
                values.insert(w->objectName(), d->value());
            else if (auto *n = qobject_cast<QSpinBox *>(w))
                values.insert(w->objectName(), n->value());
            else if (auto *c = qobject_cast<QComboBox *>(w))
                values.insert(w->objectName(), c->currentIndex());
            else if (auto *c = qobject_cast<QCheckBox *>(w))
                values.insert(w->objectName(), c->isChecked());
        }
    return {{"values", values},
            {"symbols", symbols_->text()},
            {"from", from_->date().toString(Qt::ISODate)},
            {"till", till_->date().toString(Qt::ISODate)}};
}
quant::SimConfig ResearchPanel::configuration_settings(const QJsonObject &object) const {
    const auto v = object.value("values").toObject();
    const auto current = configuration().value("values").toObject();
    auto value = [&](const char *key) {
        const QString name = QStringLiteral("study_") + key;
        return v.contains(name) ? v.value(name).toVariant() : current.value(name).toVariant();
    };
    quant::SimConfig c;
    c.strategy.fast = value("fast").toInt();
    c.strategy.slow = value("slow").toInt();
    c.strategy.indicator_window = value("window").toInt();
    if (c.strategy.fast >= c.strategy.slow)
        throw std::invalid_argument("SMA order");
    c.initial_cash = value("cash").toDouble();
    c.commission_rate = value("commission").toDouble() / 100;
    c.fixed_fee = value("fixed").toDouble();
    c.spread_bps = value("spread").toDouble();
    c.slippage_bps = value("slippage").toDouble();
    c.allocation = value("allocation").toDouble() / 100;
    c.stop_fraction = value("stop").toDouble() / 100;
    c.target_fraction = value("target").toDouble() / 100;
    c.risk_per_trade = value("risk").toDouble() / 100;
    c.max_drawdown = value("max_dd").toDouble() / 100;
    c.daily_loss = value("daily").toDouble() / 100;
    c.participation = value("participation").toDouble() / 100;
    c.lot = value("lot").toLongLong();
    c.tick_size = value("tick").toDouble();
    c.max_entries_per_day = value("max_entries").toInt();
    c.annual_periods = value("annual").toInt();
    c.pessimistic = value("ambiguity").toInt() == 0;
    for (int g = 0; g < 2; ++g) {
        auto &group = g == 0 ? c.strategy.entry : c.strategy.exit;
        group.rules.clear();
        group.all = value(g == 0 ? "entry_all" : "exit_all").toBool();
        for (int i = 0; i < 2; ++i) {
            const QString prefix = QString("study_rule_%1_%2_").arg(g).arg(i);
            auto r = [&](const QString &key) {
                return v.contains(prefix + key) ? v.value(prefix + key) : current.value(prefix + key);
            };
            group.rules.push_back(
                {static_cast<quant::Feature>(r("lhs").toInt()), static_cast<quant::Feature>(r("rhs").toInt()),
                 static_cast<quant::Compare>(r("op").toInt()), r("value").toDouble(), r("enabled").toBool()});
        }
    }
    return c;
}
bool ResearchPanel::apply_configuration(const QJsonObject &object, bool validate_only) {
    try {
        const auto symbols = object.value("symbols").toString();
        if (symbols.size() > 256 || parse_symbols(symbols).size() > 6)
            return false;
        const auto from = QDate::fromString(object.value("from").toString(), Qt::ISODate),
                   till = QDate::fromString(object.value("till").toString(), Qt::ISODate);
        if (!from.isValid() || !till.isValid() || from < from_->minimumDate() ||
            till > till_->maximumDate() || from > till || from.daysTo(till) > 7305)
            return false;
        const auto values = object.value("values").toObject();
        if (values.size() > 100)
            return false;
        for (auto it = values.begin(); it != values.end(); ++it) {
            auto *w = findChild<QWidget *>(it.key());
            if (!w || !it.key().startsWith("study_"))
                return false;
            const auto v = it.value();
            if (auto *n = qobject_cast<QDoubleSpinBox *>(w)) {
                if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < n->minimum() ||
                    v.toDouble() > n->maximum())
                    return false;
            } else if (auto *n = qobject_cast<QSpinBox *>(w)) {
                if (!v.isDouble() || std::floor(v.toDouble()) != v.toDouble() ||
                    v.toDouble() < n->minimum() || v.toDouble() > n->maximum())
                    return false;
            } else if (auto *c = qobject_cast<QComboBox *>(w)) {
                if (!v.isDouble() || std::floor(v.toDouble()) != v.toDouble() || v.toDouble() < 0 ||
                    v.toDouble() >= c->count())
                    return false;
            } else if (qobject_cast<QCheckBox *>(w)) {
                if (!v.isBool())
                    return false;
            } else
                return false;
        }
        quant::PaperEngine validate(configuration_settings(object));
        if (validate_only)
            return true;
        // Validate everything before changing the controls. Unknown keys cannot become credentials or URLs.
        for (auto it = values.begin(); it != values.end(); ++it) {
            auto *w = findChild<QWidget *>(it.key());
            const auto v = it.value();
            if (auto *n = qobject_cast<QDoubleSpinBox *>(w))
                n->setValue(v.toDouble());
            else if (auto *n = qobject_cast<QSpinBox *>(w))
                n->setValue(v.toInt());
            else if (auto *c = qobject_cast<QComboBox *>(w))
                c->setCurrentIndex(v.toInt());
            else if (auto *c = qobject_cast<QCheckBox *>(w))
                c->setChecked(v.toBool());
        }
        symbols_->setText(symbols);
        from_->setDate(from);
        till_->setDate(till);
        return true;
    } catch (...) {
        return false;
    }
}
bool ResearchPanel::save_project(const QString &path, bool include_account) {
    if (include_account && !save_cash_->isChecked())
        return false;
    auto parameters = configuration();
    auto values = parameters.value("values").toObject();
    if (!save_cash_->isChecked())
        values.remove("study_cash");
    parameters.insert("values", values);
    QJsonObject project{{"format", "QuantPriceLive"},
                        {"schema", 1},
                        {"app_version", "6.1.0"},
                        {"parameters", parameters},
                        {"attempts", attempts_}};
    QJsonArray seen;
    for (const auto &p : holdout_used_)
        if (p.second)
            seen.append(p.first);
    project.insert("opened_holdouts", seen);
    if (include_account) {
        QJsonObject series;
        for (const auto &p : data_)
            series.insert(p.first, encode_bars(p.second));
        project.insert("series", series);
        project.insert("synthetic", synthetic_);
        project.insert("selected", selected_->currentText());
        if (paper_ && !live_paper_)
            project.insert("paper", QJsonObject{{"parameters", paper_parameters_},
                                                {"bars", encode_bars(paper_bars_)},
                                                {"index", static_cast<int>(paper_index_)},
                                                {"halted", paper_->result().halted},
                                                {"manual_stop", paper_manual_stop_},
                                                {"source", paper_source_.left(256)}});
    }
    const auto bytes = QJsonDocument(project).toJson();
    if (bytes.size() > 8 * 1024 * 1024)
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool ResearchPanel::open_project(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
        return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = doc.object();
    if (error.error != QJsonParseError::NoError || root.value("format") != "QuantPriceLive" ||
        root.value("schema").toInt() != 1)
        return false;
    const QStringList allowed{"format",          "schema", "app_version", "parameters", "attempts",
                              "opened_holdouts", "series", "synthetic",   "selected",   "paper"};
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!allowed.contains(it.key()))
            return false;
    std::map<QString, std::vector<quant::Bar>> dataset;
    std::vector<quant::Bar> paper_bars;
    std::unique_ptr<quant::PaperEngine> restored;
    const auto old = configuration();
    try {
        const auto series = root.value("series").toObject();
        if (series.size() > 6)
            return false;
        for (auto it = series.begin(); it != series.end(); ++it) {
            if (parse_symbols(it.key()) != QStringList{it.key()} || !it.value().isArray())
                return false;
            dataset[it.key()] = decode_bars(it.value().toArray());
        }
        const auto seen = root.value("opened_holdouts").toArray();
        if (seen.size() > 1000)
            return false;
        for (const auto &value : seen)
            if (value.toString().size() != 64)
                return false;
        if (holdout_used_.size() + static_cast<std::size_t>(seen.size()) > 1000)
            return false;
        const auto paper = root.value("paper").toObject();
        const int index = paper.value("index").toInt(-1);
        if (!paper.isEmpty()) {
            if (!paper.value("bars").isArray())
                return false;
            paper_bars = decode_bars(paper.value("bars").toArray());
            if (index < 0 || index > static_cast<int>(paper_bars.size()))
                return false;
            if (!apply_configuration(paper.value("parameters").toObject(), true))
                return false;
            auto c = configuration_settings(paper.value("parameters").toObject());
            c.liquidate_end = false;
            restored = std::make_unique<quant::PaperEngine>(c);
            for (int i = 0; i < index; ++i) {
                if (i == paper.value("manual_stop").toInt(-1))
                    restored->request_stop("Восстановленная ручная остановка");
                restored->step(paper_bars[i]);
            }
            if (paper.value("halted").toBool())
                restored->request_stop("Восстановленная остановка");
        }
        if (!apply_configuration(root.value("parameters").toObject(), true))
            return false;
        cancel_work();
        if (!root.value("parameters").toObject().value("values").toObject().contains("study_cash"))
            cash_->setValue(100000);
        if (!apply_configuration(root.value("parameters").toObject())) {
            apply_configuration(old);
            return false;
        }
        data_ = std::move(dataset);
        paper_ = std::move(restored);
        paper_bars_ = std::move(paper_bars);
        paper_index_ = std::max(0, index);
        paper_parameters_ = paper.value("parameters").toObject();
        paper_manual_stop_ = paper.value("manual_stop").toInt(-1);
        paper_source_ = QStringLiteral("ЛОКАЛЬНЫЙ ПРОЕКТ • ") + paper.value("source").toString().left(256);
        synthetic_ = root.value("synthetic").toBool();
        local_project_ = true;
        source_dirty_ = false;
        result_current_ = false;
        selected_->clear();
        for (const auto &p : data_)
            selected_->addItem(p.first);
        const auto selected = root.value("selected").toString();
        if (selected_->findText(selected) >= 0)
            selected_->setCurrentText(selected);
        attempts_ = std::max(attempts_, std::clamp(root.value("attempts").toInt(), 0, 1000000));
        // Never erase the current session's experiment history by loading an older project.
        for (const auto &value : seen)
            holdout_used_[value.toString()] = true;
        last_result_ = LabResult{};
        report_->clear();
        statistics_->clear();
        feature_text_->clear();
        tests_->clear();
        backtest_journal_->setRowCount(0);
        features_table_->setRowCount(0);
        folds_->setRowCount(0);
        if (!paper_) {
            journal_->setRowCount(0);
            paper_status_->setText(QStringLiteral("Счёт не сохранён в проекте."));
            paper_chart_->clear();
        }
        for (auto *chart : {price_chart_, equity_chart_, drawdown_chart_})
            chart->clear();
        quality_->setText(QStringLiteral("Котировки из локального проекта: происхождение и полнота онлайн не "
                                         "подтверждены. Пересчитайте результат."));
        if (paper_)
            update_paper();
        preflight();
        update_journey();
        return true;
    } catch (...) {
        apply_configuration(old);
        return false;
    }
}
void ResearchPanel::start_live_paper() {
    if (synthetic_ || local_project_ || loading_ || worker_->isRunning()) {
        paper_status_->setText(
            QStringLiteral("Для новых свечей сначала загрузите актуальную публичную историю MOEX."));
        tabs_->setCurrentIndex(3);
        return;
    }
    try {
        auto bars = selected_bars();
        const auto cutoff = QDateTime::currentMSecsSinceEpoch() - 20 * 60000;
        bars.erase(std::remove_if(bars.begin(), bars.end(),
                                  [cutoff](const quant::Bar &b) { return b.end > cutoff; }),
                   bars.end());
        quant::validate_bars(bars);
        auto c = settings();
        c.liquidate_end = false;
        if (bars.size() < 40)
            throw std::invalid_argument("Warmup");
        bool known = manual_metadata_->isChecked();
        for (const auto &i : catalog_.instruments())
            if (i.symbol == selected_->currentText() && i.tick_size > 0)
                known = true;
        if (!known)
            throw std::invalid_argument("Metadata");
        const auto key = bars_hash(selected_bars());
        if (holdout_used_.size() < 1000 || holdout_used_.count(key))
            holdout_used_[key] = true;
        holdout_status_->setText(
            QStringLiteral("Режим новых свечей использует актуальную историю для прогрева. Исторический "
                           "holdout этой выборки больше не считается независимым."));
        paper_timer_.stop();
        live_timer_.stop();
        live_loader_.cancel();
        paper_ = std::make_unique<quant::PaperEngine>(c);
        // History warms features, without opening any historical position or recording historical P&L.
        for (const auto &b : bars)
            paper_->warmup(b);
        paper_->arm_from_warmup();
        paper_bars_.clear();
        paper_index_ = 0;
        paper_manual_stop_ = -1;
        paper_parameters_ = configuration();
        live_symbol_ = selected_->currentText();
        live_interval_ = period_->currentIndex() == 2 ? 60 : 24;
        live_last_ = bars.back().time;
        live_paper_ = true;
        paper_source_ = live_symbol_ + QStringLiteral(" • НОВЫЕ ЗАКРЫТЫЕ СВЕЧИ MOEX, ЗАДЕРЖКА • виртуальное "
                                                      "ретроспективное исполнение, не доступная сейчас цена");
        journal_->setRowCount(0);
        journal_->setProperty("journal_total", 0);
        update_paper();
        tabs_->setCurrentIndex(3);
        live_timer_.start();
        refresh_paper();
    } catch (...) {
        paper_status_->setText(
            QStringLiteral("Нужны ≥40 свечей, метаданные лота/шага и корректные параметры риска."));
        tabs_->setCurrentIndex(3);
    }
}
void ResearchPanel::refresh_paper() {
    if (!live_paper_ || !paper_ || live_loader_.busy())
        return;
    const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Europe/Moscow")).date();
    const auto since =
        QDateTime::fromMSecsSinceEpoch(live_last_).toTimeZone(QTimeZone("Europe/Moscow")).date();
    // Refuse an unbounded reconnect catch-up. No silent skip to the latest quote.
    if (since.daysTo(today) > 365) {
        paper_->request_stop("История слишком устарела; загрузите новые данные");
        live_timer_.stop();
        update_paper();
        return;
    }
    live_loader_.load_range(live_symbol_, since, today, live_interval_, true);
}
void ResearchPanel::fetch_reference() {
    auto b = selected_bars();
    if (reserve_->isChecked())
        b.resize(b.size() * 8 / 10);
    if (b.empty() || synthetic_ || local_project_) {
        reference_status_->setText(QStringLiteral("Нужна загруженная история MOEX, не ДЕМО/проект."));
        return;
    }
    reference_loader_.cancel();
    const auto generation = ++study_generation_;
    if (dividend_reply_)
        dividend_reply_->abort();
    events_->setRowCount(0);
    const auto
        from = QDateTime::fromMSecsSinceEpoch(b.front().time).toTimeZone(QTimeZone("Europe/Moscow")).date(),
        till = QDateTime::fromMSecsSinceEpoch(b.back().time).toTimeZone(QTimeZone("Europe/Moscow")).date();
    disconnect(&reference_loader_, nullptr, this, nullptr);
    connect(
        &reference_loader_, &MarketHistory::loaded, this,
        [this, b](const QString &, int, const QVector<Candle> &reference, bool) {
            QSet<QDate> observed, own;
            for (const auto &c : reference)
                observed.insert(
                    QDateTime::fromMSecsSinceEpoch(c.time_ms).toTimeZone(QTimeZone("Europe/Moscow")).date());
            for (const auto &c : b)
                own.insert(
                    QDateTime::fromMSecsSinceEpoch(c.time).toTimeZone(QTimeZone("Europe/Moscow")).date());
            int missing = 0;
            for (const auto &d : observed)
                if (!own.contains(d)) {
                    ++missing;
                    if (events_->rowCount() < 500)
                        row(events_, {d.toString(Qt::ISODate), QStringLiteral("Нет свечи инструмента"),
                                      QStringLiteral("Дневная свеча IMOEX существует; возможны "
                                                     "приостановка/пропуск/отсутствие сделок")});
                }
            reference_status_->setText(
                reference_status_->text() +
                QStringLiteral("\nIMOEX: %1 наблюдаемых торговых дат; %2 без свечи выбранной акции. "
                               "Остальные даты имеют неизвестный статус, а не автоматически «выходной».")
                    .arg(observed.size())
                    .arg(missing));
        });
    connect(&reference_loader_, &MarketHistory::failed, this, [this](const QString &e) {
        reference_status_->setText(reference_status_->text() + "\nIMOEX: " + e);
    });
    reference_status_->setText(
        QStringLiteral("Проверяем публичные блоки ISS. Цены остаются нескорректированными."));
    reference_loader_.load_range("IMOEX", from, till, 24);
    const auto symbol = selected_->currentText();
    dividend_reply_ = request_json(
        study_network_,
        QNetworkRequest(
            QUrl("https://iss.moex.com/iss/securities/" + symbol + "/dividends.json?iss.meta=off")),
        this,
        [this, symbol, generation, from, till](const HttpResult &r) {
            if (generation != study_generation_)
                return;
            dividend_reply_.clear();
            const auto block = r.document.object().value("dividends").toObject();
            const auto columns = block.value("columns").toArray();
            const auto data = block.value("data").toArray();
            auto index = [&](const QString &name) {
                for (int i = 0; i < columns.size(); ++i)
                    if (columns[i].toString().compare(name, Qt::CaseInsensitive) == 0)
                        return i;
                return -1;
            };
            const int date = index("registryclosedate"), amount = index("value");
            if (!r.ok() || date < 0 || amount < 0) {
                reference_status_->setText(
                    reference_status_->text() +
                    QStringLiteral("\nДивиденды: валидный публичный блок для %1 недоступен (HTTP 200 с "
                                   "description не считается дивидендными данными). Полный публичный реестр "
                                   "сплитов не подтверждён. Проверьте официальную карточку эмитента; "
                                   "неизвестные события не исправляются автоматически.")
                        .arg(symbol));
                return;
            }
            int accepted = 0;
            for (const auto &v : data) {
                const auto a = v.toArray();
                if (date >= a.size() || amount >= a.size())
                    continue;
                const auto d = QDate::fromString(a[date].toString().left(10), Qt::ISODate);
                const double n = a[amount].toDouble(NAN);
                if (!d.isValid() || d < from || d > till || !std::isfinite(n) || n < 0 ||
                    events_->rowCount() >= 500)
                    continue;
                row(events_,
                    {d.toString(Qt::ISODate), QStringLiteral("Реестр дивидендов"),
                     QStringLiteral("%1 • выплата %2; дата реестра НЕ равна ex-date, цены не корректируются")
                         .arg(symbol, num(n))});
                ++accepted;
            }
            reference_status_->setText(
                reference_status_->text() +
                QStringLiteral("\nДивиденды: принято %1 строк. Сплиты: статус неизвестен.").arg(accepted));
        },
        {}, {256 * 1024, 15000});
    for (std::size_t i = 1; i < b.size() && events_->rowCount() < 500; ++i)
        if (std::abs(b[i].open / b[i - 1].close - 1) > .15)
            row(events_,
                {QDateTime::fromMSecsSinceEpoch(b[i].time)
                     .toTimeZone(QTimeZone("Europe/Moscow"))
                     .date()
                     .toString(Qt::ISODate),
                 QStringLiteral("Ценовой разрыв >15%"),
                 QStringLiteral("%1%; причина неизвестна: не объявляется автоматически сплитом/дивидендом")
                     .arg(num((b[i].open / b[i - 1].close - 1) * 100))});
}
void ResearchPanel::update_metadata() {
    lot_->setEnabled(synthetic_ || manual_metadata_->isChecked());
    tick_->setEnabled(synthetic_ || manual_metadata_->isChecked());
    if (synthetic_) {
        metadata_->setText(QStringLiteral(
            "СИНТЕТИЧЕСКОЕ ДЕМО • лот и шаг условные; биржевые метаданные здесь не используются."));
        return;
    }
    if (manual_metadata_->isChecked())
        return;
    for (const auto &i : catalog_.instruments())
        if (i.symbol == selected_->currentText()) {
            lot_->setValue(i.lot_size);
            tick_->setValue(i.tick_size);
            metadata_->setText(
                QStringLiteral("%1 • TQBR • текущий лот %2 • MINSTEP %3 • знаков %4. Исторические изменения "
                               "лота/шага API этого запроса не восстанавливает.")
                    .arg(i.symbol)
                    .arg(i.lot_size)
                    .arg(i.tick_size, 0, 'g', 10)
                    .arg(i.decimals));
            return;
        }
    metadata_->setText(
        QStringLiteral("Метаданные выбранного инструмента пока неизвестны. Нажмите «Лот и шаг из MOEX»."));
}
void ResearchPanel::preflight() {
    const auto b = selected_bars();
    QStringList lines;
    const auto n = reserve_->isChecked() ? b.size() * 8 / 10 : b.size();
    if (n < 40)
        lines << QStringLiteral("ОШИБКА: в исследовательской части менее 40 свечей.");
    if (fast_->value() >= slow_->value())
        lines << QStringLiteral("ОШИБКА: SMA fast должна быть меньше slow.");
    const int warmup = std::max({slow_->value(), window_->value(), 35});
    if (n <= static_cast<std::size_t>(warmup + 1))
        lines << QStringLiteral(
                     "ПРЕДУПРЕЖДЕНИЕ: почти вся выборка уходит на прогрев индикаторов (%1 свечей).")
                     .arg(warmup);
    if (n < static_cast<std::size_t>(train_->value() + test_->value()))
        lines << QStringLiteral("ПРЕДУПРЕЖДЕНИЕ: для полного окна walk-forward данных недостаточно.");
    if (n <= static_cast<std::size_t>(2 * lags_->value() + 1))
        lines << QStringLiteral("ПРЕДУПРЕЖДЕНИЕ: ACF/PACF будут ограничены половиной выборки.");
    if (n < static_cast<std::size_t>(2 * adf_lags_->value() + 12) ||
        n < static_cast<std::size_t>(2 * kpss_lags_->value() + 2))
        lines << QStringLiteral("ОШИБКА: слишком много лагов для ADF/KPSS.");
    if (!b.empty()) {
        const auto age = (QDateTime::currentMSecsSinceEpoch() - b.back().end) / 3600000.;
        if (age > 96)
            lines << QStringLiteral(
                         "ПРЕДУПРЕЖДЕНИЕ: последней свече %1 ч; проверьте выбранные даты и торговые сессии.")
                         .arg(num(age, 1));
    }
    if (data_.size() > 1) {
        std::vector<std::vector<quant::Bar>> aligned;
        const auto cutoff = n > 0 && n <= b.size() ? b[n - 1].end : 0;
        for (const auto &p : data_) {
            auto series = p.second;
            if (reserve_->isChecked())
                series.erase(std::remove_if(series.begin(), series.end(),
                                            [cutoff](const quant::Bar &c) { return c.end > cutoff; }),
                             series.end());
            if (series.size() >= 3)
                aligned.push_back(std::move(series));
        }
        try {
            const auto common = quant::aligned_correlation(aligned).common_returns;
            lines << QStringLiteral("Согласованных доходностей: %1.%2")
                         .arg(common)
                         .arg(common < 3
                                  ? QStringLiteral(" Корреляция недоступна: нужно ≥3 и ненулевая дисперсия.")
                                  : QString());
        } catch (...) {
            lines << QStringLiteral("Корреляция недоступна: проверьте интервалы.");
        }
    }
    if (!synthetic_ && !manual_metadata_->isChecked() && tick_->value() <= 0)
        lines << QStringLiteral("ОШИБКА: шаг цены неизвестен; загрузите метаданные MOEX.");
    lines.prepend(
        QStringLiteral("Готовность: %1 исследовательских / %2 всего свечей; прогрев до %3; запусков %4. "
                       "Общие интервалы корреляции проверяются расчётом, без заполнения пропусков.")
            .arg(n)
            .arg(b.size())
            .arg(warmup)
            .arg(attempts_));
    preflight_->setText(lines.join('\n'));
}
void ResearchPanel::record_run(const LabResult &r) {
    const auto hash = r.hash;
    if (runs_.size() >= 10)
        runs_.erase(runs_.begin());
    runs_.push_back(
        {QStringLiteral("#%1 • %2").arg(attempts_).arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)),
         hash, r.source, r.parameters, r.simulation.metrics, r.simulation.equity,
         r.walk.folds.empty() ? quant::unavailable() : r.walk.compounded_return});
    stability_->setRowCount(0);
    stress_->setRowCount(0);
    runs_table_->setRowCount(0);
    const auto &stability = r.robustness.stability;
    if (stability.size() == 9) {
        for (int i = 0; i < 3; ++i) {
            QStringList values{QString::number(stability[i * 3].fast)};
            for (int j = 0; j < 3; ++j)
                values << num(stability[i * 3 + j].metrics.total_return * 100) + "%";
            row(stability_, values);
            for (int j = 0; j < 3; ++j) {
                const auto &t = stability[i * 3 + j];
                auto *item = stability_->item(i, j + 1);
                item->setToolTip(QStringLiteral("SMA %1/%2 • просадка %3%")
                                     .arg(t.fast)
                                     .arg(t.slow)
                                     .arg(num(t.metrics.max_drawdown * 100)));
                item->setBackground(!std::isfinite(t.metrics.total_return) ? QColor("#555555")
                                    : t.metrics.total_return >= 0          ? QColor("#245a48")
                                                                           : QColor("#693c45"));
                stability_->horizontalHeaderItem(j + 1)->setText(QString::number(t.slow));
            }
        }
    }
    for (const auto &t : r.robustness.stress)
        row(stress_, {QString::fromStdString(t.name), num(t.metrics.total_return * 100),
                      num(t.metrics.max_drawdown * 100), num(t.metrics.turnover)});
    refresh_runs();
    if (r.has_holdout)
        holdout_status_->setText(
            holdout_status_->text() +
            QStringLiteral("\nФинальный тест: %1%, просадка %2%; заморожено SMA %3/%4, разрыв %5 свечей. "
                           "Исследовано настроек: 8 на обучении; ручных запусков: %6.")
                .arg(num(r.holdout.test.metrics.total_return * 100),
                     num(r.holdout.test.metrics.max_drawdown * 100))
                .arg(r.holdout.frozen.strategy.fast)
                .arg(r.holdout.frozen.strategy.slow)
                .arg(r.holdout.gap)
                .arg(attempts_));
    preflight();
}
void ResearchPanel::refresh_runs() {
    runs_table_->setRowCount(0);
    QVector<PlotLine> lines;
    const QStringList colors{"#39d2b0", "#829eff", "#efc571", "#f08c98", "#b99fe8", "#77b6d0"};
    const auto first = runs_.size() > 6 ? runs_.size() - 6 : 0;
    for (std::size_t i = 0; i < runs_.size(); ++i) {
        const auto &run = runs_[i];
        row(runs_table_, {(run.imported ? QStringLiteral("Файл • ") : QString()) + run.label,
                          num(run.metrics.total_return * 100), num(run.metrics.max_drawdown * 100),
                          num(run.metrics.turnover), num(run.metrics.fees + run.metrics.execution_cost),
                          num(run.oos * 100), run.hash.left(12)});
        runs_table_->item(runs_table_->rowCount() - 1, 0)
            ->setToolTip(run.source + "\n" + QString::fromUtf8(QJsonDocument(run.parameters).toJson()));
        if (i >= first && !run.equity.empty()) {
            auto normalized = run.equity;
            for (auto &v : normalized)
                v = v / run.equity.front() * 100;
            lines.push_back({run.label, QColor(colors[i - first]), decimate(normalized), false});
        }
    }
    runs_chart_->set_lines(
        lines, QStringLiteral("Сравнение запусков, начальный капитал = 100"),
        QStringLiteral("Ось X: порядковый номер свечи; разные периоды не синхронизированы"));
    compare_experiments();
}
QString ResearchPanel::fingerprint(const std::vector<quant::Bar> &bars) const { return bars_hash(bars); }
QString ResearchPanel::report_html(bool compact) const {
    const auto &r = last_result_;
    if (r.simulation.equity.empty())
        return {};
    const auto &m = r.simulation.metrics;
    QString html =
        QStringLiteral("<!doctype html><html lang='ru'><meta charset='utf-8'><title>Quant Price Live — "
                       "исследование</title><style>body{font:14px sans-serif;max-width:1100px;margin:30px "
                       "auto;color:#172a40}table{border-collapse:collapse;width:100%}td,th{border:1px solid "
                       "#b7c5d2;padding:5px;text-align:left}pre{white-space:pre-wrap;word-break:break-all}"
                       "img{max-width:100%}</style><h1>Quant Price Live 6.1.0 — учебное исследование</h1>");
    html += "<p>" + r.source.toHtmlEscaped() + " • отчёт UTC " +
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + "</p>";
    html += "<p>SHA-256 свечей: " + r.hash.toHtmlEscaped() +
            "</p><p>Свечей: " + QString::number(r.parameters.value("bar_count").toInt()) +
            "; UTC начало / конец (мс): " + r.parameters.value("data_begin_utc").toString() + " / " +
            r.parameters.value("data_end_utc").toString() + "</p>";
    html += QStringLiteral("<p>Чистая доходность %1%; просадка по закрытиям %2%; оборот %3; исполнений %4; "
                           "Sharpe %5; Sortino %6; Calmar %7.</p>")
                .arg(num(m.total_return * 100), num(m.max_drawdown * 100), num(m.turnover))
                .arg(m.fills)
                .arg(num(m.sharpe), num(m.sortino), num(m.calmar));
    html += QStringLiteral("<p>База держать: %1%; деньги: 0%; walk-forward: %2 окон, OOS %3%. Финальный "
                           "тест: %4. Число ручных запусков в сессии: %5.</p>")
                .arg(num(r.baseline.metrics.total_return * 100))
                .arg(r.walk.folds.size())
                .arg(num(r.walk.compounded_return * 100),
                     r.has_holdout ? num(r.holdout.test.metrics.total_return * 100) + "%"
                                   : QStringLiteral("не открыт"))
                .arg(attempts_);
    // Render from the immutable result, not from mutable/zoomed GUI widgets.
    auto plot = [](const std::vector<double> &values, const QColor &color) {
        QImage image(1000, 260, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        if (!values.empty()) {
            const auto bounds = std::minmax_element(values.begin(), values.end());
            double lo = *bounds.first, hi = *bounds.second;
            if (hi <= lo)
                hi = lo + 1;
            p.setPen(QPen(color, 2));
            QPolygonF line;
            const auto step = std::max<std::size_t>(1, (values.size() + 998) / 999);
            for (std::size_t i = 0; i < values.size(); i += step)
                line << QPointF(55. + i * 925. / std::max<std::size_t>(1, values.size() - 1),
                                225. - (values[i] - lo) / (hi - lo) * 200.);
            if (line.size() > 1)
                p.drawPolyline(line);
            p.setPen(Qt::black);
            p.drawText(5, 20, QString::number(hi, 'f', 2));
            p.drawText(5, 245, QString::number(lo, 'f', 2));
        }
        p.end();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return QString::fromLatin1(bytes.toBase64());
    };
    html += QStringLiteral("<p>Комиссии %1 ₽; модельная потеря на спреде/проскальзывании/шаге %2 ₽. Эти "
                           "суммы уже учтены в капитале, повторно не вычитаются.</p>")
                .arg(num(m.fees), num(m.execution_cost));
    html += "<h2>Капитал, ₽</h2><img src='data:image/png;base64," +
            plot(r.simulation.equity, QColor("#16896c")) +
            "'><h2>Просадка (доля)</h2><img src='data:image/png;base64," +
            plot(r.simulation.drawdowns, QColor("#ba3a54")) + "'>";
    html += QStringLiteral("<p>ADF=%1, критическое 5%%=%2, lag=%3; KPSS=%4, bandwidth=%5. Гипотезы и "
                           "ограничения указаны ниже.</p>")
                .arg(num(r.stationarity.adf), num(r.stationarity.adf_critical5))
                .arg(r.stationarity.adf_lags)
                .arg(num(r.stationarity.kpss))
                .arg(r.stationarity.kpss_lags);
    if (r.has_holdout)
        html += QStringLiteral("<p>Финальная проверка: начало %1; эмбарго %2; замороженные SMA %3/%4; "
                               "результат %5%%; просадка %6%%.</p>")
                    .arg(r.holdout.split)
                    .arg(r.holdout.gap)
                    .arg(r.holdout.frozen.strategy.fast)
                    .arg(r.holdout.frozen.strategy.slow)
                    .arg(num(r.holdout.test.metrics.total_return * 100),
                         num(r.holdout.test.metrics.max_drawdown * 100));
    html += "<h2>Параметры зафиксированного запуска</h2><pre>" +
            QString::fromUtf8(QJsonDocument(r.parameters).toJson()).toHtmlEscaped() + "</pre>";
    html += "<h2>Устойчивость и стресс</h2><table><tr><th>Сценарий / SMA</th><th>Чистый результат, "
            "%</th><th>Просадка, %</th></tr>";
    for (const auto &t : r.robustness.stability)
        html += "<tr><td>SMA " + QString::number(t.fast) + "/" + QString::number(t.slow) + "</td><td>" +
                num(t.metrics.total_return * 100) + "</td><td>" + num(t.metrics.max_drawdown * 100) +
                "</td></tr>";
    for (const auto &t : r.robustness.stress)
        html += "<tr><td>" + QString::fromStdString(t.name).toHtmlEscaped() + "</td><td>" +
                num(t.metrics.total_return * 100) + "</td><td>" + num(t.metrics.max_drawdown * 100) +
                "</td></tr>";
    html += "</table><h2>Решения и виртуальные сделки</h2><table><tr><th>UTC (мс)</th><th>Акций "
            "±</th><th>Цена</th><th>Капитал</th><th>Причина</th></tr>";
    const auto limit =
        compact ? std::min<std::size_t>(500, r.simulation.journal.size()) : r.simulation.journal.size();
    for (std::size_t i = 0; i < limit; ++i) {
        const auto &e = r.simulation.journal[i];
        html += "<tr><td>" + QString::number(e.time) + "</td><td>" + QString::number(e.quantity) +
                "</td><td>" + num(e.price, 6) + "</td><td>" + num(e.equity) + "</td><td>" +
                QString::fromStdString(e.reason).toHtmlEscaped() + "</td></tr>";
    }
    html += "</table><p>Событий показано " + QString::number(limit) + " из " +
            QString::number(r.simulation.journal.size()) +
            ". PDF ограничен 500 событиями; полный журнал — в HTML.</p>";
    html += QStringLiteral(
        "<h2>Модель и ограничения</h2><p>Только long, без плеча и реальных торговых заявок. Сигнал "
        "рассчитывается на закрытии, исполнение на следующем доступном открытии с издержками; шаг "
        "округляется против стратегии. Стоп/цель — рыночные триггеры; при касании обоих действует выбранный "
        "приоритет. Финальная ликвидация по закрытию известного конца теста. Лимиты проверяются по закрытиям "
        "и не гарантируют максимальный убыток при гэпе. Риск размера включает комиссии входа/выхода и "
        "ожидаемое исполнение стопа, но не неизвестные гэпы/повторные комиссии частичных выходов. Дивиденды, "
        "сплиты и налоги не моделируются. Текущие метаданные не заменяют исторические. Sharpe/Sortino без "
        "годового масштаба, если N=0; ставка сравнения 0. Временные доходности могут иметь разные "
        "календарные интервалы. ADF: фиксированные лаги и константа; KPSS: константа и Bartlett HAC. В "
        "тестах нет точных p-value и гарантии прогноза. Повторный подбор или просмотр финального участка "
        "разрушает его независимость; счётчик сессии не предотвращает подбор вне "
        "приложения.</p><p>Источники: https://iss.moex.com/iss/ ; https://www.moex.com/a8531 . Из отчёта не "
        "выполняются внешние запросы. Логинов, токенов и сетевых заголовков нет. Учебная модель не является "
        "инвестиционной рекомендацией.</p></html>");
    return html;
}
void ResearchPanel::export_report(bool pdf) {
    if (exporting_) {
        status_->setText(QStringLiteral("Дождитесь завершения текущего экспорта."));
        return;
    }
    if (last_result_.simulation.equity.empty()) {
        status_->setText(QStringLiteral("Сначала выполните расчёт."));
        return;
    }
    const auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Локальный отчёт содержит параметры капитала и сделки"), {},
        pdf ? "PDF (*.pdf)" : "HTML (*.html)");
    if (path.isEmpty())
        return;
    const auto html = report_html(pdf);
    exporting_ = true;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        exporting_ = false;
        status_->setText(watcher->result() ? QStringLiteral("Отчёт сохранён локально.")
                                           : QStringLiteral("Не удалось сохранить отчёт."));
        watcher->deleteLater();
    });
    status_->setText(QStringLiteral("Экспорт отчёта в фоне…"));
    watcher->setFuture(QtConcurrent::run([html, path, pdf] { return write_report(html, path, pdf); }));
}
bool ResearchPanel::workbench_test() {
    if (last_result_.simulation.equity.empty())
        return false;
    const auto old = configuration();
    const auto previous_result = last_result_;
    if (!experiments_test())
        return false;
    auto bad = old;
    auto values = bad.value("values").toObject();
    values.insert("study_lot", -1);
    bad.insert("values", values);
    if (apply_configuration(bad) || configuration() != old)
        return false;
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const auto path = dir.filePath("project.json");
    save_cash_->setChecked(false);
    if (!save_project(path))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto bytes = file.readAll();
    file.close();
    if (bytes.contains("study_cash") || bytes.contains("\"series\"") || bytes.contains("TOKEN"))
        return false;
    if (!open_project(path))
        return false;
    // Restore demo input after intentionally opening a configuration-only project.
    demo();
    apply_configuration(old);
    if (last_result_.simulation.equity.empty()) {
        const auto b = selected_bars();
        last_result_.simulation = quant::backtest(b, settings());
        last_result_.baseline = quant::buy_hold(b, settings());
        last_result_.parameters = configuration();
        last_result_.source = "СИНТЕТИЧЕСКОЕ ДЕМО";
        last_result_.hash = bars_hash(b);
    }
    last_result_ = previous_result;
    present(previous_result);
    const auto html = report_html();
    if (!html.contains("SHA-256") || !html.contains(last_result_.hash) ||
        !html.contains("data:image/png;base64,"))
        return false;
    auto pdf = QtConcurrent::run(
        [html, path = dir.filePath("report.pdf")] { return write_report(html, path, true); });
    pdf.waitForFinished();
    if (!pdf.result())
        return false;
    QFile pdf_file(dir.filePath("report.pdf"));
    if (!pdf_file.open(QIODevice::ReadOnly) || pdf_file.size() < 1000 || !pdf_file.read(4).startsWith("%PDF"))
        return false;
    start_paper();
    if (reserve_->isChecked() && paper_bars_.size() != selected_bars().size() * 8 / 10)
        return false;
    for (int i = 0; i < 10; ++i)
        step_paper();
    stop_paper(QStringLiteral("Проверка восстановления"));
    const auto before = paper_->result();
    save_cash_->setChecked(true);
    if (!save_project(path, true) || !open_project(path) || !paper_)
        return false;
    const auto restored = paper_->result();
    if (restored.equity != before.equity || restored.units != before.units || restored.cash != before.cash ||
        !restored.halted)
        return false;
    save_cash_->setChecked(false);
    demo();
    apply_configuration(old);
    last_result_ = previous_result;
    present(previous_result);
    const auto robust = quant::robustness(selected_bars(), settings());
    if (robust.stability.size() != 9 || robust.stress.size() != 6)
        return false;
    price_chart_->set_range(.2, .6);
    equity_chart_->set_range(.2, .6);
    price_chart_->reset_view();
    return true;
}
} // namespace live
