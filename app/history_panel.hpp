#pragma once
#include "market_history.hpp"
#include "visual_chart.hpp"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
namespace live {
class HistoryPanel : public QWidget {
  public:
    explicit HistoryPanel(QWidget *parent = nullptr);
    void set_symbol(const QString &symbol);
    void clear_private_state();
    void show_test_bars(const QVector<Candle> &bars);

  private:
    void load(bool force);
    MarketHistory loader_;
    QString symbol_ = "SBER";
    QLabel *status_;
    QLabel *summary_;
    QComboBox *period_;
    QLineEdit *symbol_edit_;
    QPushButton *load_;
    VisualChart *chart_;
};
} // namespace live
