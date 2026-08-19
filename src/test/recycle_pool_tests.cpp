// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license.

#include <boost/test/unit_test.hpp>

#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <node/utxo_snapshot.h>
#include <primitives/transaction.h>
#include <streams.h>

#include <limits>
#include <map>
#include <random>
#include <vector>

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

BOOST_AUTO_TEST_CASE(payout_cap_is_fixed_forever)
{
    BOOST_CHECK_EQUAL(Consensus::RECYCLE_PAYOUT_CAP, 1 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(0), 1 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(1'314'000), 1 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(Consensus::UTXO_EXPIRY_AGE), 1 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(std::numeric_limits<int>::max()), 1 * COIN);
    BOOST_CHECK_EQUAL(Consensus::GetRecyclePayoutCap(-1), 0);
}

BOOST_AUTO_TEST_CASE(payout_is_limited_by_available_pool)
{
    const auto full_cap{Consensus::GetRecyclePayoutAllowance(0, 100 * COIN, 0)};
    BOOST_REQUIRE(full_cap);
    BOOST_CHECK_EQUAL(*full_cap, 1 * COIN);

    const auto partial{Consensus::GetRecyclePayoutAllowance(0, COIN / 4, COIN / 4)};
    BOOST_REQUIRE(partial);
    BOOST_CHECK_EQUAL(*partial, COIN / 2);

    const auto newly_expired{Consensus::GetRecyclePayoutAllowance(0, 0, 7 * COIN)};
    BOOST_REQUIRE(newly_expired);
    BOOST_CHECK_EQUAL(*newly_expired, 1 * COIN);

    const auto final_satoshi{Consensus::GetRecyclePayoutAllowance(std::numeric_limits<int>::max(), 1, 0)};
    BOOST_REQUIRE(final_satoshi);
    BOOST_CHECK_EQUAL(*final_satoshi, 1);

    const auto empty{Consensus::GetRecyclePayoutAllowance(0, 0, 0)};
    BOOST_REQUIRE(empty);
    BOOST_CHECK_EQUAL(*empty, 0);

    BOOST_CHECK(!Consensus::GetRecyclePayoutAllowance(-1, 0, 0));
    BOOST_CHECK(!Consensus::GetRecyclePayoutAllowance(0, -1, 0));
    BOOST_CHECK(!Consensus::GetRecyclePayoutAllowance(0, MAX_MONEY, 1));
}

BOOST_AUTO_TEST_CASE(pool_is_charged_only_for_claimed_recycle_value)
{
    constexpr CAmount subsidy{50 * COIN};
    constexpr CAmount fees{COIN / 4};
    constexpr CAmount ordinary_reward{subsidy + fees};
    constexpr CAmount allowance{COIN};

    const auto underclaimed_ordinary{Consensus::GetClaimedRecyclePayout(
        40 * COIN, ordinary_reward, allowance)};
    BOOST_REQUIRE(underclaimed_ordinary);
    BOOST_CHECK_EQUAL(*underclaimed_ordinary, 0);

    const auto ordinary_only{Consensus::GetClaimedRecyclePayout(
        ordinary_reward, ordinary_reward, allowance)};
    BOOST_REQUIRE(ordinary_only);
    BOOST_CHECK_EQUAL(*ordinary_only, 0);

    const auto partial_recycle{Consensus::GetClaimedRecyclePayout(
        ordinary_reward + 30'000'000, ordinary_reward, allowance)};
    BOOST_REQUIRE(partial_recycle);
    BOOST_CHECK_EQUAL(*partial_recycle, 30'000'000);

    const auto full_recycle{Consensus::GetClaimedRecyclePayout(
        ordinary_reward + allowance, ordinary_reward, allowance)};
    BOOST_REQUIRE(full_recycle);
    BOOST_CHECK_EQUAL(*full_recycle, allowance);

    BOOST_CHECK(!Consensus::GetClaimedRecyclePayout(
        ordinary_reward + allowance + 1, ordinary_reward, allowance));
    BOOST_CHECK(!Consensus::GetClaimedRecyclePayout(-1, ordinary_reward, allowance));
    BOOST_CHECK(!Consensus::GetClaimedRecyclePayout(0, -1, allowance));
    BOOST_CHECK(!Consensus::GetClaimedRecyclePayout(0, ordinary_reward, -1));
    BOOST_CHECK(!Consensus::GetClaimedRecyclePayout(MAX_MONEY, MAX_MONEY, 1));
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

BOOST_AUTO_TEST_CASE(snapshot_bulk_load_builds_expiry_index)
{
    CCoinsViewCache cache{&CoinsViewEmpty::Get()};
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 7};
    Coin coin;
    coin.out.nValue = 3 * COIN;
    coin.nHeight = 123;
    const int expiry_height{Consensus::UTXOExpiryHeight(coin.nHeight)};

    cache.EmplaceCoinInternalDANGER(outpoint, Coin{coin});
    const auto bucket{cache.GetRecycleExpiryBucket(expiry_height)};
    BOOST_REQUIRE_EQUAL(bucket.size(), 1U);
    const Coin& indexed{bucket.at(outpoint)};
    BOOST_CHECK(indexed.out == coin.out);
    BOOST_CHECK_EQUAL(indexed.nHeight, coin.nHeight);
    BOOST_CHECK_EQUAL(indexed.IsCoinBase(), coin.IsCoinBase());
}

