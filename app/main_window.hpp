#pragma once
#include "chart_widget.hpp"
#include "market_feed.hpp"
#include "quant/rolling.hpp"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <deque>
#include <map>
namespace live {
/** Desktop monitor: asynchronous feed, rolling statistics and chart. */
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(bool test_mode = false);
    void start_demo_test();
    bool has_demo_data() const;
    bool has_any_data() const;
    void start_moex_test();
private:
    struct Row {
        explicit Row(std::size_t window) : stats(window) {}
        quant::RollingSeries stats;
        Quote quote;
        qint64 received = 0;
        std::deque<QPointF> points;
    };
    void connect_feed();
    void receive(const Quote& q);
    void render();
    void save_settings();
    void set_controls(bool connected);
    MarketFeed feed_;
    QComboBox* provider_;
    QLineEdit *symbols_, *token_;
    QSpinBox *window_, *frequency_;
    QPushButton *start_, *stop_;
    QLabel *banner_, *connection_, *metric_price_, *metric_sma_, *metric_vol_, *metric_count_;
    QTableWidget* table_;
    ChartWidget* chart_;
    QTimer render_timer_;
    QStringList tickers_;
    std::map<QString, Row> rows_;
    bool test_mode_ = false;
};
}
