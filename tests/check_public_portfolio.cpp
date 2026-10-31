#include "portfolio_account_file.hpp"
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
    std::shared_ptr<live::PortfolioAccountRestore> restored;
    live::PortfolioLiveFeed feed(
        nullptr, QUrl("https://iss.moex.com/iss/engines/stock/markets/shares/boards/TQBR/securities/"),
        [&] { return clock; });
    feed.on_fatal = [&](const QString &) {
        std::puts("Public MOEX two-symbol feed: FAIL (validated history)");
        app.exit(1);
    };
    feed.on_frames = [&](const auto &frames) {
        try {
            if (!restored || frames.empty())
                throw std::runtime_error("empty");
            for (const auto &f : frames) {
                restored->account->step(f);
                restored->baseline->step(f);
            }
            std::printf(
                "Public MOEX two-symbol feed: PASS, restored account new common frames=%zu, fills=%d\n",
                frames.size(), restored->account->result().fills);
            feed.stop();
            app.exit(0);
        } catch (const std::exception &) {
            feed.stop();
            std::puts("Public MOEX restored account: FAIL");
            app.exit(1);
        }
    };
    QObject::connect(&loader, &live::MarketHistory::failed, &app, [&](const QString &) {
        std::puts("Public MOEX bootstrap: unavailable");
        app.exit(1);
    });
    QObject::connect(
        &loader, &live::MarketHistory::loaded, &app,
        [&](const QString &, int, const QVector<live::Candle> &bars, bool) {
            for (const auto &c : bars)
                assets[index].bars.push_back(
                    {c.time_ms, c.time_ms + 3600000, c.open, c.high, c.low, c.close, c.volume,
                     QDateTime::fromMSecsSinceEpoch(c.time_ms, zone).offsetFromUtc()});
            if (++index < assets.size()) {
                loader.load_range(QString::fromStdString(assets[index].symbol), QDate(2024, 1, 8),
                                  QDate(2024, 1, 16), 60, true);
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
                live::PortfolioAccountSnapshot snapshot;
                snapshot.live = true;
                snapshot.interval = 60;
                snapshot.warmup_cutoff = snapshot.warmup_end = warm.end;
                snapshot.project.input.source =
                    "Public MOEX historical fixture; lot=1/tick=.01 are test assumptions";
                for (auto &a : assets) {
                    a.weight = .4;
                    a.tick_size = .01;
                    snapshot.project.enabled.push_back(true);
                    snapshot.project.modes.push_back(0);
                }
                snapshot.project.input.assets = assets;
                auto warm_schedule = quant::portfolio_schedule(assets);
                snapshot.excluded = warm_schedule.excluded;
                quant::PortfolioAccount account(assets, snapshot.project.config);
                auto baseline = quant::portfolio_baseline_account(assets, snapshot.project.config);
                for (const auto &f : warm_schedule.frames) {
                    account.warmup(f);
                    baseline.warmup(f);
                }
                account.arm_from_warmup();
                baseline.arm_from_warmup();
                snapshot.reference = account.result();
                snapshot.baseline_reference = baseline.result();
                snapshot.audit =
                    quant::PortfolioStreamBuffer(assets, warm.time, warm.end, begin, 60).snapshot();
                std::atomic<bool> cancel{false};
                auto bytes = live::encode_portfolio_account(snapshot, cancel);
                restored = std::make_shared<live::PortfolioAccountRestore>(
                    live::decode_portfolio_account(bytes, cancel));
                feed.start(std::make_unique<quant::PortfolioStreamBuffer>(
                               quant::PortfolioStreamBuffer::restore(*restored->snapshot.audit)),
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
