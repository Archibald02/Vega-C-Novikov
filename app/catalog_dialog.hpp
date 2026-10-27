#pragma once
#include "instrument_catalog.hpp"
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSet>
namespace live {
/** Searchable multi-select board catalog. It does not send trading instructions. */
class CatalogDialog : public QDialog {
public:
    CatalogDialog(InstrumentCatalog& catalog,const QStringList& selected,QWidget* parent=nullptr);
    QStringList selection() const;
private:
    void populate();
    void filter();
    void update_count();
    InstrumentCatalog& catalog_;
    QSet<QString> checked_;
    QLineEdit* search_;
    QListWidget* list_;
    QLabel *status_,*count_;
};
}
