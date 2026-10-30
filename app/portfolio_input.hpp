#pragma once
#include "quant/portfolio.hpp"
#include <QString>
namespace live {
/** Explicit immutable copy of currently usable research candles; hidden holdout is excluded. */
struct PortfolioInput {
    std::vector<quant::PortfolioAsset> assets;
    QString source;
    bool synthetic = false, metadata_known = false;
};
} // namespace live