BOOST_AUTO_TEST_CASE(snapshot_metadata_v3_roundtrip_and_v2_zero_balance_compatibility)
{
    constexpr MessageStartChars magic{0xfa, 0xbf, 0xb5, 0xda};
    const uint256 base_hash{uint256::ONE};

    DataStream v3_stream;
    v3_stream << node::SnapshotMetadata{magic, base_hash, 42, 7 * COIN};
    node::SnapshotMetadata v3_metadata{magic};
    v3_stream >> v3_metadata;
    BOOST_CHECK(v3_metadata.m_base_blockhash == base_hash);
    BOOST_CHECK_EQUAL(v3_metadata.m_coins_count, 42U);
    BOOST_CHECK_EQUAL(v3_metadata.m_recycle_pool_balance, 7 * COIN);
    BOOST_CHECK(v3_stream.empty());

    // Version 2 has no serialized Pool field. It is safe only at a height
    // whose hardcoded AssumeUTXO commitment also expects zero: the production
    // loader requires exact equality with that commitment.
    DataStream v2_stream;
    v2_stream << SNAPSHOT_MAGIC_BYTES << uint16_t{2} << magic << base_hash << uint64_t{42};
    node::SnapshotMetadata v2_metadata{magic};
    v2_stream >> v2_metadata;
    BOOST_CHECK(v2_metadata.m_base_blockhash == base_hash);
    BOOST_CHECK_EQUAL(v2_metadata.m_coins_count, 42U);
    BOOST_CHECK_EQUAL(v2_metadata.m_recycle_pool_balance, 0);
    BOOST_CHECK(v2_metadata.MatchesRecyclePoolBalance(0));
    BOOST_CHECK(!v2_metadata.MatchesRecyclePoolBalance(COIN));
    BOOST_CHECK(v2_stream.empty());
}

BOOST_AUTO_TEST_CASE(snapshot_bulk_load_avoids_expiry_height_overflow)
{
    CCoinsViewCache cache{&CoinsViewEmpty::Get()};
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 8};
    Coin coin;
    coin.out.nValue = COIN;
    coin.nHeight = std::numeric_limits<int>::max();

    BOOST_REQUIRE_NO_THROW(cache.EmplaceCoinInternalDANGER(outpoint, Coin{coin}));
    BOOST_CHECK(cache.HaveCoin(outpoint));
    BOOST_CHECK(cache.GetRecycleExpiryBucket(std::numeric_limits<int>::max()).empty());
}

