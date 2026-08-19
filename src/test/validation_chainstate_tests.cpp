// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/recycle_pool.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <vector>

class CTxMemPool;

namespace {

struct SyntheticBlockIndex {
    uint256 hash;
    CBlockIndex index;

    SyntheticBlockIndex(const CBlock& block, CBlockIndex* previous, int height)
        : hash{block.GetHash()}, index{block}
    {
        index.phashBlock = &hash;
        index.pprev = previous;
        index.nHeight = height;
        index.nStatus = BlockStatus::BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA;
    }
};

CBlock MakeSyntheticBlock(
    const CBlockIndex& previous,
    int height,
    CAmount coinbase_value,
    std::vector<CTransactionRef> transactions = {})
{
    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript{} << height << OP_0;
    coinbase.vout.emplace_back(coinbase_value, CScript{} << OP_TRUE);

    CBlock block;
    block.nVersion = VERSIONBITS_TOP_BITS;
    block.hashPrevBlock = previous.GetBlockHash();
    block.nTime = previous.GetBlockTime() + 1;
    block.nBits = previous.nBits;
    block.vtx.emplace_back(MakeTransactionRef(std::move(coinbase)));
    block.vtx.insert(block.vtx.end(), transactions.begin(), transactions.end());
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;
    return block;
}

CMutableTransaction SpendToAnyoneCanSpend(const COutPoint& input, CAmount output_value)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(input);
    tx.vout.emplace_back(output_value, CScript{} << OP_TRUE);
    return tx;
}

Coin TestCoin(CAmount value, int height)
{
    return Coin{CTxOut{value, CScript{} << OP_TRUE}, height, /*fCoinBaseIn=*/false};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(8_MiB));
    BOOST_REQUIRE(manager.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(m_rng, c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(m_rng.rand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            16_MiB, // upsizing the coinsview cache
            4_MiB // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            4_MiB, // downsizing the coinsview cache
            8_MiB // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(connect_tip_does_not_cache_inputs_on_failed_connect, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};

    COutPoint outpoint;
    {
        LOCK(cs_main);
        outpoint = AddTestCoin(m_rng, chainstate.CoinsTip());
        chainstate.CoinsTip().Flush(/*reallocate_cache=*/false);
    }

    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(MAX_MONEY, CScript{} << OP_TRUE);

    const auto tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const CBlock block{CreateBlock({tx}, CScript{} << OP_TRUE)};
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(block), true, true, nullptr));

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip()->GetBlockHash()); // block rejected
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoinInCache(outpoint));    // input not cached
}

