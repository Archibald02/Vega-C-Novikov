#pragma once
#include "market_history.hpp"
#include "quant/portfolio_stream.hpp"
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <memory>
namespace live {
/** Sequential public MOEX cycle, no disk or credentials. Base/clock injection is for fixture tests. */
class PortfolioLiveFeed : public QObject {
  public:
    explicit PortfolioLiveFeed(
        QObject *parent = nullptr,
        QUrl base = QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities/"),
        std::function<qint64()> clock = [] { return QDateTime::currentMSecsSinceEpoch(); });
    ~PortfolioLiveFeed() override { stop(); }
    void start(std::unique_ptr<quant::PortfolioStreamBuffer> buffer, int interval);
    void stop();
    void pause(bool paused);
    void refresh();
    bool paused() const { return paused_; }
    bool active() const { return active_; }
    std::function<void(const std::vector<std::vector<quant::Bar>> &)> on_frames;
    std::function<void(const QString &)> on_status, on_fatal;
    std::function<void(const quant::PortfolioStreamState &)> on_snapshot;

  private:
    void next();
    void failure(bool fatal);
    void schedule();
    MarketHistory loader_;
    QTimer timer_;
    QElapsedTimer cooldown_;
    std::function<qint64()> clock_;
    std::unique_ptr<quant::PortfolioStreamBuffer> buffer_;
    std::vector<std::vector<quant::Bar>> cycle_;
    int interval_ = 24;
    std::size_t index_ = 0;
    qint64 cutoff_ = 0, begin_ = 0;
    bool active_ = false, paused_ = false, loading_ = false;
    quint64 generation_ = 0;
};
} // namespace live
