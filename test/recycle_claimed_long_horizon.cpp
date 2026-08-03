// Standalone 600-year simulation of adversarial coinbase underclaims.
//
// Build example:
//   c++ -std=c++20 -O2 -Isrc test/recycle_claimed_long_horizon.cpp
//       src/consensus/recycle_pool.cpp src/consensus/recycle_state.cpp
//       src/uint256.cpp src/crypto/hex_base.cpp -o recycle_claimed_long_horizon

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

constexpr int BLOCKS_PER_YEAR{52'560};
constexpr int LAST_YEAR{600};
constexpr CAmount EXPIRY_BATCH{600'000 * COIN};

constexpr int HeightAtYear(int year)
{
    return year * BLOCKS_PER_YEAR;
}

CAmount OrdinaryReward(int height)
{
    const int halvings{height / 210'000};
    const CAmount subsidy{height == 0 || halvings >= 64 ? 0 : (50 * COIN) >> halvings};
    const CAmount synthetic_fees{height % 10'000};
    return subsidy + synthetic_fees;
}

} // namespace

int main()
{
    static_assert(HeightAtYear(100) == Consensus::UTXO_EXPIRY_AGE);

    Consensus::RecycleState state;
    for (uint32_t event{0}; event <= 50; ++event) {
        Coin coin;
        coin.nHeight = HeightAtYear(static_cast<int>(event) * 10);
        coin.out.nValue = EXPIRY_BATCH;
        assert(state.Queue(COutPoint{Txid{}, event}, coin));
    }

    CAmount total_expired{0};
    CAmount total_claimed{0};
    int zero_claim_blocks{0};
    int partial_claim_blocks{0};

    for (int height{0}; height <= HeightAtYear(LAST_YEAR); ++height) {
        const CAmount due{height >= HeightAtYear(100) && height % HeightAtYear(10) == 0
                ? EXPIRY_BATCH
                : 0};
        const CAmount pool_before{state.PoolBalance()};
        const auto allowance{Consensus::GetRecyclePayoutAllowance(height, pool_before, due)};
        assert(allowance);

        CAmount requested{*allowance};
        if (*allowance > 0 && height % 101 == 0) {
            requested = 0;
            ++zero_claim_blocks;
        } else if (*allowance > 1 && height % 37 == 0) {
            requested = *allowance / 3;
            ++partial_claim_blocks;
        }

        const CAmount ordinary{OrdinaryReward(height)};
        const CAmount coinbase_value{ordinary + requested};
        const auto claimed{Consensus::GetClaimedRecyclePayout(
            coinbase_value, ordinary, *allowance)};
        assert(claimed && *claimed == requested);

        // One satoshi above the allowance must always be rejected.
        assert(!Consensus::GetClaimedRecyclePayout(
            ordinary + *allowance + 1, ordinary, *allowance));

        const auto undo{state.ExpireAndPay(height, *claimed)};
        assert(undo && undo->payout == *claimed);
        CAmount observed_expiry{0};
        for (const auto& entry : undo->expired) observed_expiry += entry.coin.out.nValue;
        assert(observed_expiry == due);

        total_expired += due;
        total_claimed += *claimed;
        assert(state.PoolBalance() == total_expired - total_claimed);

        if (due > 0) {
            const CAmount pool_after{state.PoolBalance()};
            assert(state.Undo(height, *undo));
            assert(state.PoolBalance() == pool_before);
            const auto reapplied{state.ExpireAndPay(height, *claimed)};
            assert(reapplied && reapplied->payout == *claimed);
            assert(state.PoolBalance() == pool_after);
        }
    }

    assert(zero_claim_blocks > 0);
    assert(partial_claim_blocks > 0);
    assert(total_expired == 30'600'000 * COIN);
    assert(state.PoolBalance() == total_expired - total_claimed);

    std::cout << "600-year claimed-only simulation passed\n"
              << "zero_claim_blocks=" << zero_claim_blocks << "\n"
              << "partial_claim_blocks=" << partial_claim_blocks << "\n"
              << "claimed_satoshis=" << total_claimed << "\n"
              << "pool_satoshis=" << state.PoolBalance() << "\n";
    return 0;
}
