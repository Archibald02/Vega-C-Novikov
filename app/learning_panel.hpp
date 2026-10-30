#pragma once
#include "quant/learning.hpp"
#include "visual_chart.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <atomic>
#include <memory>
namespace live {
struct LessonOutcome {
    quant::LessonResult data;
    int kind = 0, answer = 0;
    double input = 0;
    QString error;
};
/** Standalone, offline synthetic lessons. Never changes the user's research or market subscription. */
class LearningPanel : public QWidget {
  public:
    explicit LearningPanel(QWidget *parent = nullptr);
    ~LearningPanel() override;
    bool self_test();
    QSize minimumSizeHint() const override { return {600, 350}; }

  private:
    void change_lesson();
    void invalidate();
    void run();
    void show_result(const LessonOutcome &result);
    void busy(bool on);
    QComboBox *lesson_, *answer_;
    QDoubleSpinBox *input_;
    QLabel *question_, *input_label_, *summary_, *explanation_;
    QPushButton *run_, *cancel_;
    VisualChart *chart_;
    QFutureWatcher<LessonOutcome> *worker_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
};
} // namespace live