BOOST_FIXTURE_TEST_CASE(recycle_expiry_connect_disconnect_production_path, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    CBlockIndex* const real_tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip())};
    constexpr int created_height{100};
    constexpr int expiry_height{created_height + Consensus::UTXO_EXPIRY_AGE};
    constexpr CAmount initial_pool{2 * COIN};
    constexpr CAmount fee{1000};

    // ConnectBlock at expiry-1 must still accept a transaction spending the coin.
    const COutPoint before_outpoint{Txid::FromUint256(uint256{1}), 0};
    CCoinsViewCache before_view{&CoinsViewEmpty::Get()};
    before_view.SetBestBlock(real_tip->GetBlockHash());
    before_view.AddCoin(before_outpoint, TestCoin(2 * COIN, created_height), false);
    const CMutableTransaction before_spend{SpendToAnyoneCanSpend(before_outpoint, 2 * COIN - fee)};
    const CAmount before_reward{GetBlockSubsidy(expiry_height - 1, Params().GetConsensus()) + fee};
    const CBlock before_block{MakeSyntheticBlock(
        *real_tip, expiry_height - 1, before_reward, {MakeTransactionRef(before_spend)})};
    SyntheticBlockIndex before_index{before_block, real_tip, expiry_height - 1};
    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.ConnectBlock(before_block, state, &before_index.index, before_view, /*fJustCheck=*/true));
        BOOST_CHECK(!before_view.HaveCoin(before_outpoint));
    }

    // Prepare multiple outputs expiring in the same block, one already-spent
    // output, and one live input so transaction undo and expiry undo coexist.
    const COutPoint expiry_a{Txid::FromUint256(uint256{2}), 7};
    const COutPoint expiry_b{Txid::FromUint256(uint256{3}), 1};
    const COutPoint spent_early{Txid::FromUint256(uint256{4}), 9};
    const COutPoint live_input{Txid::FromUint256(uint256{5}), 0};
    CCoinsViewCache view{&CoinsViewEmpty::Get()};
    view.SetBestBlock(real_tip->GetBlockHash());
    view.SetRecyclePoolBalance(initial_pool);
    view.AddCoin(expiry_b, TestCoin(3 * COIN, created_height), false);
    view.AddCoin(expiry_a, TestCoin(2 * COIN, created_height), false);
    view.AddCoin(spent_early, TestCoin(4 * COIN, created_height), false);
    BOOST_REQUIRE(view.SpendCoin(spent_early));
    view.AddCoin(live_input, TestCoin(COIN, created_height + 1), false);

    const auto original_bucket{view.GetRecycleExpiryBucket(expiry_height)};
    BOOST_REQUIRE_EQUAL(original_bucket.size(), 2U);
    BOOST_CHECK(original_bucket.contains(expiry_a));
    BOOST_CHECK(original_bucket.contains(expiry_b));
    BOOST_CHECK(!original_bucket.contains(spent_early));
    const auto check_original_bucket = [&] {
        const auto bucket{view.GetRecycleExpiryBucket(expiry_height)};
        BOOST_REQUIRE_EQUAL(bucket.size(), original_bucket.size());
        for (const auto& [outpoint, coin] : original_bucket) {
            BOOST_REQUIRE(bucket.contains(outpoint));
            const Coin& restored{bucket.at(outpoint)};
            BOOST_CHECK(restored.out == coin.out);
            BOOST_CHECK_EQUAL(restored.nHeight, coin.nHeight);
            BOOST_CHECK_EQUAL(restored.IsCoinBase(), coin.IsCoinBase());
        }
    };

    // A block spending an output at its exact expiry height is rejected. The
    // caller's disposable cache is mutated, but the parent chainstate view must
    // remain byte-for-byte equivalent in all Recycle state visible here.
    const CMutableTransaction expired_spend{SpendToAnyoneCanSpend(expiry_a, 2 * COIN - fee)};
    const CBlock rejected_block{MakeSyntheticBlock(
        *real_tip, expiry_height, GetBlockSubsidy(expiry_height, Params().GetConsensus()),
        {MakeTransactionRef(expired_spend)})};
    SyntheticBlockIndex rejected_index{rejected_block, real_tip, expiry_height};
    {
        CCoinsViewCache rejected_view{&view};
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_CHECK(!chainstate.ConnectBlock(
            rejected_block, state, &rejected_index.index, rejected_view, /*fJustCheck=*/true));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }
    BOOST_CHECK(view.HaveCoin(expiry_a));
    BOOST_CHECK(view.HaveCoin(expiry_b));
    BOOST_CHECK(view.HaveCoin(live_input));
    BOOST_CHECK_EQUAL(view.GetRecyclePoolBalance(), initial_pool);
    check_original_bucket();

    const CMutableTransaction live_spend{SpendToAnyoneCanSpend(live_input, COIN - fee)};
    const CAmount ordinary_reward{GetBlockSubsidy(expiry_height, Params().GetConsensus()) + fee};
    const CAmount recycle_claim{Consensus::RECYCLE_PAYOUT_CAP};
    const CBlock expiry_block{MakeSyntheticBlock(
        *real_tip, expiry_height, ordinary_reward + recycle_claim,
        {MakeTransactionRef(live_spend)})};
    SyntheticBlockIndex expiry_index{expiry_block, real_tip, expiry_height};

    auto check_connected = [&] {
        BOOST_CHECK(!view.HaveCoin(expiry_a));
        BOOST_CHECK(!view.HaveCoin(expiry_b));
        BOOST_CHECK(!view.HaveCoin(spent_early));
        BOOST_CHECK(!view.HaveCoin(live_input));
        BOOST_CHECK(view.HaveCoin(COutPoint{live_spend.GetHash(), 0}));
        BOOST_CHECK(view.GetRecycleExpiryBucket(expiry_height).empty());
        BOOST_CHECK_EQUAL(view.GetRecyclePoolBalance(), initial_pool + 5 * COIN - recycle_claim);
    };

    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.ConnectBlock(expiry_block, state, &expiry_index.index, view));
        check_connected();
        BOOST_REQUIRE_EQUAL(chainstate.DisconnectBlock(expiry_block, &expiry_index.index, view), DISCONNECT_OK);
    }

    BOOST_CHECK(view.HaveCoin(expiry_a));
    BOOST_CHECK(view.HaveCoin(expiry_b));
    BOOST_CHECK(!view.HaveCoin(spent_early));
    BOOST_CHECK(view.HaveCoin(live_input));
    BOOST_CHECK(!view.HaveCoin(COutPoint{live_spend.GetHash(), 0}));
    check_original_bucket();
    BOOST_CHECK_EQUAL(view.GetRecyclePoolBalance(), initial_pool);
    BOOST_CHECK_EQUAL(view.GetBestBlock(), real_tip->GetBlockHash());

    // Reconnecting the identical block must reproduce the exact same result.
    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.ConnectBlock(expiry_block, state, &expiry_index.index, view));
        check_connected();
    }
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const auto get_notify_tip{[&]() {
        LOCK(m_node.notifications->m_tip_block_mutex);
        BOOST_REQUIRE(m_node.notifications->TipBlock());
        return *m_node.notifications->TipBlock();
    }};
    uint256 curr_tip = get_notify_tip();

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        chainman.m_blockman.ReadBlock(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));

    curr_tip = get_notify_tip();

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    curr_tip = get_notify_tip();

    Chainstate& background_cs{*Assert(WITH_LOCK(::cs_main, return chainman.HistoricalChainstate()))};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // get_notify_tip() should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, get_notify_tip());
}

BOOST_AUTO_TEST_SUITE_END()
