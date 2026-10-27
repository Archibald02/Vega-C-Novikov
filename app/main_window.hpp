#pragma once
#include "chart_widget.hpp"
#include "instrument_catalog.hpp"
#include "market_feed.hpp"
#include "history_panel.hpp"
#include "visual_chart.hpp"
#include "resource_monitor.hpp"
#include "research_panel.hpp"
#include "quant/rolling.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <map>
namespace live {
/** Bounded live monitor. Heavy selected-instrument views are event-driven. */
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(bool test_mode=false);
    ~MainWindow() override { feed_.disconnect(this); feed_.stop(); }
    void start_demo_test(int symbols=4);
    void start_moex_test();
    void start_soak_test();
    bool soak_ready() const;
    bool exercise_visuals_test();
    bool exercise_lab_test();
    void start_lab_online_test(){tabs_->setCurrentIndex(7);lab_->start_online_test();}
    bool lab_online_ready(){return lab_->online_ready();}
    void select_lab_test_view(int index){lab_->set_test_view(index);}
    bool exercise_privacy_test();
    void select_visual_test(int index);
    bool has_demo_data() const;
    bool has_any_data() const;
    bool exercise_tools_test();
    bool stress_test();
    QString performance_status() const;
    void stop_for_test();
    std::uint64_t chart_rebuilds() const {return chart_->rebuild_count();}
private:
    struct Row {
        Row(std::size_t window,std::size_t history) : stats(window),indicators(window),points(history) {}
        quant::RollingSeries stats;
        quant::StreamingIndicators indicators;
        ChartSeries points;
        Quote quote;
        qint64 received=0;
        std::uint64_t revision=0;
        bool dirty=true,alert_armed=false,alert_above=true;
        double alert_level=0;
    };
    void connect_feed();
    void receive(const Quote& q);
    void render();
    void maintenance();
    void schedule_render();
    void selection_changed();
    void update_forecast();
    void calculate_risk();
    void save_settings();
    void set_controls(bool connected);
    Row* selected_row();
    QString selected_symbol() const;
    QWidget* forecast_panel();
    QWidget* risk_panel();
    QWidget* help_panel();
    QWidget* performance_panel();
    QWidget* comparison_panel();
    void update_comparison();
    void reset_local_data();
    void arm_alert();
    void show_catalog();
    MarketFeed feed_;
    InstrumentCatalog catalog_;
    ResourceMonitor resources_;
    QComboBox *provider_,*performance_,*history_,*oscillator_,*forecast_model_,*alert_direction_;
    QLineEdit *symbols_,*token_;
    QSpinBox *window_,*frequency_,*poll_seconds_,*lookback_,*horizon_,*lot_;
    QDoubleSpinBox *capital_,*risk_,*entry_,*stop_price_,*target_,*alert_level_;
    QCheckBox *show_sma_,*show_ema_,*show_bands_,*forecast_enabled_;
    QPushButton *start_,*stop_,*catalog_button_;
    QLabel *banner_,*connection_,*metric_price_,*metric_sma_,*metric_vol_,*metric_count_,*indicator_text_,*resource_text_,*forecast_text_,*risk_text_,*alert_text_;
    QTableWidget* table_;
    QTabWidget* tabs_;
    ChartWidget* chart_;
    HistoryPanel* candles_;
    ResearchPanel* lab_;
    quint64 quality_rx_=0,quality_dup_=0,quality_old_=0,quality_invalid_=0,quality_network_=0,quality_reconnect_=0;
    VisualChart *comparison_, *forecast_chart_;
    QLabel* token_label_;
    bool comparison_dirty_ = true;
    qint64 session_start_ms_ = 0;
    QTimer render_timer_,maintenance_timer_;
    QStringList tickers_;
    std::map<QString,Row> rows_;
    QString forecast_symbol_;
    std::uint64_t forecast_revision_=0;
    bool forecast_dirty_=true,test_mode_=false;
    int redraw_ms_=500;
};
}
