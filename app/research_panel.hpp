#pragma once
#include "instrument_catalog.hpp"
#include "market_history.hpp"
#include "portfolio_input.hpp"
#include "quant/research.hpp"
#include "quant/robustness.hpp"
#include "research_snapshot.hpp"
#include "visual_chart.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <map>
#include <memory>
namespace live {
/** Localized model codes shared by the GUI and explicitly exported reports. */
QString execution_gate(const std::string &gate);
QString execution_reason(const std::string &code);
struct LabResult {
    std::shared_ptr<const ReplaySnapshot> snapshot;
    quant::Strategy explained_strategy;
    quant::SimConfig explained_config;
    quant::Distribution distribution;
    quant::Stationarity stationarity;
    quant::Correlation correlation;
    std::vector<double> acf, pacf;
    std::vector<quant::Features> features;
    quant::SimResult simulation, baseline;
    quant::WalkForward walk;
    quant::Robustness robustness;
    quant::HoldoutResult holdout;
    bool has_holdout = false;
    QJsonObject parameters;
    QString source, hash;
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
    PortfolioInput portfolio_input() const;
    bool privacy_test();
    void start_online_test();
    bool online_ready();
    void set_test_view(int index);
    /** Navigate without loading data or recalculating; optionally reveal expert analytics. */
    void navigate_to(int index, bool advanced = false);
    void show_demo();
    QSize sizeHint() const override { return {1000, 600}; }
    QSize minimumSizeHint() const override { return {640, 350}; }
    int heightForWidth(int) const override { return minimumSizeHint().height(); }

  private:
    void update_journey();
    void explain_bar(int index);
    QSpinBox *explanation_bar_ = nullptr;
    QSpinBox *entry_confirmation_, *exit_confirmation_, *cooldown_bars_, *max_holding_bars_, *signal_delay_;
    QLabel *explanation_ = nullptr;
    void next_step();
    bool journey_test();
    QWidget *data_page_, *project_page_, *about_page_, *expert_controls_, *analytics_expert_;
    QVBoxLayout *data_layout_, *quality_layout_;
    QListWidget *journey_;
    QLabel *dataset_context_, *step_title_, *step_hint_, *result_context_, *result_banner_;
    QLabel *return_card_, *drawdown_card_, *fills_card_, *cost_card_;
    QPushButton *next_ = nullptr, *back_ = nullptr, *cancel_button_ = nullptr;
    bool result_current_ = false, show_result_when_ready_ = false, source_dirty_ = false;
    QWidget *workbench_page();
    void setup_workbench();
    void preflight();
    void update_metadata();
    QJsonObject configuration() const;
    bool apply_configuration(const QJsonObject &object, bool validate_only = false);
    quant::SimConfig configuration_settings(const QJsonObject &object) const;
    bool save_project(const QString &path, bool include_account = false);
    bool open_project(const QString &path);
    QString fingerprint(const std::vector<quant::Bar> &bars) const;
    QString report_html(bool compact = false) const;
    void export_report(bool pdf);
    void record_run(const LabResult &result);
    void refresh_runs();
    void setup_experiments(QVBoxLayout *layout);
    bool save_experiments(const QString &path) const;
    bool open_experiments(const QString &path);
    bool experiments_test();
    void compare_experiments();
    void snapshot_operation(bool save);
    QFutureWatcher<QString> *snapshot_worker_ = nullptr;
    std::shared_ptr<std::atomic<bool>> snapshot_cancel_;
    QLabel *experiment_detail_ = nullptr;
    void refresh_paper();
    void start_live_paper();
    void fetch_reference();
    bool workbench_test();
    QDateEdit *from_, *till_;
    QCheckBox *custom_dates_, *reserve_, *expert_, *save_account_, *save_cash_, *manual_metadata_;
    QSpinBox *gap_;
    QDoubleSpinBox *tick_;
    QLabel *preflight_, *holdout_status_, *metadata_, *reference_status_, *release_status_;
    QTableWidget *stability_, *stress_, *runs_table_, *events_;
    VisualChart *runs_chart_;
    InstrumentCatalog catalog_;
    MarketHistory live_loader_,
        reference_loader_{nullptr, QUrl("https://iss.moex.com/iss/engines/stock/markets/index/securities/")};
    QNetworkAccessManager study_network_;
    QPointer<QNetworkReply> dividend_reply_, release_reply_;
    QTimer live_timer_;
    QString live_symbol_;
    QJsonObject paper_parameters_;
    bool local_project_ = false, exporting_ = false;
    int live_interval_ = 24;
    qint64 live_last_ = 0;
    bool live_paper_ = false, evaluate_holdout_ = false;
    quint64 study_generation_ = 0;
    int paper_manual_stop_ = -1;
    int attempts_ = 0;
    QString holdout_key_;
    struct SavedRun {
        QString label, hash, source;
        QJsonObject parameters;
        quant::Metrics metrics;
        std::vector<double> equity;
        double oos = quant::unavailable();
        QString hypothesis;
        bool imported = false;
        std::shared_ptr<const ReplaySnapshot> snapshot;
    };
    std::vector<SavedRun> runs_;
    std::map<QString, bool> holdout_used_;
    struct RuleUi {
        QCheckBox *enabled;
        QComboBox *lhs, *op, *rhs;
        QDoubleSpinBox *value;
        QComboBox *join = nullptr, *open = nullptr, *close = nullptr;
    };
    QWidget *analytics_page();
    QWidget *strategy_page();
    QWidget *report_page();
    QWidget *paper_page();
    QWidget *rules_box(const QString &title, std::vector<RuleUi> &controls, QCheckBox *&all,
                       QComboBox *&expression_mode, bool entry);
    quant::RuleGroup ui_rules(const std::vector<RuleUi> &controls, bool all, bool custom) const;
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
    QComboBox *entry_expression_, *exit_expression_;
    std::vector<RuleUi> entry_rules_, exit_rules_;
    QPushButton *calculate_, *load_;
    QPushButton *paper_play_ = nullptr, *paper_step_ = nullptr;
    QLabel *status_, *quality_, *live_quality_, *statistics_, *tests_, *feature_text_, *report_,
        *paper_status_;
    QTableWidget *correlations_, *folds_, *journal_, *features_table_, *backtest_journal_;
    QTabWidget *tabs_;
    VisualChart *distribution_chart_, *acf_chart_, *price_chart_, *equity_chart_, *drawdown_chart_,
        *paper_chart_;
    MarketHistory loader_;
    QFutureWatcher<LabResult> *worker_ = nullptr;
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
