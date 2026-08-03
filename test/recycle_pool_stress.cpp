// Standalone deterministic stress harness for consensus Recycle Pool arithmetic.
// Build example:
//   c++ -std=c++20 -O2 -fsanitize=undefined -Isrc test/recycle_pool_stress.cpp
//       src/consensus/recycle_pool.cpp -o recycle_pool_stress

#include <consensus/amount.h>
#include <consensus/recycle_pool.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <random>

int main()
{
    std::mt19937_64 rng{0x52535420260803ULL};

    // Random valid transitions must conserve Pool accounting exactly.
    for (int i = 0; i < 1'000'000; ++i) {
        const CAmount before{static_cast<CAmount>(rng() % (MAX_MONEY + 1))};
        const CAmount room{MAX_MONEY - before};
        const CAmount expired{static_cast<CAmount>(rng() % (static_cast<uint64_t>(room) + 1))};
        const CAmount available{before + expired};
        const CAmount payout{static_cast<CAmount>(rng() % (static_cast<uint64_t>(available) + 1))};
        const auto update{Consensus::UpdateRecyclePool(before, expired, payout)};
        assert(update);
        assert(update->balance_after == before + expired - payout);
        assert(update->balance_after - expired + payout == before);
        assert(MoneyRange(update->balance_after));
    }

    // A miner may underclaim its coinbase. Only value above ordinary subsidy
    // and fees may debit the Pool, and the unclaimed remainder must stay in it.
    for (int i = 0; i < 1'000'000; ++i) {
        const CAmount ordinary{static_cast<CAmount>(rng() % (MAX_MONEY - COIN + 1))};
        const CAmount allowance{static_cast<CAmount>(rng() % (COIN + 1))};
        const CAmount maximum{ordinary + allowance};
        const CAmount coinbase{static_cast<CAmount>(rng() % (static_cast<uint64_t>(maximum) + 1))};
        const CAmount expected_claim{coinbase > ordinary ? coinbase - ordinary : 0};

        const auto claim{Consensus::GetClaimedRecyclePayout(coinbase, ordinary, allowance)};
        assert(claim && *claim == expected_claim);
        const auto update{Consensus::UpdateRecyclePool(allowance, 0, *claim)};
        assert(update);
        assert(update->balance_after == allowance - expected_claim);
    }

    // Invalid and overflow-adjacent values must be rejected.
    assert(!Consensus::UpdateRecyclePool(-1, 0, 0));
    assert(!Consensus::UpdateRecyclePool(0, -1, 0));
    assert(!Consensus::UpdateRecyclePool(0, 0, -1));
    assert(!Consensus::UpdateRecyclePool(MAX_MONEY, 1, 0));
    assert(!Consensus::UpdateRecyclePool(std::numeric_limits<CAmount>::max(), 1, 0));
    assert(!Consensus::GetClaimedRecyclePayout(COIN + 1, 0, COIN));
    assert(!Consensus::GetClaimedRecyclePayout(MAX_MONEY, MAX_MONEY, 1));

    // The per-block cap is exactly 1 RST and never halves, even at the
    // largest representable block height.
    assert(Consensus::GetRecyclePayoutCap(0) == COIN);
    assert(Consensus::GetRecyclePayoutCap(1'314'000) == COIN);
    assert(Consensus::GetRecyclePayoutCap(Consensus::UTXO_EXPIRY_AGE) == COIN);
    assert(Consensus::GetRecyclePayoutCap(std::numeric_limits<int>::max()) == COIN);
    assert(Consensus::GetRecyclePayoutCap(-1) == 0);

    const auto under_cap{Consensus::GetRecyclePayoutAllowance(0, COIN / 2, 0)};
    assert(under_cap && *under_cap == COIN / 2);
    const auto over_cap{Consensus::GetRecyclePayoutAllowance(0, 10 * COIN, 0)};
    assert(over_cap && *over_cap == COIN);
    const auto final_satoshi{Consensus::GetRecyclePayoutAllowance(std::numeric_limits<int>::max(), 1, 0)};
    assert(final_satoshi && *final_satoshi == 1);

    return 0;
}