BOOST_AUTO_TEST_CASE(expiry_is_exactly_once_and_spend_boundary_is_consistent)
{
    constexpr int created{321};
    const int expiry{Consensus::UTXOExpiryHeight(created)};
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 1};
    Coin coin;
    coin.out.nValue = 4 * COIN;
    coin.nHeight = created;

    BOOST_CHECK(!Consensus::IsUTXOExpired(created, expiry - 1));
    BOOST_CHECK(Consensus::IsUTXOExpired(created, expiry));
    BOOST_CHECK(Consensus::IsUTXOExpired(created, expiry + 1));

    // A spend in the block before expiry removes the queued entry normally.
    Consensus::RecycleState spent_before;
    BOOST_REQUIRE(spent_before.Queue(outpoint, coin));
    BOOST_REQUIRE(spent_before.Unqueue(outpoint, coin));
    const auto empty_expiry{spent_before.ExpireAndPay(expiry, 0)};
    BOOST_REQUIRE(empty_expiry);
    BOOST_CHECK(empty_expiry->expired.empty());
    BOOST_CHECK_EQUAL(spent_before.PoolBalance(), 0);

    // At the expiry block, expiry is applied before transactions. The output
    // is removed exactly once and is therefore unavailable to a transaction.
    Consensus::RecycleState expires_at_boundary;
    BOOST_REQUIRE(expires_at_boundary.Queue(outpoint, coin));
    const auto undo{expires_at_boundary.ExpireAndPay(expiry, COIN)};
    BOOST_REQUIRE(undo);
    BOOST_REQUIRE_EQUAL(undo->expired.size(), 1U);
    BOOST_CHECK_EQUAL(expires_at_boundary.PoolBalance(), 3 * COIN);
    BOOST_CHECK(!expires_at_boundary.Unqueue(outpoint, coin));
    const auto second_pass{expires_at_boundary.ExpireAndPay(expiry, 0)};
    BOOST_REQUIRE(second_pass);
    BOOST_CHECK(second_pass->expired.empty());
    BOOST_CHECK_EQUAL(expires_at_boundary.PoolBalance(), 3 * COIN);

    BOOST_REQUIRE(expires_at_boundary.Undo(expiry, *second_pass));
    BOOST_REQUIRE(expires_at_boundary.Undo(expiry, *undo));
    BOOST_CHECK_EQUAL(expires_at_boundary.PoolBalance(), 0);
    BOOST_CHECK_EQUAL(expires_at_boundary.BucketSize(expiry), 1U);
}

BOOST_AUTO_TEST_CASE(transaction_input_is_spendable_before_but_not_at_expiry)
{
    constexpr int created{50};
    const int expiry{Consensus::UTXOExpiryHeight(created)};
    const COutPoint outpoint{Txid::FromUint256(uint256::ONE), 0};
    Coin coin;
    coin.out.nValue = 2 * COIN;
    coin.nHeight = created;

    CCoinsViewCache cache{&CoinsViewEmpty::Get()};
    cache.AddCoin(outpoint, Coin{coin}, false);

    CMutableTransaction mutable_tx;
    mutable_tx.vin.emplace_back(outpoint);
    mutable_tx.vout.emplace_back(COIN, CScript{});
    const CTransaction tx{mutable_tx};

    TxValidationState before_state;
    CAmount before_fee{0};
    BOOST_REQUIRE(Consensus::CheckTxInputs(tx, before_state, cache, expiry - 1, before_fee));
    BOOST_CHECK_EQUAL(before_fee, COIN);

    // ConnectBlock expires this bucket before processing the block's
    // transactions, so the same input must be missing at the boundary.
    BOOST_REQUIRE(cache.SpendCoin(outpoint));
    TxValidationState boundary_state;
    CAmount boundary_fee{0};
    BOOST_CHECK(!Consensus::CheckTxInputs(tx, boundary_state, cache, expiry, boundary_fee));
    BOOST_CHECK_EQUAL(boundary_state.GetRejectReason(), "bad-txns-inputs-missingorspent");
}

