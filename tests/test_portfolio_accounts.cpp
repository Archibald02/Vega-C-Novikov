#include "portfolio_account_file.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <doctest/doctest.h>
namespace {
constexpr qint64 epoch = 1700000000000LL, hour = 3600000;
quant::Bar bar(int i) {
    double p = 100 + i % 5;
    return {epoch + i * hour, epoch + (i + 1) * hour, p, p, p, p, 10000, 10800};
}
live::PortfolioAccountSnapshot example(bool stream = true, int count = 2, bool stop = true) {
    live::PortfolioAccountSnapshot s;
    s.project.input.source = "Fixture account, unverified";
    s.project.input.synthetic = !stream;
    s.project.config.initial_cash = 100000;
    for (const auto &symbol : {"A", "B"}) {
        quant::PortfolioAsset a;
        a.symbol = symbol;
        a.tick_size = .01;
        a.weight = .4;
        for (int i = 0; i < 80; ++i)
            a.bars.push_back(bar(i));
        s.project.input.assets.push_back(a);
        s.project.enabled.push_back(true);
        s.project.modes.push_back(0);
    }
    auto assets = live::portfolio_project_assets(s.project);
    quant::PortfolioAccount a(assets, s.project.config);
    auto base = quant::portfolio_baseline_account(assets, s.project.config);
    auto timeline = quant::portfolio_schedule(assets);
    s.live = stream;
    s.interval = stream ? 60 : 0;
    s.index = count;
    s.excluded = timeline.excluded;
    if (stream) {
        s.warmup_cutoff = s.warmup_end = bar(39).end;
        auto seed = assets;
        for (auto &v : seed)
            v.bars.resize(40);
        auto warm = quant::portfolio_schedule(seed);
        s.excluded = warm.excluded;
        for (const auto &f : warm.frames) {
            a.warmup(f);
            base.warmup(f);
        }
        a.arm_from_warmup();
        base.arm_from_warmup();
        quant::PortfolioStreamBuffer audit(seed, bar(39).time, bar(39).end, epoch, 60);
        std::vector<std::vector<quant::Bar>> cycle(2);
        for (auto &v : cycle)
            for (int i = 0; i < 40 + count; ++i)
                v.push_back(bar(i));
        s.frames = audit.merge(cycle, epoch, bar(39 + count).end);
        s.audit = audit.snapshot();
    }
    for (int i = 0; i < count; ++i) {
        const auto &f = stream ? s.frames[i] : timeline.frames[i];
        a.step(f);
        base.step(f);
    }
    if (stop) {
        s.stop_after = count;
        s.stop_reason = "MANUAL_STOP";
        a.request_stop();
    }
    s.reference = a.result();
    s.baseline_reference = base.result();
    return s;
}
QByteArray repack(QJsonObject o, QJsonObject data) {
    o["data"] = data;
    o["sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(data).toJson(QJsonDocument::Compact),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
} // namespace
TEST_CASE("Account checkpoint restores deferred exits, cash, fees and full baseline offline") {
    std::atomic<bool> cancel{false};
    auto s = example();
    auto bytes = live::encode_portfolio_account(s, cancel);
    auto restored = live::decode_portfolio_account(bytes, cancel);
    CHECK(restored.account->result().equity == s.reference.equity);
    CHECK(restored.baseline->result().equity == s.baseline_reference.equity);
    CHECK(restored.account->halted());
    CHECK(restored.account->result().positions[0].units > 0);
    CHECK_FALSE(restored.snapshot.project.input.moex_live);
    CHECK(live::encode_portfolio_account(restored.snapshot, cancel) == bytes);
    auto uninterrupted = live::replay_portfolio_account(s, cancel);
    std::vector<quant::Bar> next{bar(42), bar(42)};
    restored.account->step(next);
    uninterrupted.account->step(next);
    CHECK(restored.account->result().equity == uninterrupted.account->result().equity);
    CHECK(restored.account->result().positions[0].units == 0);
    CHECK(restored.account->result().fees == uninterrupted.account->result().fees);
    CHECK(restored.account->result().journal.back().reason == "RISK_EXIT");
    CHECK_FALSE(bytes.contains("password"));
    CHECK_FALSE(bytes.contains("token"));
}
TEST_CASE("Account checkpoint before first fill and archive replay preserve pending targets") {
    std::atomic<bool> c{false};
    auto live = example(true, 0, false);
    auto restored = live::decode_portfolio_account(live::encode_portfolio_account(live, c), c);
    CHECK(restored.account->result().fills == 0);
    restored.account->step({bar(40), bar(40)});
    CHECK(restored.account->result().fills == 2);
    auto history = example(false, 2, false);
    auto archive = live::decode_portfolio_account(live::encode_portfolio_account(history, c), c);
    CHECK(archive.schedule.frames.size() == 80);
    CHECK(archive.snapshot.index == 2);
    archive.account->step(archive.schedule.frames[2]);
    auto full = example(false, 3, false);
    CHECK(archive.account->result().equity == full.reference.equity);
}
TEST_CASE("Account checkpoint rejects altered references, unsupported model and malformed inputs") {
    std::atomic<bool> c{false};
    auto bytes = live::encode_portfolio_account(example(), c);
    const auto original = QJsonDocument::fromJson(bytes).object();
    SUBCASE("model") {
        auto o = original;
        o["model"] = "future";
        CHECK_THROWS(live::decode_portfolio_account(QJsonDocument(o).toJson(), c));
    }
    SUBCASE("cash even after rehash") {
        auto data = original["data"].toObject(), refs = data["reference"].toObject(),
             a = refs["account"].toObject();
        a["cash"] = a["cash"].toDouble() + 1;
        refs["account"] = a;
        data["reference"] = refs;
        CHECK_THROWS(live::decode_portfolio_account(repack(original, data), c));
    }
    SUBCASE("unknown field") {
        auto data = original["data"].toObject();
        data["connect_token"] = "forbidden";
        CHECK_THROWS(live::decode_portfolio_account(repack(original, data), c));
    }
    SUBCASE("audit differs from accepted prices") {
        auto data = original["data"].toObject();
        auto audit = data["audit"].toObject();
        auto rows = audit["series"].toArray();
        auto a = rows[0].toArray();
        auto b = a[a.size() - 1].toArray();
        b[6] = b[6].toDouble() + 1;
        a[a.size() - 1] = b;
        rows[0] = a;
        audit["series"] = rows;
        data["audit"] = audit;
        CHECK_THROWS(live::decode_portfolio_account(repack(original, data), c));
    }
    SUBCASE("future command") {
        auto data = original["data"].toObject();
        data["stop_after"] = 3;
        CHECK_THROWS(live::decode_portfolio_account(repack(original, data), c));
    }
    SUBCASE("bad OHLC") {
        auto data = original["data"].toObject();
        auto frames = data["frames"].toArray();
        auto f = frames[0].toArray();
        auto b = f[0].toArray();
        b[4] = 1e9;
        f[0] = b;
        frames[0] = f;
        data["frames"] = frames;
        CHECK_THROWS(live::decode_portfolio_account(repack(original, data), c));
    }
    CHECK_THROWS(live::decode_portfolio_account(bytes.left(bytes.size() / 2), c));
    c = true;
    CHECK_THROWS(live::decode_portfolio_account(bytes, c));
}
TEST_CASE("Audit checkpoint retains pending unmatched data and catches revisions after restore") {
    auto s = example(true, 0, false);
    auto b = quant::PortfolioStreamBuffer::restore(*s.audit);
    auto known = *s.audit;
    auto cycle = known.series;
    cycle[0].push_back(bar(40));
    CHECK(b.merge(cycle, epoch, bar(40).end).empty());
    auto copy = quant::PortfolioStreamBuffer::restore(b.snapshot());
    cycle[1].push_back(bar(40));
    auto a = b.merge(cycle, epoch, bar(40).end), r = copy.merge(cycle, epoch, bar(40).end);
    REQUIRE(a.size() == 1);
    CHECK(r.size() == 1);
    CHECK(r[0][0].time == a[0][0].time);
    cycle[0].back().volume += 1;
    CHECK_THROWS(copy.merge(cycle, epoch, bar(40).end));
    auto bad = s.audit.value();
    bad.series[0].pop_back();
    CHECK_THROWS(quant::PortfolioStreamBuffer::restore(bad));
}
TEST_CASE("Account checkpoint explicit atomic IO, limits and cancellation preserve prior files") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    std::atomic<bool> c{false};
    auto s = example();
    auto path = dir.filePath("account.qpa.json");
    live::write_portfolio_account(path, s, c);
    CHECK(live::read_portfolio_account(path, c).snapshot.index == 2);
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    auto bytes = f.readAll();
    f.close();
    c = true;
    CHECK_THROWS(live::write_portfolio_account(path, s, c));
    c = false;
    s.reference.cash += 1;
    CHECK_THROWS(live::write_portfolio_account(path, s, c));
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll() == bytes);
    f.close();
    CHECK_THROWS(live::decode_portfolio_account(QByteArray(live::portfolio_account_byte_limit + 1, ' '), c));
    CHECK_THROWS(live::read_portfolio_account(dir.path(), c));
    CHECK_THROWS(live::write_portfolio_account(dir.filePath("absent/x"), example(), c));
    auto oversized = example();
    oversized.frames.resize(5001);
    CHECK_THROWS(live::encode_portfolio_account(oversized, c));
}
TEST_CASE("Account checkpoint maximum warmup and trading histories preserve attribution and caps") {
    std::atomic<bool> c{false};
    live::PortfolioAccountSnapshot s;
    s.live = true;
    s.interval = 60;
    s.index = 5000;
    s.project.input.source = "Maximum bounded fixture";
    s.project.config.commission_rate = .001;
    s.project.config.spread_bps = s.project.config.slippage_bps = 0;
    s.project.config.max_drawdown = s.project.config.daily_loss = 1;
    for (int j = 0; j < 6; ++j) {
        quant::PortfolioAsset a;
        a.symbol = std::string(1, static_cast<char>('A' + j));
        a.weight = .1;
        a.tick_size = .01;
        for (int i = 0; i < 5000; ++i) {
            auto b = bar(i);
            b.open = b.high = b.low = b.close = 100;
            a.bars.push_back(b);
        }
        s.project.input.assets.push_back(std::move(a));
        s.project.enabled.push_back(true);
        s.project.modes.push_back(0);
    }
    s.warmup_end = s.warmup_cutoff = bar(4999).end;
    s.excluded.assign(6, 0);
    for (int i = 5000; i < 10000; ++i) {
        auto b = bar(i);
        b.open = b.high = b.low = b.close = 100;
        s.frames.push_back(std::vector<quant::Bar>(6, b));
    }
    quant::PortfolioStreamState audit;
    audit.interval = 60;
    audit.watermark = bar(9999).time;
    audit.end = bar(9999).end;
    audit.coverage = bar(9963).time;
    for (int j = 0; j < 6; ++j) {
        audit.symbols.push_back(std::string(1, static_cast<char>('A' + j)));
        std::vector<quant::Bar> rows;
        for (int i = 4963; i < 5000; ++i)
            rows.push_back(s.frames[i][j]);
        audit.series.push_back(std::move(rows));
    }
    s.audit = audit;
    auto replay = live::replay_portfolio_account(s, c);
    s.reference = replay.account->result();
    s.baseline_reference = replay.baseline->result();
    CHECK(s.reference.equity.back() == doctest::Approx(s.project.config.initial_cash - s.reference.fees));
    double pnl = 0;
    for (const auto &p : s.reference.positions)
        pnl += p.pnl;
    CHECK(pnl == doctest::Approx(-s.reference.fees));
    auto bytes = live::encode_portfolio_account(s, c);
    CHECK(bytes.size() <= live::portfolio_account_byte_limit);
    auto restored = live::decode_portfolio_account(bytes, c);
    CHECK(restored.account->result().equity == s.reference.equity);
    CHECK(restored.snapshot.frames.size() == 5000);
    auto next = bar(10000);
    CHECK_THROWS(restored.account->step(std::vector<quant::Bar>(6, next)));
    CHECK(restored.account->result().common_bars == 5000);
}
