// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#ifndef BITCOIN_CONSENSUS_RECYCLE_STATE_H
#define BITCOIN_CONSENSUS_RECYCLE_STATE_H

#include <coins.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <serialize.h>

#include <map>
#include <optional>
#include <vector>

namespace Consensus {

/** A UTXO queued for removal at a deterministic block height. */
struct ExpiryEntry {
    COutPoint outpoint;
    Coin coin;

    SERIALIZE_METHODS(ExpiryEntry, obj) { READWRITE(obj.outpoint, obj.coin); }
};

/** Data needed to reverse one block's expiry and Pool accounting. */
struct RecycleBlockUndo {
    std::vector<ExpiryEntry> expired;
    CAmount pool_balance_before{0};
    CAmount payout{0};

    SERIALIZE_METHODS(RecycleBlockUndo, obj)
    {
        READWRITE(obj.expired, obj.pool_balance_before, obj.payout);
    }
};

/**
 * In-memory model of the persistent chainstate extension.
 *
 * Buckets are keyed by the height at which their outputs expire. Entries are
 * removed from a bucket when spent, preventing an ever-growing spent-output
 * queue. The same representation is used by tests and by the future DB cache
 * adapter so connect/disconnect semantics have one source of truth.
 */
class RecycleState {
private:
    CAmount m_pool_balance{0};
    std::map<int, std::map<COutPoint, Coin>> m_expiry_buckets;

public:
    CAmount PoolBalance() const { return m_pool_balance; }
    size_t BucketSize(int expiry_height) const;

    bool Queue(const COutPoint& outpoint, const Coin& coin);
    bool Unqueue(const COutPoint& outpoint, const Coin& coin);

    /** Remove the due bucket, add its value, then debit payout. */
    std::optional<RecycleBlockUndo> ExpireAndPay(int height, CAmount payout);

    /** Reverse ExpireAndPay exactly, including restoring the due bucket. */
    bool Undo(int height, const RecycleBlockUndo& undo);
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_RECYCLE_STATE_H
