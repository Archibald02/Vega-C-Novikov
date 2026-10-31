#pragma once
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVector>
namespace live {
/** Navigation only: entries never execute a calculation, change a rule or start a feed. */
struct FunctionDestination {
    QString title, detail, keywords;
    int workspace, research_page = -1;
    bool advanced = false;
};
const QVector<FunctionDestination> &function_destinations();
QVector<int> find_functions(const QString &query);
/** Bounded, local search. Query and navigation history are never persisted. */
class FunctionSearch : public QDialog {
  public:
    explicit FunctionSearch(QWidget *parent = nullptr);
    int selected_destination() const;
    QLineEdit *query() const { return query_; }
    QListWidget *results() const { return results_; }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    void filter(const QString &query);
    QLineEdit *query_;
    QListWidget *results_;
    QLabel *count_;
};
} // namespace live
