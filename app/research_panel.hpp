#pragma once
#include "market_history.hpp"
#include "quant/research.hpp"
#include "visual_chart.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <atomic>
#include <map>
#include <memory>
namespace live {
struct LabResult {
    quant::Distribution distribution;
    quant::Stationarity stationarity;
    quant::Correlation correlation;
    std::vector<double> acf, pacf;
    std::vector<quant::Features> features;
    quant::SimResult simulation, baseline;
    quant::WalkForward walk;
    QString error;
};
/** Standalone bounded research workspace. All broker interactions remain read-only. */
class ResearchPanel : public QWidget {
  public:
    explicit ResearchPanel(QWidget *parent = nullptr);
    ~ResearchPanel() override;
    void set_symbol(const QString &symbol);
    void set_feed_quality(const QString &text);
    void clear_private_state();
    bool self_test();
    bool privacy_test();
    void start_online_test();
    bool online_ready();
    void set_test_view(int index);

  private:
    struct RuleUi {
        QCheckBox *enabled;
        QComboBox *lhs, *op, *rhs;
        QDoubleSpinBox *value;
    };
    QWidget *analytics_page();
    QWidget *strategy_page();
    QWidget *report_page();
    QWidget *paper_page();
    QWidget *rules_box(const QString &title, std::vector<RuleUi> &controls, QCheckBox *&all, bool entry);
    quant::SimConfig settings() const;
    void load_next();
    void calculate();
    void present(const LabResult &result);
    void show_distribution();
    void demo();
    void start_paper();
    void step_paper();
    void update_paper();
    void stop_paper(const QString &reason);
    void cancel_work();
    std::vector<quant::Bar> selected_bars() const;
    QLineEdit *symbols_;
    QComboBox *period_, *selected_, *distribution_mode_, *ambiguity_;
    QSpinBox *window_, *lags_, *adf_lags_, *kpss_lags_, *fast_, *slow_, *lot_, *annual_, *train_, *test_,
        *max_entries_;
    QDoubleSpinBox *cash_, *commission_, *fixed_, *spread_, *slippage_, *allocation_, *stop_, *target_,
        *risk_, *max_dd_, *daily_, *participation_;
    QCheckBox *entry_all_, *exit_all_;
    std::vector<RuleUi> entry_rules_, exit_rules_;
    QPushButton *calculate_, *load_;
    QLabel *status_, *quality_, *live_quality_, *statistics_, *tests_, *feature_text_, *report_,
        *paper_status_;
    QTableWidget *correlations_, *folds_, *journal_, *features_table_, *backtest_journal_;
    QTabWidget *tabs_;
    VisualChart *distribution_chart_, *acf_chart_, *price_chart_, *equity_chart_, *drawdown_chart_,
        *paper_chart_;
    MarketHistory loader_;
    QFutureWatcher<LabResult> *worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QTimer paper_timer_;
    std::map<QString, std::vector<quant::Bar>> data_;
    QStringList requested_;
    QStringList diagnostics_;
    int request_index_ = 0;
    bool loading_ = false, synthetic_ = false;
    std::unique_ptr<quant::PaperEngine> paper_;
    std::vector<quant::Bar> paper_bars_;
    std::size_t paper_index_ = 0;
    LabResult last_result_;
    QString paper_source_;
};
} // namespace live
