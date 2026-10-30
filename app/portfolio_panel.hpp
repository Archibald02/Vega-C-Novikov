#pragma once
#include "portfolio_input.hpp"
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
    void calculate(bool prepare_replay = false);
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
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<PortfolioReplay> replay_;
    QTimer replay_timer_;
};
} // namespace live
