// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#include <consensus/recycle_pool.h>

#include <consensus/amount.h>

#include <algorithm>
#include <limits>

namespace Consensus {

CAmount GetRecyclePayoutCap(int block_height)
{
    if (block_height < 0) return 0;
    const int reductions{block_height / RECYCLE_PAYOUT_REDUCTION_INTERVAL};
    if (reductions >= 64) return 0;
    return INITIAL_RECYCLE_PAYOUT_CAP >> reductions;
}

std::optional<CAmount> GetRecyclePayout(
    int block_height, CAmount balance_before, CAmount expired_value)
{
    if (block_height < 0 || balance_before < 0 || expired_value < 0) return std::nullopt;
    if (!MoneyRange(balance_before) || !MoneyRange(expired_value)) return std::nullopt;
    if (expired_value > std::numeric_limits<CAmount>::max() - balance_before) return std::nullopt;

    const CAmount available{balance_before + expired_value};
    if (!MoneyRange(available)) return std::nullopt;
    return std::min(available, GetRecyclePayoutCap(block_height));
}

std::optional<RecyclePoolUpdate> UpdateRecyclePool(
    CAmount balance_before, CAmount expired_value, CAmount payout)
{
    if (balance_before < 0 || expired_value < 0 || payout < 0) return std::nullopt;
    if (!MoneyRange(balance_before) || !MoneyRange(expired_value) || !MoneyRange(payout)) return std::nullopt;
    if (expired_value > std::numeric_limits<CAmount>::max() - balance_before) return std::nullopt;

    const CAmount available{balance_before + expired_value};
    if (!MoneyRange(available) || payout > available) return std::nullopt;

    const CAmount balance_after{available - payout};
    if (!MoneyRange(balance_after)) return std::nullopt;
    return RecyclePoolUpdate{balance_before, expired_value, payout, balance_after};
}

} // namespace Consensus