BOOST_AUTO_TEST_CASE(simultaneous_expiry_has_no_loss_duplicate_or_overflow)
{
    Consensus::RecycleState state;
    constexpr int created{700};
    const int expiry{Consensus::UTXOExpiryHeight(created)};
    constexpr int count{4096};
    CAmount expected{0};
    for (int i = 0; i < count; ++i) {
        Coin coin;
        coin.nHeight = created;
        coin.out.nValue = 1 + i;
        expected += coin.out.nValue;
        BOOST_REQUIRE(state.Queue(COutPoint{Txid{}, static_cast<uint32_t>(i)}, coin));
    }
    BOOST_REQUIRE_EQUAL(state.BucketSize(expiry), count);
    const CAmount claim{std::min<CAmount>(expected, Consensus::RECYCLE_PAYOUT_CAP)};
    const auto undo{state.ExpireAndPay(expiry, claim)};
    BOOST_REQUIRE(undo);
    BOOST_CHECK_EQUAL(undo->expired.size(), count);
    BOOST_CHECK_EQUAL(state.PoolBalance(), expected - claim);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry), 0U);
    BOOST_REQUIRE(state.Undo(expiry, *undo));
    BOOST_CHECK_EQUAL(state.PoolBalance(), 0);
    BOOST_CHECK_EQUAL(state.BucketSize(expiry), count);
}

BOOST_AUTO_TEST_CASE(randomized_expiry_reorg_invariants_1000_seeds)
{
    constexpr int seeds{1000};
    constexpr int coins_per_seed{64};
    for (int seed = 0; seed < seeds; ++seed) {
        std::mt19937_64 rng{static_cast<uint64_t>(seed)};
        Consensus::RecycleState state;
        std::map<int, CAmount> expected_by_height;
        std::map<int, size_t> expected_count;
        for (int i = 0; i < coins_per_seed; ++i) {
            Coin coin;
            coin.nHeight = 1 + static_cast<uint32_t>(rng() % 32);
            coin.out.nValue = 1 + static_cast<CAmount>(rng() % (10 * COIN));
            const COutPoint outpoint{Txid{}, static_cast<uint32_t>(i)};
            BOOST_REQUIRE(state.Queue(outpoint, coin));
            const bool spend_before_expiry{(rng() % 4) == 0};
            if (spend_before_expiry) {
                BOOST_REQUIRE(state.Unqueue(outpoint, coin));
            } else {
                const int height{Consensus::UTXOExpiryHeight(coin.nHeight)};
                expected_by_height[height] += coin.out.nValue;
                ++expected_count[height];
            }
        }

        CAmount expected_pool{0};
        struct Applied { int height; Consensus::RecycleBlockUndo undo; CAmount before; CAmount after; };
        std::vector<Applied> applied;
        for (const auto& [height, expired] : expected_by_height) {
            BOOST_CHECK_EQUAL(state.BucketSize(height), expected_count[height]);
            const auto allowance{Consensus::GetRecyclePayoutAllowance(height, expected_pool, expired)};
            BOOST_REQUIRE(allowance);
            const CAmount claim{*allowance == 0 ? 0 : static_cast<CAmount>(rng() % (static_cast<uint64_t>(*allowance) + 1))};
            const CAmount before{expected_pool};
            expected_pool += expired - claim;
            const auto undo{state.ExpireAndPay(height, claim)};
            BOOST_REQUIRE(undo);
            BOOST_CHECK_EQUAL(undo->expired.size(), expected_count[height]);
            BOOST_CHECK_EQUAL(state.PoolBalance(), expected_pool);
            applied.push_back({height, *undo, before, expected_pool});
        }

        // Reorg all expiry blocks, then reconnect them and require identical state.
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            BOOST_REQUIRE(state.Undo(it->height, it->undo));
            BOOST_CHECK_EQUAL(state.PoolBalance(), it->before);
            BOOST_CHECK_EQUAL(state.BucketSize(it->height), expected_count[it->height]);
        }
        BOOST_CHECK_EQUAL(state.PoolBalance(), 0);
        for (const auto& item : applied) {
            const auto redo{state.ExpireAndPay(item.height, item.undo.payout)};
            BOOST_REQUIRE(redo);
            BOOST_CHECK_EQUAL(redo->expired.size(), item.undo.expired.size());
            BOOST_CHECK_EQUAL(state.PoolBalance(), item.after);
        }
        BOOST_CHECK_EQUAL(state.PoolBalance(), expected_pool);
    }
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

BOOST_AUTO_TEST_SUITE_END()
