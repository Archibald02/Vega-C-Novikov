#pragma once
#include "portfolio.hpp"
#include <map>
namespace quant {
struct PortfolioStreamState {
    std::vector<std::string> symbols;
    std::vector<std::vector<Bar>> series;
    std::int64_t watermark = 0, end = 0, coverage = 0;
    int interval = 0;
};
/** Complete-cycle transactional aligner. Frames use ascending symbol order.
 * Revisions/deletions/late insertions in the audited overlap stop processing.
 * No interpolation; older history is not audited. At most 512 bars per symbol. */
class PortfolioStreamBuffer {
  public:
    PortfolioStreamBuffer(std::vector<PortfolioAsset> seed, std::int64_t watermark,
                          std::int64_t watermark_end, std::int64_t audit_begin, int interval);
    std::vector<std::vector<Bar>> merge(const std::vector<std::vector<Bar>> &cycle,
                                        std::int64_t request_begin, std::int64_t closed_cutoff);
    std::int64_t last_time() const { return last_time_; }
    std::int64_t last_end() const { return last_end_; }
    const std::vector<std::string> &symbols() const { return symbols_; }
    std::size_t retained_bars() const;
    PortfolioStreamState snapshot() const;
    static PortfolioStreamBuffer restore(const PortfolioStreamState &state);

  private:
    std::vector<std::map<std::int64_t, Bar>> known_;
    std::vector<std::string> symbols_;
    std::int64_t last_time_, last_end_, coverage_;
    int interval_;
};
} // namespace quant
