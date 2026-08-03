// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#include <consensus/recycle_state.h>

#include <consensus/recycle_pool.h>

namespace Consensus {

size_t RecycleState::BucketSize(int expiry_height) const
{
    const auto it{m_expiry_buckets.find(expiry_height)};
    return it == m_expiry_buckets.end() ? 0 : it->second.size();
}

bool RecycleState::Queue(const COutPoint& outpoint, const Coin& coin)
{
    if (coin.IsSpent() || !MoneyRange(coin.out.nValue) ||
        coin.nHeight > static_cast<uint32_t>(INT_MAX - UTXO_EXPIRY_AGE)) return false;
    return m_expiry_buckets[UTXOExpiryHeight(coin.nHeight)].emplace(outpoint, coin).second;
}

bool RecycleState::Unqueue(const COutPoint& outpoint, const Coin& coin)
{
    if (coin.IsSpent() || !MoneyRange(coin.out.nValue) ||
        coin.nHeight > static_cast<uint32_t>(INT_MAX - UTXO_EXPIRY_AGE)) return false;
    const int height{UTXOExpiryHeight(coin.nHeight)};
    const auto bucket_it{m_expiry_buckets.find(height)};
    if (bucket_it == m_expiry_buckets.end()) return false;
    const auto entry_it{bucket_it->second.find(outpoint)};
    if (entry_it == bucket_it->second.end()) return false;
    bucket_it->second.erase(entry_it);
    if (bucket_it->second.empty()) m_expiry_buckets.erase(bucket_it);
    return true;
}

std::optional<RecycleBlockUndo> RecycleState::ExpireAndPay(int height, CAmount payout)
{
    if (height < 0 || payout < 0) return std::nullopt;

    RecycleBlockUndo undo;
    undo.pool_balance_before = m_pool_balance;
    undo.payout = payout;

    CAmount expired_value{0};
    if (const auto bucket_it{m_expiry_buckets.find(height)}; bucket_it != m_expiry_buckets.end()) {
        undo.expired.reserve(bucket_it->second.size());
        for (const auto& [outpoint, coin] : bucket_it->second) {
            if (!MoneyRange(coin.out.nValue) || coin.out.nValue > MAX_MONEY - expired_value) return std::nullopt;
            expired_value += coin.out.nValue;
            undo.expired.push_back({outpoint, coin});
        }
    }

    const auto update{UpdateRecyclePool(m_pool_balance, expired_value, payout)};
    if (!update) return std::nullopt;
    m_expiry_buckets.erase(height);
    m_pool_balance = update->balance_after;
    return undo;
}

bool RecycleState::Undo(int height, const RecycleBlockUndo& undo)
{
    if (height < 0 || !MoneyRange(undo.pool_balance_before) || !MoneyRange(undo.payout)) return false;
    if (m_expiry_buckets.contains(height)) return false;

    std::map<COutPoint, Coin> restored;
    CAmount expired_value{0};
    for (const auto& entry : undo.expired) {
        if (entry.coin.IsSpent() || !MoneyRange(entry.coin.out.nValue) ||
            entry.coin.nHeight > static_cast<uint32_t>(INT_MAX - UTXO_EXPIRY_AGE) ||
            UTXOExpiryHeight(entry.coin.nHeight) != height ||
            entry.coin.out.nValue > MAX_MONEY - expired_value) return false;
        expired_value += entry.coin.out.nValue;
        if (!restored.emplace(entry.outpoint, entry.coin).second) return false;
    }
    const auto expected{UpdateRecyclePool(undo.pool_balance_before, expired_value, undo.payout)};
    if (!expected || expected->balance_after != m_pool_balance) return false;
    if (!restored.empty()) m_expiry_buckets.emplace(height, std::move(restored));
    m_pool_balance = undo.pool_balance_before;
    return true;
}

} // namespace Consensus
