// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#include <boost/test/unit_test.hpp>

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>

#include <limits>

BOOST_AUTO_TEST_SUITE(recycle_pool_tests)

BOOST_AUTO_TEST_CASE(expiry_boundary)
{
    constexpr int created{100};
    BOOST_CHECK(!Consensus::IsUTXOExpired(created, created + Consensus::UTXO_EXPIRY_AGE - 1));
    BOOST_CHECK(Consensus::IsUTXOExpired(created, created + Consensus::UTXO_EXPIRY_AGE));
    BOOST_CHECK_EQUAL(Consensus::UTXOExpiryHeight(created), created + 5'256'000);
    BOOST_CHECK(!Consensus::IsUTXOExpired(created, created - 1));
}

BOOST_AUTO_TEST_CASE(accounting_is_reversible)
{
    const auto update{Consensus::UpdateRecyclePool(7 * COIN, 3 * COIN, 2 * COIN)};
    BOOST_REQUIRE(update);
    BOOST_CHECK_EQUAL(update->balance_before, 7 * COIN);
    BOOST_CHECK_EQUAL(update->expired_value, 3 * COIN);
    BOOST_CHECK_EQUAL(update->payout, 2 * COIN);
    BOOST_CHECK_EQUAL(update->balance_after, 8 * COIN);
    BOOST_CHECK_EQUAL(update->balance_after - update->expired_value + update->payout,
                      update->balance_before);
}

BOOST_AUTO_TEST_CASE(rejects_invalid_accounting)
{
    BOOST_CHECK(!Consensus::UpdateRecyclePool(-1, 0));
    BOOST_CHECK(!Consensus::UpdateRecyclePool(0, -1));
    BOOST_CHECK(!Consensus::UpdateRecyclePool(COIN, 0, 2 * COIN));
    BOOST_CHECK(!Consensus::UpdateRecyclePool(MAX_MONEY, 1));
}

BOOST_AUTO_TEST_CASE(payout_reduces_every_twenty_five_years)
{
    constexpr int interval{Consensus::RECYCLE_PAYOUT_REDUCTION_INTERVAL};
    BOOST_CHECK_EQUAL(interval, 1'314'000);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(0), 50 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(interval - 1), 50 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(interval), 25 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(4 * interval), 3 * COIN + COIN / 8);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(64 * interval), 0);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(-1), 0);
}

BOOST_AUTO_TEST_CASE(payout_is_limited_by_available_pool)
{
    const auto full_cap{Consensus::GetRecyclePayout(0, 100 * COIN, 0)};
    BOOST_REQUIRE(full_cap);
    BOOST_CHECK_EQUAL(*full_cap, 50 * COIN);

    const auto partial{Consensus::GetRecyclePayout(0, 2 * COIN, COIN)};
    BOOST_REQUIRE(partial);
    BOOST_CHECK_EQUAL(*partial, 3 * COIN);

    const auto newly_expired{Consensus::GetRecyclePayout(0, 0, 7 * COIN)};
    BOOST_REQUIRE(newly_expired);
    BOOST_CHECK_EQUAL(*newly_expired, 7 * COIN);

    BOOST_CHECK(!Consensus::GetRecyclePayout(-1, 0, 0));
    BOOST_CHECK(!Consensus::GetRecyclePayout(0, -1, 0));
    BOOST_CHECK(!Consensus::GetRecyclePayout(0, MAX_MONEY, 1));
}

BOOST_AUTO_TEST_CASE(expiry_state_connect_disconnect)
{
    Consensus::RecycleState state;
    const COutPoint outpoint{};
    Coin coin;
    coin.out.nValue = 8 * COIN;
    coin.nHeight = 100;
    const int expiry_height{100 + Consensus::UTXO_EXPIRY_AGE};

    BOOST_CHECK(state.Queue(outpoint, coin));
    BOOST_CHECK(!state.Queue(outpoint, coin));
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 1U);

    const auto undo{state.ExpireAndPay(expiry_height, 3 * COIN)};
    BOOST_REQUIRE(undo);
    BOOST_CHECK_EQUAL(undo->expired.size(), 1U);
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);

    BOOST_CHECK(state.Undo(expiry_height, *undo));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 0);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 1U);
    BOOST_CHECK(state.Unqueue(outpoint, coin));
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);
}

BOOST_AUTO_TEST_CASE(failed_expiry_is_atomic)
{
    Consensus::RecycleState state;
    const COutPoint outpoint{};
    Coin coin;
    coin.out.nValue = 8 * COIN;
    coin.nHeight = 100;
    const int expiry_height{100 + Consensus::UTXO_EXPIRY_AGE};

    BOOST_REQUIRE(state.Queue(outpoint, coin));
    BOOST_CHECK(!state.ExpireAndPay(expiry_height, 9 * COIN));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 0);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 1U);
}

BOOST_AUTO_TEST_CASE(rejects_malformed_queue_and_undo_without_mutation)
{
    Consensus::RecycleState state;
    const int expiry_height{100 + Consensus::UTXO_EXPIRY_AGE};
    Coin valid;
    valid.out.nValue = 8 * COIN;
    valid.nHeight = 100;
    BOOST_REQUIRE(state.Queue(COutPoint{}, valid));
    const auto undo{state.ExpireAndPay(expiry_height, 3 * COIN)};
    BOOST_REQUIRE(undo);
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);

    auto malformed{*undo};
    malformed.payout = MAX_MONEY + 1;
    BOOST_CHECK(!state.Undo(expiry_height, malformed));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);

    malformed = *undo;
    malformed.expired.front().coin.nHeight = std::numeric_limits<int32_t>::max();
    BOOST_CHECK(!state.Undo(expiry_height, malformed));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);

    malformed = *undo;
    malformed.pool_balance_before = COIN;
    BOOST_CHECK(!state.Undo(expiry_height, malformed));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);

    malformed = *undo;
    malformed.expired.push_back(malformed.expired.front());
    BOOST_CHECK(!state.Undo(expiry_height, malformed));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 5 * COIN);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry_height), 0U);

    Coin invalid_value{valid};
    invalid_value.out.nValue = -1;
    BOOST_CHECK(!state.Queue(COutPoint{Txid::FromUint256(uint256::ONE), 1}, invalid_value));
}

BOOST_AUTO_TEST_CASE(payout_satoshi_truncation_boundaries)
{
    constexpr int interval{Consensus::RECYCLE_PAYOUT_REDUCTION_INTERVAL};
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(32 * interval), 1);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(33 * interval), 0);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(std::numeric_limits<int>::max()), 0);
}

BOOST_AUTO_TEST_SUITE_END()
