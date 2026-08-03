// Standalone deterministic stress harness for expiry buckets and reorg undo.

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_state.h>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

int main()
{
    Consensus::RecycleState state;
    constexpr int created_height{100};
    constexpr int expiry_height{created_height + Consensus::UTXO_EXPIRY_AGE};
    constexpr int count{10'000};
    std::vector<std::pair<COutPoint, Coin>> entries;
    entries.reserve(count);

    for (int i = 0; i < count; ++i) {
        Coin coin;
        coin.nHeight = created_height;
        coin.out.nValue = 1'000 + i;
        const COutPoint outpoint{Txid{}, static_cast<uint32_t>(i)};
        assert(state.Queue(outpoint, coin));
        assert(!state.Queue(outpoint, coin));
        entries.emplace_back(outpoint, coin);
    }
    assert(state.BucketSize(expiry_height) == count);

    // Removal must match the exact queued Coin. An outpoint paired with
    // different metadata must not be able to delete the real expiry entry.
    Coin mismatched_coin{entries.front().second};
    ++mismatched_coin.out.nValue;
    assert(!state.Unqueue(entries.front().first, mismatched_coin));
    assert(state.BucketSize(expiry_height) == count);

    Coin different_height{entries.front().second};
    ++different_height.nHeight;
    assert(!state.Queue(entries.front().first, different_height));
    assert(state.BucketSize(expiry_height + 1) == 0);

    CAmount expected_expired{0};
    for (int i = 0; i < count; ++i) {
        if ((i % 2) == 0) {
            assert(state.Unqueue(entries[i].first, entries[i].second));
            assert(!state.Unqueue(entries[i].first, entries[i].second));
        } else {
            expected_expired += entries[i].second.out.nValue;
        }
    }
    assert(state.BucketSize(expiry_height) == count / 2);

    const auto undo{state.ExpireAndPay(expiry_height, expected_expired / 3)};
    assert(undo);
    assert(undo->expired.size() == count / 2);
    assert(state.PoolBalance() == expected_expired - expected_expired / 3);
    assert(state.BucketSize(expiry_height) == 0);

    // Corrupted undo must fail atomically.
    auto malformed{*undo};
    malformed.expired.front().coin.nHeight = std::numeric_limits<int32_t>::max();
    assert(!state.Undo(expiry_height, malformed));
    assert(state.PoolBalance() == expected_expired - expected_expired / 3);
    assert(state.BucketSize(expiry_height) == 0);

    assert(state.Undo(expiry_height, *undo));
    assert(state.PoolBalance() == 0);
    assert(state.BucketSize(expiry_height) == count / 2);
    assert(!state.Undo(expiry_height, *undo));

    return 0;
}
