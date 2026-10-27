#include "catalog_dialog.hpp"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPushButton>
#include <QVBoxLayout>
namespace live {
CatalogDialog::CatalogDialog(InstrumentCatalog& catalog,const QStringList& selected,QWidget* parent)
    :QDialog(parent),catalog_(catalog),checked_(selected.begin(),selected.end()) {
    setWindowTitle(QStringLiteral("Каталог акций • MOEX TQBR"));resize(700,620);
    auto* layout=new QVBoxLayout(this);
    auto* note=new QLabel(QStringLiteral("Действующие инструменты из каталога MOEX. Доступность в АЛОР зависит от аккаунта.\nОтметьте до 20 акций. Поиск не сбрасывает выбор."));note->setWordWrap(true);layout->addWidget(note);
    search_=new QLineEdit;search_->setPlaceholderText(QStringLiteral("Тикер или название: SBER, Сбер, Газпром…"));layout->addWidget(search_);
    status_=new QLabel(QStringLiteral("Загрузка каталога…"));layout->addWidget(status_);
    list_=new QListWidget;list_->setSelectionMode(QAbstractItemView::NoSelection);layout->addWidget(list_,1);
    auto* tools=new QHBoxLayout;count_=new QLabel;tools->addWidget(count_);tools->addStretch();
    auto* clear=new QPushButton(QStringLiteral("Снять выбор"));tools->addWidget(clear);
    auto* refresh=new QPushButton(QStringLiteral("Обновить с MOEX"));tools->addWidget(refresh);layout->addLayout(tools);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    connect(search_,&QLineEdit::textChanged,this,[this]{filter();});
    connect(list_,&QListWidget::itemChanged,this,[this](QListWidgetItem* item){
        const auto symbol=item->data(Qt::UserRole).toString();
        if(item->checkState()==Qt::Checked) checked_.insert(symbol); else checked_.remove(symbol);
        update_count();
    });
    connect(clear,&QPushButton::clicked,this,[this]{checked_.clear();populate();});
    connect(refresh,&QPushButton::clicked,this,[this]{status_->setText(QStringLiteral("Обновление…"));catalog_.load(true);});
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{
        if(checked_.isEmpty() || checked_.size()>20) {QMessageBox::warning(this,QStringLiteral("Выбор"),QStringLiteral("Нужно выбрать от 1 до 20 инструментов."));return;}accept();
    });
    connect(&catalog_,&InstrumentCatalog::updated,this,[this]{populate();status_->setText(QStringLiteral("Доступно в каталоге: %1 • кэш в памяти 15 минут").arg(catalog_.instruments().size()));});
    connect(&catalog_,&InstrumentCatalog::failed,this,[this](const QString& reason){status_->setText(reason);});
    populate();catalog_.load();
}
void CatalogDialog::populate() {
    const QSignalBlocker blocked(list_);list_->clear();QSet<QString> added;
    for(const auto& instrument:catalog_.instruments()) {
        auto* item=new QListWidgetItem(QStringLiteral("%1  —  %2  ·  лот %3").arg(instrument.symbol,instrument.name).arg(instrument.lot_size),list_);
        item->setData(Qt::UserRole,instrument.symbol);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);
        item->setCheckState(checked_.contains(instrument.symbol)?Qt::Checked:Qt::Unchecked);added.insert(instrument.symbol);
    }
    for(const auto& symbol:checked_) if(!added.contains(symbol)) {
        auto* item=new QListWidgetItem(symbol+QStringLiteral(" — в текущем списке; каталог ещё не подтвердил доступность"),list_);
        item->setData(Qt::UserRole,symbol);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(Qt::Checked);
    }
    filter();update_count();
}
void CatalogDialog::filter() {
    const auto term=search_->text().trimmed();
    for(int i=0;i<list_->count();++i) list_->item(i)->setHidden(!list_->item(i)->text().contains(term,Qt::CaseInsensitive));
}
void CatalogDialog::update_count() {count_->setText(QStringLiteral("Выбрано: %1 / 20").arg(checked_.size()));}
QStringList CatalogDialog::selection() const {auto values=checked_.values();values.sort();return values;}
}
