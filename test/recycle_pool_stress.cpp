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

    // Invalid and overflow-adjacent values must be rejected.
    assert(!Consensus::UpdateRecyclePool(-1, 0, 0));
    assert(!Consensus::UpdateRecyclePool(0, -1, 0));
    assert(!Consensus::UpdateRecyclePool(0, 0, -1));
    assert(!Consensus::UpdateRecyclePool(MAX_MONEY, 1, 0));
    assert(!Consensus::UpdateRecyclePool(std::numeric_limits<CAmount>::max(), 1, 0));

    // Exact era boundaries and satoshi truncation.
    constexpr int interval{Consensus::RECYCLE_PAYOUT_REDUCTION_INTERVAL};
    assert(Consensus::GetRecyclePayoutCap(interval - 1) == 50 * COIN);
    assert(Consensus::GetRecyclePayoutCap(interval) == 25 * COIN);
    assert(Consensus::GetRecyclePayoutCap(32 * interval) == 1);
    assert(Consensus::GetRecyclePayoutCap(33 * interval) == 0);
    assert(Consensus::GetRecyclePayoutCap(std::numeric_limits<int>::max()) == 0);

    return 0;
}
