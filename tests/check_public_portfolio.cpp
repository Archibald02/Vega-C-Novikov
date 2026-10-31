#include "portfolio_live_feed.hpp"
#include <QCoreApplication>
#include <QTimeZone>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QTimeZone zone("Europe/Moscow");
    live::MarketHistory loader;
    std::vector<quant::PortfolioAsset> assets(2);
    assets[0].symbol = "SBER";
    assets[1].symbol = "GAZP";
    std::size_t index = 0;
    qint64 clock = 0;
    live::PortfolioLiveFeed feed(
        nullptr, QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities/"),
        [&] { return clock; });
    feed.on_fatal = [&](const QString &) {
        std::puts("Public MOEX two-symbol feed: FAIL (validated history)");
        app.exit(1);
    };
    feed.on_frames = [&](const auto &frames) {
        std::printf("Public MOEX two-symbol feed: PASS, new common closed frames=%zu\n", frames.size());
        feed.stop();
        app.exit(frames.empty() ? 1 : 0);
    };
    QObject::connect(&loader, &live::MarketHistory::failed, &app, [&](const QString &) {
        std::puts("Public MOEX bootstrap: unavailable");
        app.exit(1);
    });
    QObject::connect(&loader, &live::MarketHistory::loaded, &app,
                     [&](const QString &, int, const QVector<live::Candle> &bars, bool) {
                         for (const auto &c : bars)
                             assets[index].bars.push_back(
                                 {c.time_ms, c.time_ms + 3600000, c.open, c.high, c.low, c.close, c.volume,
                                  QDateTime::fromMSecsSinceEpoch(c.time_ms, zone).offsetFromUtc()});
                         if (++index < assets.size()) {
                             loader.load_range(QString::fromStdString(assets[index].symbol),
                                               QDate(2024, 1, 8), QDate(2024, 1, 16), 60, true);
                             return;
                         }
                         try {
                             const auto schedule = quant::portfolio_schedule(assets);
                             if (schedule.frames.size() < 41)
                                 throw std::runtime_error("coverage");
                             const auto warm = schedule.frames[schedule.frames.size() - 2][0];
                             clock = schedule.frames.back()[0].end + 20 * 60000;
                             for (auto &a : assets)
                                 a.bars.erase(std::remove_if(a.bars.begin(), a.bars.end(),
                                                             [&](const auto &b) { return b.end > warm.end; }),
                                              a.bars.end());
                             const auto begin = QDateTime::fromMSecsSinceEpoch(warm.time, zone)
                                                    .date()
                                                    .addDays(-1)
                                                    .startOfDay(zone)
                                                    .toMSecsSinceEpoch();
                             feed.start(std::make_unique<quant::PortfolioStreamBuffer>(assets, warm.time,
                                                                                       warm.end, begin, 60),
                                        60);
                         } catch (const std::exception &) {
                             std::puts("Public MOEX bootstrap: insufficient compatible history");
                             app.exit(1);
                         }
                     });
    QTimer::singleShot(0, &app,
                       [&] { loader.load_range("SBER", QDate(2024, 1, 8), QDate(2024, 1, 16), 60, true); });
    QTimer::singleShot(55000, &app, [&] {
        std::puts("Public MOEX two-symbol feed: timeout");
        app.exit(1);
    });
    return app.exec();
}
