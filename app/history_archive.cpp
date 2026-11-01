#include "market_history.hpp"
namespace live {
void MarketHistory::archive_entry(const Entry &entry) {
    // Keep the most recently superseded copy of a content hash, including its provenance.
    for (qsizetype i = archive_.size(); i-- > 0;)
        if (archive_[i].info.key == entry.info.key && archive_[i].info.sha256 == entry.info.sha256)
            archive_.removeAt(i);
    archive_.push_back(entry);
    int count = 0;
    for (qsizetype i = archive_.size(); i-- > 0;)
        if (archive_[i].info.key == entry.info.key && ++count > 2)
            archive_.removeAt(i);
    while (archive_.size() > 8)
        archive_.removeFirst();
}
QVector<HistorySetInfo> MarketHistory::archived_datasets(const QString &key) const {
    QVector<HistorySetInfo> result;
    for (qsizetype i = archive_.size(); i-- > 0;)
        if (archive_[i].info.key == key) {
            auto info = archive_[i].info;
            info.archived = true;
            result.push_back(info);
        }
    return result;
}
bool MarketHistory::restore_revision(const QString &key, quint64 revision) {
    if (active_)
        return false;
    for (const auto &stored : archive_)
        if (stored.info.key == key && stored.info.revision == revision) {
            const auto entry = stored;
            current_info_ = entry.info;
            current_info_.archived = true;
            const auto generation = generation_;
            emit state_message(QStringLiteral("АРХИВ RAM: прежняя редакция; сеть не перепроверялась. "
                                              "Актуальный RAM-набор не заменён."));
            if (generation != generation_)
                return false;
            emit quality_report(entry.info.symbol, entry.received, entry.rejected);
            if (generation == generation_)
                emit loaded(entry.info.symbol, entry.info.preset, entry.bars, true);
            return generation == generation_;
        }
    return false;
}
bool MarketHistory::remove_revision(const QString &key, quint64 revision) {
    if (active_)
        return false;
    for (qsizetype i = 0; i < archive_.size(); ++i)
        if (archive_[i].info.key == key && archive_[i].info.revision == revision) {
            archive_.removeAt(i);
            emit history_changed();
            return true;
        }
    return false;
}
void MarketHistory::clear_archive() {
    archive_.clear();
    emit history_changed();
}
} // namespace live
