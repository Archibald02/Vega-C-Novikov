#pragma once
#include "portfolio_account_file.hpp"
#include "portfolio_input.hpp"
#include "portfolio_live_feed.hpp"
#include "portfolio_project.hpp"
#include "ui_helpers.hpp"
#include "visual_chart.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <memory>
namespace live {
struct PortfolioReplay {
    quant::PortfolioSchedule schedule;
    std::unique_ptr<quant::PortfolioAccount> account, baseline;
    QString source;
    std::size_t index = 0;
    bool failed = false;
    bool live = false;
    int interval = 0;
    std::unique_ptr<quant::PortfolioStreamBuffer> buffer;
    PortfolioAccountSnapshot checkpoint;
    bool restored = false;
};
struct PortfolioOutcome {
    quant::PortfolioResult result, baseline;
    QString source, error;
    std::shared_ptr<PortfolioReplay> replay;
};
struct PortfolioFileOutcome {
    PortfolioProject project;
    QString error;
    bool saved = false;
};
struct PortfolioAccountFileOutcome {
    QString error;
    bool saved = false;
    std::shared_ptr<PortfolioAccountRestore> restored;
};
/** Synchronous-bar, shared-cash educational portfolio research. No order API or
 * account access. */
class PortfolioPanel : public QWidget {
  public:
    explicit PortfolioPanel(QWidget *parent = nullptr);
    ~PortfolioPanel() override;
    QPushButton *import_button() const { return import_; }
    void set_input(PortfolioInput input);
    void notice(const QString &message) { status_->setText(message); }
    void clear_private_state();
    bool self_test();
    QSize minimumSizeHint() const override { return {640, 350}; }

  private:
    void demo();
    void invalidate();
    void equal_weights();
    void calculate(bool prepare_replay = false, bool prepare_live = false);
    void busy(bool on);
    void present(const PortfolioOutcome &outcome);
    PortfolioProject project() const;
    void apply_project(PortfolioProject project);
    void project_file(bool save, QString path = {});
    bool working() const;
    void replay_step();
    void replay_stop();
    void replay_end(bool notify = true);
    void replay_controls();
    void replay_present();
    void live_frames(const std::vector<std::vector<quant::Bar>> &frames);
    void live_failure(const QString &message);
    void record_stop(const std::string &reason);
    void account_file(bool save, QString path = {});
    void account_resume();
    PortfolioAccountSnapshot account_snapshot() const;
    bool account_self_test();
    void setup_account_files();
    bool account_io_ = false;
    struct AssetRow {
        QCheckBox *enabled;
        QComboBox *rules;
        QDoubleSpinBox *weight, *tick;
        QSpinBox *lot;
        QLineEdit *group;
    };
    std::vector<AssetRow> rows_;
    PortfolioInput input_;
    QTableWidget *assets_, *positions_, *journal_;
    QPushButton *import_, *demo_, *equal_, *run_, *cancel_, *save_, *open_;
    QPushButton *replay_start_, *replay_step_, *replay_play_, *replay_stop_, *replay_end_;
    QPushButton *live_start_;
    QPushButton *account_save_, *account_open_, *account_resume_;
    QCheckBox *account_confirm_;
    QLabel *replay_status_;
    QLabel *source_, *status_, *summary_;
    QDoubleSpinBox *capital_, *invested_, *weight_limit_, *group_limit_, *commission_, *fixed_, *spread_,
        *slippage_, *drawdown_, *daily_;
    QSpinBox *period_, *fast_, *slow_, *manual_stop_;
    QCheckBox *manual_metadata_;
    VisualChart *equity_, *drawdowns_;
    ui::WheelGuard *guard_;
    QFutureWatcher<PortfolioOutcome> *worker_;
    QFutureWatcher<PortfolioFileOutcome> *file_worker_;
    QFutureWatcher<PortfolioAccountFileOutcome> *account_worker_ = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<PortfolioReplay> replay_;
    QTimer replay_timer_;
    std::unique_ptr<PortfolioLiveFeed> live_feed_;
};
} // namespace live
