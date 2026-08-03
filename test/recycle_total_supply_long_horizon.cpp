// Standalone 600-year total-supply conservation simulation.
//
// Unlike recycle_long_horizon.cpp, every recurring expiry batch in this test
// is funded from already-issued, still-live RST. No test value is injected.
// The invariant checked after every block is:
//
//   issued subsidy == liquid UTXOs + queued live UTXOs + Recycle Pool
//
// Build example:
//   c++ -std=c++20 -O2 -Isrc test/recycle_total_supply_long_horizon.cpp
//       src/consensus/recycle_pool.cpp src/consensus/recycle_state.cpp
//       src/uint256.cpp src/crypto/hex_base.cpp
//       -o recycle_total_supply_long_horizon
//   (write the four lines as one command)

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>

namespace {

constexpr int BLOCKS_PER_YEAR{52'560};
constexpr int SUBSIDY_HALVING_INTERVAL{210'000};
constexpr int FIRST_BATCH_YEAR{10};
constexpr int CREATION_INTERVAL_YEARS{10};
constexpr int LAST_CREATION_YEAR{500};
constexpr int LAST_SIMULATED_YEAR{600};
constexpr CAmount EXPIRY_BATCH_VALUE{600'000 * COIN};

int HeightAtYear(int year)
{
    return year * BLOCKS_PER_YEAR;
}

// Mirrors GetBlockSubsidy() for mainnet. Height zero is special here because
// ReSatoshi's genesis output is deliberately zero rather than 50 RST.
CAmount IssuedSubsidyAtHeight(int height)
{
    if (height == 0) return 0;
    const int halvings{height / SUBSIDY_HALVING_INTERVAL};
    if (halvings >= 64) return 0;
    return (50 * COIN) >> halvings;
}

bool SupplyConserved(CAmount issued, CAmount liquid, CAmount queued, CAmount pool)
{
    if (!MoneyRange(issued) || !MoneyRange(liquid) ||
        !MoneyRange(queued) || !MoneyRange(pool)) return false;
    if (liquid > issued || queued > issued - liquid) return false;
    return pool == issued - liquid - queued;
}

void CheckSupply(CAmount issued, CAmount liquid, CAmount queued, CAmount pool)
{
    assert(SupplyConserved(issued, liquid, queued, pool));
}

} // namespace

int main()
{
    static_assert(100 * BLOCKS_PER_YEAR == Consensus::UTXO_EXPIRY_AGE);

    // Prove that the checker rejects both one-satoshi inflation and one-satoshi
    // loss before relying on it for the 31,536,001-block simulation.
    assert(SupplyConserved(COIN, COIN, 0, 0));
    assert(!SupplyConserved(COIN, COIN, 0, 1));
    assert(!SupplyConserved(COIN, COIN - 1, 0, 0));

    Consensus::RecycleState state;
    std::map<int, CAmount> expected_expiry;
    CAmount issued{0};
    CAmount liquid_utxos{0};
    CAmount queued_utxos{0};
    CAmount total_expired{0};
    CAmount total_paid{0};
    CAmount total_relocked{0};
    uint32_t output_index{0};

    for (int height{0}; height <= HeightAtYear(LAST_SIMULATED_YEAR); ++height) {
        // Ordinary subsidy is the only source of new RST in the simulation.
        const CAmount subsidy{IssuedSubsidyAtHeight(height)};
        assert(subsidy <= MAX_MONEY - issued);
        issued += subsidy;
        liquid_utxos += subsidy;

        // Every ten years, move 600,000 existing RST into a newly created UTXO
        // that will remain untouched for 100 years. This is a transfer between
        // live-UTXO categories, not issuance.
        if (height % HeightAtYear(CREATION_INTERVAL_YEARS) == 0) {
            const int creation_year{height / BLOCKS_PER_YEAR};
            if (creation_year >= FIRST_BATCH_YEAR && creation_year <= LAST_CREATION_YEAR) {
                assert(liquid_utxos >= EXPIRY_BATCH_VALUE);

                Coin coin;
                coin.nHeight = height;
                coin.out.nValue = EXPIRY_BATCH_VALUE;
                const COutPoint outpoint{Txid{}, output_index++};
                assert(state.Queue(outpoint, coin));
                assert(expected_expiry.emplace(
                    height + Consensus::UTXO_EXPIRY_AGE, EXPIRY_BATCH_VALUE).second);

                liquid_utxos -= EXPIRY_BATCH_VALUE;
                queued_utxos += EXPIRY_BATCH_VALUE;
                total_relocked += EXPIRY_BATCH_VALUE;
            }
        }

        const CAmount due{expected_expiry.contains(height) ? expected_expiry.at(height) : 0};
        const CAmount available{state.PoolBalance() + due};
        const auto payout{Consensus::GetRecyclePayoutAllowance(height, state.PoolBalance(), due)};
        assert(payout);
        assert(*payout == std::min(available, Consensus::RECYCLE_PAYOUT_CAP));

        const CAmount pool_before{state.PoolBalance()};
        const auto undo{state.ExpireAndPay(height, *payout)};
        assert(undo);

        CAmount observed_expiry{0};
        for (const auto& entry : undo->expired) observed_expiry += entry.coin.out.nValue;
        assert(observed_expiry == due);

        queued_utxos -= due;
        liquid_utxos += *payout;
        total_expired += due;
        total_paid += *payout;

        assert(state.PoolBalance() == total_expired - total_paid);
        CheckSupply(issued, liquid_utxos, queued_utxos, state.PoolBalance());

        // At every expiry event, disconnect the block and verify that both the
        // RecycleState and the whole-supply ledger return to their exact prior
        // values. Then reconnect it and verify the same invariant again.
        if (due > 0) {
            const CAmount pool_after{state.PoolBalance()};
            assert(state.Undo(height, *undo));
            queued_utxos += due;
            liquid_utxos -= *payout;
            total_expired -= due;
            total_paid -= *payout;

            assert(state.PoolBalance() == pool_before);
            assert(state.BucketSize(height) == 1);
            CheckSupply(issued, liquid_utxos, queued_utxos, state.PoolBalance());

            const auto reapplied{state.ExpireAndPay(height, *payout)};
            assert(reapplied && reapplied->expired.size() == 1);
            queued_utxos -= due;
            liquid_utxos += *payout;
            total_expired += due;
            total_paid += *payout;

            assert(state.PoolBalance() == pool_after);
            CheckSupply(issued, liquid_utxos, queued_utxos, state.PoolBalance());
        }

        // Take exact whole-supply snapshots at representative boundaries.
        if (height == HeightAtYear(100)) {
            assert(total_expired == 0);
            assert(state.PoolBalance() == 0);
        }
        if (height == HeightAtYear(110)) {
            assert(total_expired == 600'000 * COIN);
            assert(state.PoolBalance() == 599'999 * COIN);
        }
        if (height == HeightAtYear(600)) {
            assert(queued_utxos == 0);
            assert(state.PoolBalance() == 4'245'599 * COIN);
        }
    }

    assert(output_index == 50);
    assert(total_relocked == 30'000'000 * COIN);
    assert(total_expired == total_relocked);
    assert(total_paid == (HeightAtYear(600) - HeightAtYear(110) + 1LL) * COIN);
    assert(state.PoolBalance() == 4'245'599 * COIN);
    assert(queued_utxos == 0);
    CheckSupply(issued, liquid_utxos, queued_utxos, state.PoolBalance());

    std::cout << std::fixed << std::setprecision(8)
              << "600-year total-supply simulation passed\n"
              << "issued=" << static_cast<long double>(issued) / COIN << " RST\n"
              << "liquid_utxos=" << static_cast<long double>(liquid_utxos) / COIN << " RST\n"
              << "queued_utxos=" << static_cast<long double>(queued_utxos) / COIN << " RST\n"
              << "pool=" << static_cast<long double>(state.PoolBalance()) / COIN << " RST\n"
              << "cumulative_expired=" << static_cast<long double>(total_expired) / COIN << " RST\n"
              << "cumulative_paid=" << static_cast<long double>(total_paid) / COIN << " RST\n";
    return 0;
}
