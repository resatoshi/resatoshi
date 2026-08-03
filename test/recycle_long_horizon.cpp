// Standalone long-horizon simulation for recurring UTXO expiry and fixed
// Recycle Pool payouts.
//
// Build example:
//   c++ -std=c++20 -O2 -Isrc test/recycle_long_horizon.cpp
//       src/consensus/recycle_pool.cpp src/consensus/recycle_state.cpp
//       src/uint256.cpp src/crypto/hex_base.cpp -o recycle_long_horizon
//   (write the four lines as one command)

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>

namespace {

constexpr int BLOCKS_PER_YEAR{52'560};
constexpr int CREATION_INTERVAL_YEARS{10};
constexpr int LAST_CREATION_YEAR{500};
constexpr int LAST_SIMULATED_YEAR{600};
constexpr CAmount EXPIRY_BATCH_VALUE{600'000 * COIN};

int HeightAtYear(int year)
{
    return year * BLOCKS_PER_YEAR;
}

} // namespace

int main()
{
    static_assert(100 * BLOCKS_PER_YEAR == Consensus::UTXO_EXPIRY_AGE);

    Consensus::RecycleState state;
    std::map<int, CAmount> expected_expiry;
    CAmount total_expired{0};
    CAmount total_paid{0};
    uint32_t output_index{0};

    // Every ten years another 600,000 RST remains unspent. Each one expires
    // exactly 100 years later. Since ten years of fixed payouts can release
    // only 525,600 RST, the Pool should keep paying 1 RST per block while also
    // growing by 74,400 RST per decade.
    for (int height{0}; height <= HeightAtYear(LAST_SIMULATED_YEAR); ++height) {
        if (height % HeightAtYear(CREATION_INTERVAL_YEARS) == 0) {
            const int creation_year{height / BLOCKS_PER_YEAR};
            if (creation_year <= LAST_CREATION_YEAR) {
                Coin coin;
                coin.nHeight = height;
                coin.out.nValue = EXPIRY_BATCH_VALUE;
                const COutPoint outpoint{Txid{}, output_index++};
                assert(state.Queue(outpoint, coin));
                expected_expiry.emplace(height + Consensus::UTXO_EXPIRY_AGE, EXPIRY_BATCH_VALUE);
            }
        }

        const CAmount due{expected_expiry.contains(height) ? expected_expiry.at(height) : 0};
        const CAmount available{state.PoolBalance() + due};
        const auto payout{Consensus::GetRecyclePayout(height, state.PoolBalance(), due)};
        assert(payout);
        assert(*payout == std::min(available, Consensus::RECYCLE_PAYOUT_CAP));
        if (available >= COIN) assert(*payout == COIN);

        const CAmount pool_before{state.PoolBalance()};
        const auto undo{state.ExpireAndPay(height, *payout)};
        assert(undo);

        CAmount observed_expiry{0};
        for (const auto& entry : undo->expired) observed_expiry += entry.coin.out.nValue;
        assert(observed_expiry == due);

        // At every expiry event, reverse and reapply the block. This exercises
        // century-scale disconnect/reconnect behavior as the Pool grows.
        if (due > 0) {
            const CAmount pool_after{state.PoolBalance()};
            assert(state.Undo(height, *undo));
            assert(state.PoolBalance() == pool_before);
            assert(state.BucketSize(height) == 1);

            const auto reapplied{state.ExpireAndPay(height, *payout)};
            assert(reapplied);
            assert(reapplied->expired.size() == 1);
            assert(state.PoolBalance() == pool_after);
        }

        total_expired += due;
        total_paid += *payout;
        assert(state.PoolBalance() == total_expired - total_paid);
        assert(MoneyRange(state.PoolBalance()));

        // Exact boundary snapshots. These include the expiry and the 1 RST
        // payout in the boundary block itself.
        if (height == HeightAtYear(100)) assert(state.PoolBalance() == 599'999 * COIN);
        if (height == HeightAtYear(110)) assert(state.PoolBalance() == 674'399 * COIN);
        if (height == HeightAtYear(150)) assert(state.PoolBalance() == 971'999 * COIN);
        if (height == HeightAtYear(200)) assert(state.PoolBalance() == 1'343'999 * COIN);
        if (height == HeightAtYear(300)) assert(state.PoolBalance() == 2'087'999 * COIN);
        if (height == HeightAtYear(400)) assert(state.PoolBalance() == 2'831'999 * COIN);
        if (height == HeightAtYear(500)) assert(state.PoolBalance() == 3'575'999 * COIN);
        if (height == HeightAtYear(600)) assert(state.PoolBalance() == 4'319'999 * COIN);
    }

    assert(total_expired == 51 * EXPIRY_BATCH_VALUE);
    assert(total_paid == (HeightAtYear(500) + 1) * COIN);
    assert(state.PoolBalance() == 4'319'999 * COIN);

    // Exercise the final representable expiry height as a separate boundary.
    // The fixed payout must still be 1 RST, and disconnect/reconnect must
    // remain exact without overflowing the creation-height calculation.
    Consensus::RecycleState final_height_state;
    Coin final_height_coin;
    final_height_coin.nHeight = std::numeric_limits<int>::max() - Consensus::UTXO_EXPIRY_AGE;
    final_height_coin.out.nValue = 2 * COIN;
    const COutPoint final_height_outpoint{Txid{}, 0};
    assert(final_height_state.Queue(final_height_outpoint, final_height_coin));
    const int final_height{std::numeric_limits<int>::max()};
    const auto final_height_payout{Consensus::GetRecyclePayout(final_height, 0, 2 * COIN)};
    assert(final_height_payout && *final_height_payout == COIN);
    const auto final_height_undo{final_height_state.ExpireAndPay(final_height, *final_height_payout)};
    assert(final_height_undo && final_height_state.PoolBalance() == COIN);
    assert(final_height_state.Undo(final_height, *final_height_undo));
    assert(final_height_state.PoolBalance() == 0);
    assert(final_height_state.BucketSize(final_height) == 1);
    const auto final_height_reapplied{final_height_state.ExpireAndPay(final_height, *final_height_payout)};
    assert(final_height_reapplied && final_height_state.PoolBalance() == COIN);

    Coin overflowing_height_coin{final_height_coin};
    ++overflowing_height_coin.nHeight;
    assert(!final_height_state.Queue(COutPoint{Txid{}, 1}, overflowing_height_coin));

    std::cout << "600-year recycle simulation passed\n"
              << "expired=" << total_expired / COIN << " RST\n"
              << "paid=" << total_paid / COIN << " RST\n"
              << "pool=" << state.PoolBalance() / COIN << " RST\n";
    return 0;
}
