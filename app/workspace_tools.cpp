#include "main_window.hpp"
#include "ui_helpers.hpp"
#include <QApplication>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
namespace live {
void MainWindow::show_function_search() {
    if (function_search_) {
        function_search_->show();
        function_search_->raise();
        function_search_->activateWindow();
        function_search_->query()->setFocus();
        return;
    }
    auto *dialog = new FunctionSearch(this);
    function_search_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this,
            [this, dialog] { navigate_function(dialog->selected_destination()); });
    dialog->show();
}
void MainWindow::navigate_function(int index) {
    const auto &destinations = function_destinations();
    if (index < 0 || index >= destinations.size())
        return;
    const auto &destination = destinations[index];
    tabs_->setCurrentIndex(destination.workspace);
    if (destination.research_page >= 0)
        lab_->navigate_to(destination.research_page, destination.advanced);
    update_workspace();
}
void MainWindow::apply_text_size(int pixels) {
    pixels = std::clamp(pixels, 13, 18);
    auto style = base_style_;
    style.replace("font-size:13px", QStringLiteral("font-size:%1px").arg(pixels));
    setStyleSheet(style);
    if (table_)
        table_->verticalHeader()->setDefaultSectionSize(std::max(32, pixels + 20));
}
QWidget *MainWindow::workspace_preferences() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(
        ui::paragraph(QStringLiteral("Рабочее место • сохраняются только параметры интерфейса")));
    auto *form = new QFormLayout;
    text_size_ = new QSpinBox;
    text_size_->setRange(13, 18);
    text_size_->setValue(13);
    text_size_->setSuffix(QStringLiteral(" px"));
    text_size_->setToolTip(QStringLiteral("Размер основного текста и элементов управления. Заголовки и "
                                          "подписи внутри графиков имеют отдельные размеры."));
    form->addRow(QStringLiteral("Основной текст"), text_size_);
    column_preset_ = new QComboBox;
    column_preset_->addItems({QStringLiteral("Индивидуальный набор"), QStringLiteral("Все колонки"),
                              QStringLiteral("Обзор цены"), QStringLiteral("Спрос, предложение и спред")});
    column_preset_->setCurrentIndex(1);
    form->addRow(QStringLiteral("Вид таблицы котировок"), column_preset_);
    layout->addLayout(form);
    auto *grid = new QGridLayout;
    for (int column = 2; column <= 6; ++column) {
        auto *choice = new QCheckBox(table_->horizontalHeaderItem(column)->text());
        choice->setChecked(true);
        choice->setAccessibleName(QStringLiteral("Показать колонку %1").arg(choice->text()));
        column_choices_.push_back(choice);
        grid->addWidget(choice, (column - 2) / 3, (column - 2) % 3);
        connect(choice, &QCheckBox::toggled, this, [this, column](bool on) {
            table_->setColumnHidden(column, !on);
            const QSignalBlocker blocker(column_preset_);
            column_preset_->setCurrentIndex(0);
            save_settings();
        });
    }
    layout->addLayout(grid);
    layout->addWidget(ui::paragraph(QStringLiteral(
        "Тикер, цена, время данных и возраст ответа всегда видны. Скрытие колонок не меняет данные и "
        "расчёты.\nCtrl+K — поиск функций; Ctrl+1…5 — начало, котировки, исследование, задания, портфель.")));
    auto *reset = new QPushButton(QStringLiteral("Сбросить вид: текст 13 px и все колонки"));
    layout->addWidget(reset, 0, Qt::AlignLeft);
    connect(reset, &QPushButton::clicked, this, [this] {
        text_size_->setValue(13);
        apply_column_preset(1);
        save_settings();
    });
    connect(text_size_, &QSpinBox::valueChanged, this, [this](int pixels) {
        apply_text_size(pixels);
        save_settings();
    });
    connect(column_preset_, &QComboBox::activated, this, [this](int preset) {
        apply_column_preset(preset);
        save_settings();
    });
    return page;
}
void MainWindow::apply_column_preset(int preset) {
    if (preset < 1 || preset > 3)
        return;
    for (int i = 0; i < column_choices_.size(); ++i) {
        const bool visible = preset == 1 || (preset == 2 ? i == 3 : i < 3);
        const QSignalBlocker blocker(column_choices_[i]);
        column_choices_[i]->setChecked(visible);
        table_->setColumnHidden(i + 2, !visible);
    }
    const QSignalBlocker blocker(column_preset_);
    column_preset_->setCurrentIndex(preset);
}
void MainWindow::load_workspace_preferences() {
    int pixels = 13, hidden = 0;
    if (!test_mode_) {
        QSettings settings;
        pixels = std::clamp(settings.value("workspace/text_pixels", 13).toInt(), 13, 18);
        hidden = settings.value("workspace/hidden_columns", 0).toInt() & 0x7c;
    }
    const QSignalBlocker font_blocker(text_size_);
    text_size_->setValue(pixels);
    apply_text_size(pixels);
    for (int i = 0; i < column_choices_.size(); ++i) {
        const QSignalBlocker blocker(column_choices_[i]);
        const bool visible = !(hidden & (1 << (i + 2)));
        column_choices_[i]->setChecked(visible);
        table_->setColumnHidden(i + 2, !visible);
    }
    for (int column : {0, 1, 7, 8})
        table_->setColumnHidden(column, false);
    column_preset_->setCurrentIndex(hidden ? 0 : 1);
}
bool MainWindow::workspace_tools_test() {
    FunctionSearch dialog(this);
    if (dialog.results()->count() != function_destinations().size())
        return false;
    for (const auto &word :
         {QStringLiteral("  КоРРЕЛЯЦИИ  "), QStringLiteral("ATR"), QStringLiteral("walk-forward")}) {
        dialog.query()->setText(word);
        if (dialog.results()->count() != 1)
            return false;
        const auto index = dialog.selected_destination();
        navigate_function(index);
        if (tabs_->currentIndex() != function_destinations()[index].workspace)
            return false;
    }
    dialog.query()->setText(QStringLiteral("<html>UNKNOWN_QUERY_987654</html>"));
    if (dialog.selected_destination() != -1 || dialog.results()->count())
        return false;
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(dialog.query(), &enter);
    if (dialog.result() == QDialog::Accepted)
        return false;
    dialog.query()->clear();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(dialog.query(), &down);
    if (dialog.results()->currentRow() != 1)
        return false;
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(dialog.query(), &up);
    if (dialog.results()->currentRow() != 0)
        return false;
    dialog.query()->setText("ATR");
    QApplication::sendEvent(dialog.query(), &enter);
    if (dialog.result() != QDialog::Accepted)
        return false;
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &escape);
    if (dialog.result() != QDialog::Rejected)
        return false;
    show_function_search();
    auto *first_search = function_search_.data();
    show_function_search();
    if (function_search_.data() != first_search)
        return false;
    function_search_->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (function_search_)
        return false;
    for (int preset = 1; preset <= 3; ++preset) {
        apply_column_preset(preset);
        for (int column : {0, 1, 7, 8})
            if (table_->isColumnHidden(column))
                return false;
    }
    apply_column_preset(2);
    if (!table_->isColumnHidden(2) || table_->isColumnHidden(5))
        return false;
    column_choices_[0]->setChecked(true);
    if (table_->isColumnHidden(2) || column_preset_->currentIndex() != 0)
        return false;
    text_size_->setValue(18);
    if (!styleSheet().contains("font-size:18px"))
        return false;
    // Check navigation and the enlarged controls on the supported minimum window.
    resize(960, 640);
    for (int i = 0; i < function_destinations().size(); ++i)
        navigate_function(i);
    text_size_->setValue(13);
    apply_column_preset(1);
    lab_->navigate_to(6, false);
    tabs_->setCurrentIndex(8);
    return true;
}
} // namespace live
