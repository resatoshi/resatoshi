// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#ifndef BITCOIN_CONSENSUS_RECYCLE_POOL_H
#define BITCOIN_CONSENSUS_RECYCLE_POOL_H

#include <consensus/amount.h>
#include <consensus/params.h>

#include <optional>

namespace Consensus {

/** Fixed 25-year payout era at the 10-minute target spacing. */
static constexpr int RECYCLE_PAYOUT_REDUCTION_INTERVAL{1'314'000};
static constexpr CAmount INITIAL_RECYCLE_PAYOUT_CAP{50 * COIN};

/** Whether an output created at coin_height is expired in spend_height. */
constexpr bool IsUTXOExpired(int coin_height, int spend_height)
{
    return spend_height >= coin_height && spend_height - coin_height >= UTXO_EXPIRY_AGE;
}

/** The block height at which outputs created at coin_height expire. */
constexpr int UTXOExpiryHeight(int coin_height)
{
    return coin_height + UTXO_EXPIRY_AGE;
}

/**
 * Reversible accounting result for one connected block.
 *
 * Payout is intentionally supplied by the caller: the payout schedule remains
 * a separate consensus decision. No implicit issuance is performed here.
 */
struct RecyclePoolUpdate {
    CAmount balance_before;
    CAmount expired_value;
    CAmount payout;
    CAmount balance_after;
};

/**
 * Apply one block's expired value and optional payout. Returns nullopt for a
 * negative value, overflow, a payout larger than the available pool, or a
 * result outside MoneyRange().
 */
std::optional<RecyclePoolUpdate> UpdateRecyclePool(
    CAmount balance_before, CAmount expired_value, CAmount payout = 0);

/** Maximum Recycle Pool payout permitted at block_height. */
CAmount GetRecyclePayoutCap(int block_height);

/**
 * Deterministic payout for a block. Newly expired value is available before
 * payout. The result is min(available pool, the era's payout cap).
 */
std::optional<CAmount> GetRecyclePayout(
    int block_height, CAmount balance_before, CAmount expired_value);

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_RECYCLE_POOL_H
