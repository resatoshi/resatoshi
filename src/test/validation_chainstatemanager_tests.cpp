// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chainparams.h>
#include <consensus/recycle_pool.h>
#include <consensus/validation.h>
#include <kernel/disconnected_transactions.h>
#include <node/chainstatemanager_args.h>
#include <node/kernel_notifications.h>
#include <node/utxo_snapshot.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <txdb.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/result.h>
#include <util/vector.h>
#include <validation.h>
#include <validationinterface.h>

#include <tinyformat.h>

#include <vector>

#include <boost/test/unit_test.hpp>

using node::BlockManager;
using node::KernelNotifications;
using node::SnapshotMetadata;

BOOST_FIXTURE_TEST_SUITE(validation_chainstatemanager_tests, TestingSetup)

//! Basic tests for ChainstateManager.
//!
//! First create a legacy (IBD) chainstate, then create a snapshot chainstate.
BOOST_FIXTURE_TEST_CASE(chainstatemanager, TestChain100Setup)
{
    ChainstateManager& manager = *m_node.chainman;

    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.CurrentChainstate().m_from_snapshot_blockhash));

    // Create a legacy (IBD) chainstate.
    //
    Chainstate& c1 = manager.ActiveChainstate();

    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.CurrentChainstate().m_from_snapshot_blockhash));
    {
        LOCK(manager.GetMutex());
        BOOST_CHECK_EQUAL(manager.m_chainstates.size(), 1);
        BOOST_CHECK_EQUAL(manager.m_chainstates[0].get(), &c1);
    }

    auto& active_chain = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain, &c1.m_chain);

    // Get to a valid assumeutxo tip (per chainparams);
    mineBlocks(10);
    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 110);
    auto active_tip = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    auto exp_tip = c1.m_chain.Tip();
    BOOST_CHECK_EQUAL(active_tip, exp_tip);

    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.CurrentChainstate().m_from_snapshot_blockhash));

    // Create a snapshot-based chainstate.
    //
    const uint256 snapshot_blockhash = active_tip->GetBlockHash();
    Chainstate& c2{WITH_LOCK(::cs_main, return manager.AddChainstate(std::make_unique<Chainstate>(nullptr, manager.m_blockman, manager, snapshot_blockhash)))};
    c2.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    {
        LOCK(::cs_main);
        c2.InitCoinsCache(8_MiB);
        c2.CoinsTip().SetBestBlock(active_tip->GetBlockHash());
        for (const auto& cs : manager.m_chainstates) {
            cs->ClearBlockIndexCandidates();
        }
        c2.LoadChainTip();
        for (const auto& cs : manager.m_chainstates) {
            cs->PopulateBlockIndexCandidates();
        }
    }
    BlockValidationState _;
    BOOST_CHECK(c2.ActivateBestChain(_, nullptr));

    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return *manager.CurrentChainstate().m_from_snapshot_blockhash), snapshot_blockhash);
    BOOST_CHECK(WITH_LOCK(::cs_main, return manager.CurrentChainstate().m_assumeutxo == Assumeutxo::UNVALIDATED));
    BOOST_CHECK_EQUAL(&c2, &manager.ActiveChainstate());
    BOOST_CHECK(&c1 != &manager.ActiveChainstate());
    {
        LOCK(manager.GetMutex());
        BOOST_CHECK_EQUAL(manager.m_chainstates.size(), 2);
        BOOST_CHECK_EQUAL(manager.m_chainstates[0].get(), &c1);
        BOOST_CHECK_EQUAL(manager.m_chainstates[1].get(), &c2);
    }

    auto& active_chain2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain2, &c2.m_chain);

    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 110);
    mineBlocks(1);
    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 111);
    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return c1.m_chain.Height()), 110);

    auto active_tip2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    BOOST_CHECK_EQUAL(active_tip, active_tip2->pprev);
    BOOST_CHECK_EQUAL(active_tip, c1.m_chain.Tip());
    BOOST_CHECK_EQUAL(active_tip2, c2.m_chain.Tip());

    // Let scheduler events finish running to avoid accessing memory that is going to be unloaded
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

//! Test rebalancing the caches associated with each chainstate.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_rebalance_caches, TestChain100Setup)
{
    ChainstateManager& manager = *m_node.chainman;

    size_t max_cache = 10000;
    manager.m_total_coinsdb_cache = max_cache;
    manager.m_total_coinstip_cache = max_cache;

    std::vector<Chainstate*> chainstates;

    // Create a legacy (IBD) chainstate.
    //
    Chainstate& c1 = manager.ActiveChainstate();
    chainstates.push_back(&c1);
    {
        LOCK(::cs_main);
        c1.InitCoinsCache(8_MiB);
        manager.MaybeRebalanceCaches();
    }

    BOOST_CHECK_EQUAL(c1.m_coinstip_cache_size_bytes, max_cache);
    BOOST_CHECK_EQUAL(c1.m_coinsdb_cache_size_bytes, max_cache);

    // Create a snapshot-based chainstate.
    //
    CBlockIndex* snapshot_base{WITH_LOCK(manager.GetMutex(), return manager.ActiveChain()[manager.ActiveChain().Height() / 2])};
    Chainstate& c2{WITH_LOCK(::cs_main, return manager.AddChainstate(std::make_unique<Chainstate>(nullptr, manager.m_blockman, manager, *snapshot_base->phashBlock)))};
    chainstates.push_back(&c2);
    c2.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);

    // Reset IBD state so IsInitialBlockDownload() returns true and causes
    // MaybeRebalanceCaches() to prioritize the snapshot chainstate, giving it
    // more cache space than the snapshot chainstate. Calling ResetIbd() is
    // necessary because m_cached_is_ibd is already latched to false before
    // the test starts due to the test setup. After ResetIbd() is called,
    // IsInitialBlockDownload() will return true because at this point the active
    // chainstate has a null chain tip.
    static_cast<TestChainstateManager&>(manager).ResetIbd();

    {
        LOCK(::cs_main);
        c2.InitCoinsCache(8_MiB);
        manager.MaybeRebalanceCaches();
    }

    BOOST_CHECK_CLOSE(double(c1.m_coinstip_cache_size_bytes), max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(double(c1.m_coinsdb_cache_size_bytes), max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(double(c2.m_coinstip_cache_size_bytes), max_cache * 0.95, 1);
    BOOST_CHECK_CLOSE(double(c2.m_coinsdb_cache_size_bytes), max_cache * 0.95, 1);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_ibd_exit_after_loading_blocks, ChainTestingSetup)
{
    CBlockIndex tip;
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    const bool can_be_below_minimum_work{chainman.MinimumChainWork() > 0};
    auto apply{[&](bool cached_is_ibd, bool loading_blocks, bool tip_exists, bool enough_work, bool tip_recent) {
        LOCK(::cs_main);
        chainman.ResetChainstates();
        chainman.InitializeChainstate(m_node.mempool.get());

        const auto recent_time{Now<NodeSeconds>() - chainman.m_options.max_tip_age};

        chainman.m_cached_is_ibd.store(cached_is_ibd, std::memory_order_relaxed);
        chainman.m_blockman.m_importing = loading_blocks;
        if (tip_exists) {
            tip.nChainWork = chainman.MinimumChainWork() - ((!enough_work && can_be_below_minimum_work) ? 1 : 0);
            tip.nTime = (recent_time - (tip_recent ? 0h : 100h)).time_since_epoch().count();
            chainman.ActiveChain().SetTip(tip);
        } else {
            assert(!chainman.ActiveChain().Tip());
        }
        chainman.UpdateIBDStatus();
    }};

    for (const bool cached_is_ibd : {false, true}) {
        for (const bool loading_blocks : {false, true}) {
            for (const bool tip_exists : {false, true}) {
                for (const bool enough_work : {false, true}) {
                    for (const bool tip_recent : {false, true}) {
                        apply(cached_is_ibd, loading_blocks, tip_exists, enough_work, tip_recent);
                        const bool insufficient_work{!enough_work && can_be_below_minimum_work};
                        const bool expected_ibd = cached_is_ibd && (loading_blocks || !tip_exists || insufficient_work || !tip_recent);
                        BOOST_CHECK_EQUAL(chainman.IsInitialBlockDownload(), expected_ibd);
                    }
                }
            }
        }
    }
}

struct SnapshotTestSetup : TestChain100Setup {
    // Run with coinsdb on the filesystem to support, e.g., moving invalidated
    // chainstate dirs to "*_invalid".
    //
    // Note that this means the tests run considerably slower than in-memory DB
    // tests, but we can't otherwise test this functionality since it relies on
    // destructive filesystem operations.
    explicit SnapshotTestSetup(std::optional<std::vector<AssumeutxoData>> assumeutxo_data = std::nullopt) : TestChain100Setup{
                              {},
                              {
                                  .regtest_assumeutxo_data = std::move(assumeutxo_data),
                                  .coins_db_in_memory = false,
                                  .block_tree_db_in_memory = false,
                              },
                          }
    {
    }

    std::tuple<Chainstate*, Chainstate*> SetupSnapshot()
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);

        {
            LOCK(::cs_main);
            BOOST_CHECK(!chainman.CurrentChainstate().m_from_snapshot_blockhash);
            BOOST_CHECK(!node::FindAssumeutxoChainstateDir(chainman.m_options.datadir));
        }

        size_t initial_size;
        size_t initial_total_coins{100};

        // Make some initial assertions about the contents of the chainstate.
        {
            LOCK(::cs_main);
            CCoinsViewCache& ibd_coinscache = chainman.ActiveChainstate().CoinsTip();
            initial_size = ibd_coinscache.GetCacheSize();
            size_t total_coins{0};

            for (CTransactionRef& txn : m_coinbase_txns) {
                COutPoint op{txn->GetHash(), 0};
                BOOST_CHECK(ibd_coinscache.HaveCoin(op));
                total_coins++;
            }

            BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
            BOOST_CHECK_EQUAL(initial_size, initial_total_coins);
        }

        Chainstate& validation_chainstate = chainman.ActiveChainstate();

        // Snapshot should refuse to load at this height.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash);

        // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
        // be found.
        constexpr int snapshot_height = 110;
        mineBlocks(10);
        initial_size += 10;
        initial_total_coins += 10;

        // Should not load malleated snapshots
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // A UTXO is missing but count is correct
                metadata.m_coins_count -= 1;

                Txid txid;
                auto_infile >> txid;
                // coins size
                (void)ReadCompactSize(auto_infile);
                // vout index
                (void)ReadCompactSize(auto_infile);
                Coin coin;
                auto_infile >> coin;
        }));

        BOOST_CHECK(!node::FindAssumeutxoChainstateDir(chainman.m_options.datadir));

        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is larger than coins in file
                metadata.m_coins_count += 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is smaller than coins in file
                metadata.m_coins_count -= 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ZERO;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ONE;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile&, SnapshotMetadata& metadata) {
                // The Recycle Pool balance is consensus state and may not be
                // supplied by an untrusted snapshot independently of the
                // hardcoded assumeutxo commitment.
                metadata.m_recycle_pool_balance = COIN;
        }));

        BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(fs::exists(*node::FindAssumeutxoChainstateDir(chainman.m_options.datadir)));

        // Ensure our active chain is the snapshot chainstate.
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash->IsNull());

        Chainstate& snapshot_chainstate = chainman.ActiveChainstate();

        {
            LOCK(::cs_main);

            fs::path found = *node::FindAssumeutxoChainstateDir(chainman.m_options.datadir);

            // Note: WriteSnapshotBaseBlockhash() is implicitly tested above.
            BOOST_CHECK_EQUAL(
                *node::ReadSnapshotBaseBlockhash(found),
                *Assert(chainman.CurrentChainstate().m_from_snapshot_blockhash));
        }

        const auto& au_data = ::Params().AssumeutxoForHeight(snapshot_height);
        const CBlockIndex* tip = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip());

        BOOST_CHECK_EQUAL(tip->m_chain_tx_count, au_data->m_chain_tx_count);

        // To be checked against later when we try loading a subsequent snapshot.
        uint256 loaded_snapshot_blockhash{*Assert(WITH_LOCK(chainman.GetMutex(), return chainman.CurrentChainstate().m_from_snapshot_blockhash))};

        // Make some assertions about the both chainstates. These checks ensure the
        // legacy chainstate hasn't changed and that the newly created chainstate
        // reflects the expected content.
        {
            LOCK(::cs_main);
            int chains_tested{0};

            for (const auto& chainstate : chainman.m_chainstates) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();

                BOOST_CHECK_EQUAL(coinscache.GetRecyclePoolBalance(), au_data->recycle_pool_balance);

                // Both caches will be empty initially.
                BOOST_CHECK_EQUAL((unsigned int)0, coinscache.GetCacheSize());

                size_t total_coins{0};

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    BOOST_CHECK(coinscache.HaveCoin(op));
                    const Coin& coin{coinscache.AccessCoin(op)};
                    const auto expiry_entries{coinscache.GetRecycleExpiryBucket(
                        static_cast<uint32_t>(coin.nHeight) + Consensus::UTXO_EXPIRY_AGE)};
                    BOOST_CHECK(expiry_entries.contains(op));
                    total_coins++;
                }

                BOOST_CHECK_EQUAL(initial_size , coinscache.GetCacheSize());
                BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
                chains_tested++;
            }

            BOOST_CHECK_EQUAL(chains_tested, 2);
        }

        // Mine some new blocks on top of the activated snapshot chainstate.
        constexpr size_t new_coins{100};
        mineBlocks(new_coins);  // Defined in TestChain100Setup.

        {
            LOCK(::cs_main);
            size_t coins_in_active{0};
            size_t coins_in_background{0};
            size_t coins_missing_from_background{0};

            for (const auto& chainstate : chainman.m_chainstates) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();
                bool is_background = chainstate.get() != &chainman.ActiveChainstate();

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    if (coinscache.HaveCoin(op)) {
                        (is_background ? coins_in_background : coins_in_active)++;
                    } else if (is_background) {
                        coins_missing_from_background++;
                    }
                }
            }

            BOOST_CHECK_EQUAL(coins_in_active, initial_total_coins + new_coins);
            BOOST_CHECK_EQUAL(coins_in_background, initial_total_coins);
            BOOST_CHECK_EQUAL(coins_missing_from_background, new_coins);
        }

        // Snapshot should refuse to load after one has already loaded.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));

        // Snapshot blockhash should be unchanged.
        BOOST_CHECK_EQUAL(
            *chainman.ActiveChainstate().m_from_snapshot_blockhash,
            loaded_snapshot_blockhash);
        return std::make_tuple(&validation_chainstate, &snapshot_chainstate);
    }

    // Simulate a restart of the node by flushing all state to disk, clearing the
    // existing ChainstateManager, and unloading the block index.
    //
    // @returns a reference to the "restarted" ChainstateManager
    ChainstateManager& SimulateNodeRestart()
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);

        BOOST_TEST_MESSAGE("Simulating node restart");
        {
            LOCK(chainman.GetMutex());
            for (const auto& cs : chainman.m_chainstates) {
                if (cs->CanFlushToDisk()) cs->ForceFlushStateToDisk();
            }
        }
        {
            // Process all callbacks referring to the old manager before wiping it.
            m_node.validation_signals->SyncWithValidationInterfaceQueue();
            LOCK(::cs_main);
            chainman.ResetChainstates();
            BOOST_CHECK_EQUAL(chainman.m_chainstates.size(), 0);
            m_node.notifications = std::make_unique<KernelNotifications>(Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings));
            const ChainstateManager::Options chainman_opts{
                .chainparams = ::Params(),
                .datadir = chainman.m_options.datadir,
                .notifications = *m_node.notifications,
                .signals = m_node.validation_signals.get(),
            };
            const BlockManager::Options blockman_opts{
                .chainparams = chainman_opts.chainparams,
                .blocks_dir = m_args.GetBlocksDirPath(),
                .notifications = chainman_opts.notifications,
                .block_tree_db_params = DBParams{
                    .path = chainman.m_options.datadir / "blocks" / "index",
                    .cache_bytes = m_kernel_cache_sizes.block_tree_db,
                    .memory_only = m_block_tree_db_in_memory,
                },
            };
            // For robustness, ensure the old manager is destroyed before creating a
            // new one.
            m_node.chainman.reset();
            m_node.chainman = std::make_unique<ChainstateManager>(*Assert(m_node.shutdown_signal), chainman_opts, blockman_opts);
        }
        return *Assert(m_node.chainman);
    }
};

struct NonzeroRecycleSnapshotSetup : SnapshotTestSetup {
    static std::vector<AssumeutxoData> AssumeutxoDataWithPool()
    {
        return {{
            .height = 110,
            .hash_serialized = AssumeutxoHash{uint256{"86e9a1205b418b16dde3a18a78c730e30137e28466bda5dbf6b33ab8fc05447c"}},
            .m_chain_tx_count = 111,
            .blockhash = uint256{"135eec25a6fb277884e5824e7aa7d052c4868161c99a5122170b5266f86c273d"},
            .recycle_pool_balance = 7 * COIN,
        }};
    }

    NonzeroRecycleSnapshotSetup() : SnapshotTestSetup{AssumeutxoDataWithPool()} {}
};

BOOST_FIXTURE_TEST_CASE(chainstatemanager_nonzero_recycle_snapshot, NonzeroRecycleSnapshotSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    mineBlocks(10);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveHeight()), 110);

    Chainstate& validated{chainman.ActiveChainstate()};
    {
        LOCK(cs_main);
        validated.CoinsTip().SetRecyclePoolBalance(7 * COIN);
        validated.ForceFlushStateToDisk();
    }

    const CBlockIndex* const base{WITH_LOCK(cs_main, return chainman.ActiveTip())};
    const size_t expected_coins{WITH_LOCK(cs_main, return validated.CoinsTip().GetCacheSize())};

    // A v2 file has no Pool field and therefore deserializes to zero. It must
    // be rejected against this nonzero hardcoded commitment.
    const fs::path v3_path{m_path_root / "nonzero-v3.dat"};
    {
        AutoFile output{fsbridge::fopen(v3_path, "wb")};
        CreateUTXOSnapshot(m_node, validated, std::move(output), v3_path, v3_path);
    }
    DataStream v3_bytes;
    {
        AutoFile input{fsbridge::fopen(v3_path, "rb")};
        std::byte byte;
        while (true) {
            try { input >> byte; v3_bytes << byte; } catch (const std::ios_base::failure&) { break; }
        }
    }
    constexpr size_t version_offset{5};
    constexpr size_t network_offset{7};
    constexpr size_t pool_offset{5 + 2 + 4 + 32 + 8};
    constexpr size_t coins_offset{pool_offset + 8};
    DataStream v2_bytes;
    v2_bytes.write({v3_bytes.data(), version_offset});
    v2_bytes << uint16_t{2};
    v2_bytes.write({v3_bytes.data() + network_offset, pool_offset - network_offset});
    v2_bytes.write({v3_bytes.data() + coins_offset, v3_bytes.size() - coins_offset});
    const fs::path v2_path{m_path_root / "nonzero-v2.dat"};
    {
        AutoFile output{fsbridge::fopen(v2_path, "wb")};
        output.write(v2_bytes);
        BOOST_REQUIRE_EQUAL(output.fclose(), 0);
    }
    AutoFile v2_file{fsbridge::fopen(v2_path, "rb")};
    node::SnapshotMetadata v2_metadata{Params().MessageStart()};
    v2_file >> v2_metadata;
    BOOST_CHECK_EQUAL(v2_metadata.m_recycle_pool_balance, 0);
    CBlockIndex* original_tip;
    {
        LOCK(cs_main);
        original_tip = validated.m_chain.Tip();
        validated.m_chain.SetTip(*Assert(original_tip->pprev));
    }
    BOOST_CHECK(!chainman.ActivateSnapshot(v2_file, v2_metadata, /*in_memory=*/true));
    {
        LOCK(cs_main);
        validated.m_chain.SetTip(*original_tip);
    }

    // Mutate only the v3 Pool field. All magic, network, base hash, coin
    // count, and serialized UTXO bytes remain unchanged.
    DataStream mutated_v3{std::span<const std::byte>{v3_bytes.data(), v3_bytes.size()}};
    std::array<uint8_t, 8> mutated_pool{};
    WriteLE64(mutated_pool.data(), 8 * COIN);
    std::memcpy(mutated_v3.data() + pool_offset, mutated_pool.data(), mutated_pool.size());
    const fs::path mutated_v3_path{m_path_root / "nonzero-v3-mutated-pool.dat"};
    {
        AutoFile output{fsbridge::fopen(mutated_v3_path, "wb")};
        output.write(mutated_v3);
        BOOST_REQUIRE_EQUAL(output.fclose(), 0);
    }
    AutoFile mutated_v3_file{fsbridge::fopen(mutated_v3_path, "rb")};
    node::SnapshotMetadata mutated_metadata{Params().MessageStart()};
    mutated_v3_file >> mutated_metadata;
    BOOST_CHECK_EQUAL(mutated_metadata.m_base_blockhash, base->GetBlockHash());
    BOOST_CHECK_EQUAL(mutated_metadata.m_coins_count, v2_metadata.m_coins_count);
    BOOST_CHECK_EQUAL(mutated_metadata.m_recycle_pool_balance, 8 * COIN);
    {
        LOCK(cs_main);
        original_tip = validated.m_chain.Tip();
        validated.m_chain.SetTip(*Assert(original_tip->pprev));
    }
    BOOST_CHECK(!chainman.ActivateSnapshot(mutated_v3_file, mutated_metadata, /*in_memory=*/true));
    {
        LOCK(cs_main);
        validated.m_chain.SetTip(*original_tip);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
    Chainstate& snapshot{chainman.ActiveChainstate()};
    BOOST_REQUIRE(&snapshot != &validated);

    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(snapshot.m_chain.Height(), 110);
        BOOST_CHECK_EQUAL(snapshot.m_chain.Tip()->GetBlockHash(), base->GetBlockHash());
        BOOST_CHECK_EQUAL(snapshot.CoinsTip().GetBestBlock(), base->GetBlockHash());
        BOOST_CHECK_EQUAL(snapshot.CoinsTip().GetRecyclePoolBalance(), 7 * COIN);
        BOOST_CHECK_EQUAL(validated.CoinsTip().GetRecyclePoolBalance(), 7 * COIN);
        BOOST_CHECK_EQUAL(snapshot.CoinsTip().GetCacheSize(), expected_coins);
        for (const CTransactionRef& tx : m_coinbase_txns) {
            const COutPoint outpoint{tx->GetHash(), 0};
            BOOST_REQUIRE(snapshot.CoinsTip().HaveCoin(outpoint));
            BOOST_REQUIRE(validated.CoinsTip().HaveCoin(outpoint));
            const Coin& coin{snapshot.CoinsTip().AccessCoin(outpoint)};
            BOOST_CHECK(snapshot.CoinsTip().GetRecycleExpiryBucket(
                Consensus::UTXOExpiryHeight(coin.nHeight)).contains(outpoint));
        }
    }

    // First retain the validation-only comparison for its independent
    // consensus check.
    const CBlock next{CreateBlock({}, CScript{} << OP_TRUE)};
    CBlockIndex* next_index{nullptr};
    {
        LOCK(cs_main);
        BlockValidationState accept_state;
        BOOST_REQUIRE(chainman.AcceptBlock(
            std::make_shared<const CBlock>(next), accept_state, &next_index,
            /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        CCoinsViewCache validated_view{&validated.CoinsTip()};
        CCoinsViewCache snapshot_view{&snapshot.CoinsTip()};
        BlockValidationState validated_state;
        BlockValidationState snapshot_state;
        BOOST_REQUIRE(validated.ConnectBlock(next, validated_state, next_index, validated_view, /*fJustCheck=*/true));
        BOOST_REQUIRE(snapshot.ConnectBlock(next, snapshot_state, next_index, snapshot_view, /*fJustCheck=*/true));
        BOOST_CHECK_EQUAL(validated_view.GetBestBlock(), snapshot_view.GetBestBlock());
        BOOST_CHECK_EQUAL(validated_view.GetRecyclePoolBalance(), snapshot_view.GetRecyclePoolBalance());
        BOOST_CHECK_EQUAL(validated_view.GetCacheSize(), snapshot_view.GetCacheSize());
    }

    // Then connect the same block for real into independent cache views. Each
    // cache has a different chainstate-backed CoinsDB; neither result is
    // copied from the other.
    {
        LOCK(cs_main);
        CCoinsViewCache validated_connected{&validated.CoinsTip()};
        CCoinsViewCache snapshot_connected{&snapshot.CoinsTip()};
        BlockValidationState validated_state;
        BlockValidationState snapshot_state;
        BOOST_REQUIRE(validated.ConnectBlock(next, validated_state, next_index,
                                             validated_connected, /*fJustCheck=*/false));
        BOOST_REQUIRE(snapshot.ConnectBlock(next, snapshot_state, next_index,
                                            snapshot_connected, /*fJustCheck=*/false));
        validated_connected.Flush();
        snapshot_connected.Flush();
        validated.CoinsTip().Flush();
        snapshot.CoinsTip().Flush();
        BOOST_CHECK_EQUAL(validated_connected.GetBestBlock(), next.GetHash());
        BOOST_CHECK_EQUAL(snapshot_connected.GetBestBlock(), next.GetHash());
        BOOST_CHECK_EQUAL(validated_connected.GetRecyclePoolBalance(), 6 * COIN);
        BOOST_CHECK_EQUAL(snapshot_connected.GetRecyclePoolBalance(), 6 * COIN);
    }

    struct UTXOState {
        uint256 best_block;
        CAmount pool{0};
        std::map<COutPoint, Coin> coins;
        std::map<int, RecycleExpiryBucket> expiry;
    };
    const auto read_state = [](CCoinsViewDB& db) {
        UTXOState result;
        result.best_block = db.GetBestBlock();
        result.pool = db.GetRecyclePoolBalance();
        auto cursor{db.Cursor()};
        while (cursor->Valid()) {
            COutPoint outpoint;
            Coin coin;
            BOOST_REQUIRE(cursor->GetKey(outpoint));
            BOOST_REQUIRE(cursor->GetValue(coin));
            result.coins.emplace(outpoint, coin);
            result.expiry[Consensus::UTXOExpiryHeight(coin.nHeight)].emplace(outpoint, coin);
            cursor->Next();
        }
        return result;
    };
    {
        LOCK(cs_main);
        const UTXOState validated_state{read_state(validated.CoinsDB())};
        const UTXOState snapshot_state{read_state(snapshot.CoinsDB())};
        BOOST_CHECK_EQUAL(validated_state.best_block, snapshot_state.best_block);
        BOOST_CHECK_EQUAL(validated_state.pool, snapshot_state.pool);
        BOOST_CHECK_EQUAL(validated_state.coins.size(), snapshot_state.coins.size());
        BOOST_CHECK_EQUAL(validated_state.expiry.size(), snapshot_state.expiry.size());
        for (const auto& [outpoint, validated_coin] : validated_state.coins) {
            BOOST_REQUIRE(snapshot_state.coins.contains(outpoint));
            const Coin& snapshot_coin{snapshot_state.coins.at(outpoint)};
            BOOST_CHECK_EQUAL(validated_coin.out.nValue, snapshot_coin.out.nValue);
            BOOST_CHECK_EQUAL(validated_coin.nHeight, snapshot_coin.nHeight);
            BOOST_CHECK_EQUAL(validated_coin.fCoinBase, snapshot_coin.fCoinBase);
            BOOST_CHECK(validated_coin.out.scriptPubKey == snapshot_coin.out.scriptPubKey);
        }
        for (const auto& [height, validated_bucket] : validated_state.expiry) {
            BOOST_REQUIRE(snapshot_state.expiry.contains(height));
            const auto& snapshot_bucket{snapshot_state.expiry.at(height)};
            BOOST_CHECK_EQUAL(validated_bucket.size(), snapshot_bucket.size());
            for (const auto& [outpoint, validated_coin] : validated_bucket) {
                BOOST_REQUIRE(snapshot_bucket.contains(outpoint));
                const Coin& snapshot_coin{snapshot_bucket.at(outpoint)};
                BOOST_CHECK_EQUAL(validated_coin.out.nValue, snapshot_coin.out.nValue);
                BOOST_CHECK_EQUAL(validated_coin.nHeight, snapshot_coin.nHeight);
                BOOST_CHECK_EQUAL(validated_coin.fCoinBase, snapshot_coin.fCoinBase);
            }
        }
        const COutPoint new_coinbase{next.vtx.front()->GetHash(), 0};
        BOOST_REQUIRE(validated_state.coins.contains(new_coinbase));
        BOOST_CHECK(snapshot_state.coins.contains(new_coinbase));
        BOOST_CHECK_EQUAL(validated_state.coins.at(new_coinbase).out.nValue,
                          snapshot_state.coins.at(new_coinbase).out.nValue);
    }

}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_nonzero_recycle_rollback_snapshot, NonzeroRecycleSnapshotSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    mineBlocks(10);
    Chainstate& validated{chainman.ActiveChainstate()};
    {
        LOCK(cs_main);
        validated.CoinsTip().SetRecyclePoolBalance(7 * COIN);
        validated.ForceFlushStateToDisk();
    }
    CBlockIndex* const rollback_base{WITH_LOCK(cs_main, return validated.m_chain[110])};
    mineBlocks(2);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return validated.CoinsTip().GetRecyclePoolBalance()), 5 * COIN);

    const fs::path rollback_path{m_path_root / "loadable-nonzero-rollback.dat"};
    {
        AutoFile output{fsbridge::fopen(rollback_path, "wb")};
        const UniValue result{CreateRolledBackUTXOSnapshot(
            m_node, validated, rollback_base, std::move(output), rollback_path,
            rollback_path, /*in_memory=*/true)};
        BOOST_REQUIRE_EQUAL(result["base_height"].getInt<int>(), 110);
    }

    AutoFile rollback_file{fsbridge::fopen(rollback_path, "rb")};
    node::SnapshotMetadata metadata{Params().MessageStart()};
    rollback_file >> metadata;
    BOOST_REQUIRE_EQUAL(metadata.m_base_blockhash, rollback_base->GetBlockHash());
    BOOST_REQUIRE_EQUAL(metadata.m_recycle_pool_balance, 7 * COIN);

    // Preserve the headers/block index but reset the test's validated
    // chainstate to genesis, matching a node that loads a historical snapshot
    // before background validation has reached its base.
    {
        LOCK(cs_main);
        CBlockIndex* const original_tip{validated.m_chain.Tip()};
        const uint256 genesis_hash{validated.m_chain[0]->GetBlockHash()};
        chainman.ResetChainstates();
        chainman.InitializeChainstate(m_node.mempool.get());
        Chainstate& background{chainman.ActiveChainstate()};
        BOOST_REQUIRE(chainman.LoadGenesisBlock());
        background.InitCoinsDB(1_MiB, /*in_memory=*/true, /*should_wipe=*/false);
        background.InitCoinsCache(1_MiB);
        background.CoinsTip().SetBestBlock(genesis_hash);
        background.LoadChainTip();
        chainman.MaybeRebalanceCaches();
        for (CBlockIndex* index{original_tip}; index && index != background.m_chain.Tip(); index = index->pprev) {
            index->nStatus = BlockStatus::BLOCK_VALID_TREE;
            index->nTx = 0;
            index->m_chain_tx_count = 0;
            index->nSequenceId = 0;
        }
        background.PopulateBlockIndexCandidates();
    }
    BlockValidationState activate_state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(activate_state));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainman.ActiveHeight()), 0);

    const auto activated{chainman.ActivateSnapshot(rollback_file, metadata, /*in_memory=*/false)};
    BOOST_REQUIRE(activated);

    Chainstate* loaded_snapshot{nullptr};
    {
        LOCK(cs_main);
        for (const auto& candidate : chainman.m_chainstates) {
            if (candidate->SnapshotBase() == rollback_base) loaded_snapshot = candidate.get();
        }
        BOOST_REQUIRE(loaded_snapshot);
        BOOST_CHECK_EQUAL(loaded_snapshot->CoinsTip().GetRecyclePoolBalance(), 7 * COIN);
        BOOST_CHECK_EQUAL(loaded_snapshot->CoinsTip().GetBestBlock(), rollback_base->GetBlockHash());
        const COutPoint outpoint{m_coinbase_txns.front()->GetHash(), 0};
        const Coin& coin{loaded_snapshot->CoinsTip().AccessCoin(outpoint)};
        BOOST_REQUIRE(!coin.IsSpent());
        BOOST_CHECK(loaded_snapshot->CoinsTip().GetRecycleExpiryBucket(
            Consensus::UTXOExpiryHeight(coin.nHeight)).contains(outpoint));
    }

    // Close and reconstruct the snapshot's CoinsViews against the same
    // on-disk database, exercising the chainstate persistence path without
    // disturbing the independent background-validation chainstate.
    {
        LOCK(cs_main);
        loaded_snapshot->ResetCoinsViews();
    }
    loaded_snapshot->InitCoinsDB(8_MiB, /*in_memory=*/false, /*should_wipe=*/false);
    {
        LOCK(cs_main);
        loaded_snapshot->InitCoinsCache(8_MiB);
        BOOST_REQUIRE(loaded_snapshot->LoadChainTip());
        BOOST_CHECK_EQUAL(loaded_snapshot->CoinsTip().GetRecyclePoolBalance(), 7 * COIN);
        BOOST_CHECK_EQUAL(loaded_snapshot->CoinsTip().GetBestBlock(), rollback_base->GetBlockHash());
        const COutPoint outpoint{m_coinbase_txns.front()->GetHash(), 0};
        const Coin& coin{loaded_snapshot->CoinsTip().AccessCoin(outpoint)};
        BOOST_REQUIRE(!coin.IsSpent());
        BOOST_CHECK(loaded_snapshot->CoinsTip().GetRecycleExpiryBucket(
            Consensus::UTXOExpiryHeight(coin.nHeight)).contains(outpoint));
    }
}

//! Test basic snapshot activation.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_activate_snapshot, SnapshotTestSetup)
{
    this->SetupSnapshot();
}

//! Test LoadBlockIndex behavior when multiple chainstates are in use.
//!
//! - First, verify that setBlockIndexCandidates is as expected when using a single,
//!   fully-validating chainstate.
//!
//! - Then mark a region of the chain as missing data and introduce a second chainstate
//!   that will tolerate assumed-valid blocks. Run LoadBlockIndex() and ensure that the first
//!   chainstate only contains fully validated blocks and the other chainstate contains all blocks,
//!   except those marked assume-valid, because those entries don't HAVE_DATA.
//!
BOOST_FIXTURE_TEST_CASE(chainstatemanager_loadblockindex, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& cs1 = chainman.ActiveChainstate();

    int num_indexes{0};
    // Blocks in range [assumed_valid_start_idx, last_assumed_valid_idx) will be
    // marked as assumed-valid and not having data.
    const int expected_assumed_valid{20};
    const int last_assumed_valid_idx{111};
    const int assumed_valid_start_idx = last_assumed_valid_idx - expected_assumed_valid;

    // Mine to height 120, past the hardcoded regtest assumeutxo snapshot at
    // height 110
    mineBlocks(20);

    CBlockIndex* validated_tip{nullptr};
    CBlockIndex* assumed_base{nullptr};
    CBlockIndex* assumed_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_EQUAL(assumed_tip->nHeight, 120);

    auto reload_all_block_indexes = [&]() {
        LOCK(chainman.GetMutex());
        // For completeness, we also reset the block sequence counters to
        // ensure that no state which affects the ranking of tip-candidates is
        // retained (even though this isn't strictly necessary).
        chainman.ResetBlockSequenceCounters();
        for (const auto& cs : chainman.m_chainstates) {
            cs->ClearBlockIndexCandidates();
            BOOST_CHECK(cs->setBlockIndexCandidates.empty());
        }
        chainman.LoadBlockIndex();
        for (const auto& cs : chainman.m_chainstates) {
            cs->PopulateBlockIndexCandidates();
        }
    };

    // Ensure that without any assumed-valid BlockIndex entries, only the current tip is
    // considered as a candidate.
    reload_all_block_indexes();
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), 1);

    // Reset some region of the chain's nStatus, removing the HAVE_DATA flag.
    for (int i = 0; i <= cs1.m_chain.Height(); ++i) {
        LOCK(::cs_main);
        auto index = cs1.m_chain[i];

        // Blocks with heights in range [91, 110] are marked as missing data.
        if (i < last_assumed_valid_idx && i >= assumed_valid_start_idx) {
            index->nStatus = BlockStatus::BLOCK_VALID_TREE;
            index->nTx = 0;
            index->m_chain_tx_count = 0;
        }

        ++num_indexes;

        // Note the last fully-validated block as the expected validated tip.
        if (i == (assumed_valid_start_idx - 1)) {
            validated_tip = index;
        }
        // Note the last assumed valid block as the snapshot base
        if (i == last_assumed_valid_idx - 1) {
            assumed_base = index;
        }
    }

    // Note: cs2's tip is not set when ActivateExistingSnapshot is called.
    Chainstate& cs2{WITH_LOCK(::cs_main, return chainman.AddChainstate(std::make_unique<Chainstate>(nullptr, chainman.m_blockman, chainman, *assumed_base->phashBlock)))};

    // Set tip of the fully validated chain to be the validated tip
    cs1.m_chain.SetTip(*validated_tip);

    // Set tip of the assume-valid-based chain to the assume-valid block
    cs2.m_chain.SetTip(*assumed_base);

    // Sanity check test variables.
    BOOST_CHECK_EQUAL(num_indexes, 121); // 121 total blocks, including genesis
    BOOST_CHECK_EQUAL(assumed_tip->nHeight, 120);  // original chain has height 120
    BOOST_CHECK_EQUAL(validated_tip->nHeight, 90); // current cs1 chain has height 90
    BOOST_CHECK_EQUAL(assumed_base->nHeight, 110); // current cs2 chain has height 110

    // Regenerate cs1.setBlockIndexCandidates and cs2.setBlockIndexCandidate and
    // check contents below.
    reload_all_block_indexes();

    // The fully validated chain should only have the current validated tip
    // as a candidate (block 90). Specifically:
    //
    // - It does not have blocks 0-89 because they contain less work than the
    //   chain tip.
    //
    // - It has block 90 because it has data and equal work to the chain tip,
    //   (since it is the chain tip).
    //
    // - It does not have blocks 91-110 because they do not contain data.
    //
    // - It does not have any blocks after height 110 because cs1 is a background
    //   chainstate, and only blocks that are ancestors of the snapshot block
    //   are added as candidates for the background chainstate.
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), 1);
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.count(validated_tip), 1);

    // The assumed-valid tolerant chain has the assumed valid base as a
    // candidate, but otherwise has none of the assumed-valid (which do not
    // HAVE_DATA) blocks as candidates.
    //
    // Specifically:
    // - All blocks below height 110 are not candidates, because cs2 chain tip
    //   has height 110 and they have less work than it does.
    //
    // - Block 110 is a candidate even though it does not have data, because it
    //   is the snapshot block, which is assumed valid.
    //
    // - Blocks 111-120 are added because they have data.

    // Check that block 90 is absent
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(validated_tip), 0);
    // Check that block 109 is absent
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(assumed_base->pprev), 0);
    // Check that block 110 is present
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(assumed_base), 1);
    // Check that block 120 is present
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(assumed_tip), 1);
    // Check that 11 blocks total are present.
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.size(), num_indexes - last_assumed_valid_idx + 1);
}

BOOST_FIXTURE_TEST_CASE(loadblockindex_invalid_descendants, TestChain100Setup)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    // consider the chain of blocks grand_parent <- parent <- child
    // intentionally mark:
    //   - grand_parent: BLOCK_FAILED_VALID
    //   - parent: BLOCK_FAILED_CHILD
    //   - child: not invalid
    // Test that when the block index is loaded, all blocks are marked as BLOCK_FAILED_VALID
    auto* child{m_node.chainman->ActiveChain().Tip()};
    auto* parent{child->pprev};
    auto* grand_parent{parent->pprev};
    grand_parent->nStatus = (grand_parent->nStatus | BLOCK_FAILED_VALID);
    parent->nStatus = (parent->nStatus & ~BLOCK_FAILED_VALID) | BLOCK_FAILED_CHILD;
    child->nStatus = (child->nStatus & ~BLOCK_FAILED_VALID);

    // Reload block index to recompute block status validity flags.
    m_node.chainman->LoadBlockIndex();

    // check grand_parent, parent, child is marked as BLOCK_FAILED_VALID after reloading the block index
    BOOST_CHECK(grand_parent->nStatus & BLOCK_FAILED_VALID);
    BOOST_CHECK(parent->nStatus & BLOCK_FAILED_VALID);
    BOOST_CHECK(child->nStatus & BLOCK_FAILED_VALID);
}

//! Verify that ReconsiderBlock clears failure flags for the target block, its ancestors, and descendants,
//! but not for sibling forks that diverge from a shared ancestor.
BOOST_FIXTURE_TEST_CASE(invalidate_block_and_reconsider_fork, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();

    // we have a chain of 100 blocks: genesis(0) <- ... <- block98 <- block99 <- block100
    CBlockIndex* block98;
    CBlockIndex* block99;
    CBlockIndex* block100;
    {
        LOCK(chainman.GetMutex());
        block98 = chainman.ActiveChain()[98];
        block99 = chainman.ActiveChain()[99];
        block100 = chainman.ActiveChain()[100];
    }

    // create the following block constellation:
    // genesis(0) <- ... <- block98 <- block99  <- block100
    //                              <- block99' <- block100'
    // by temporarily invalidating block99. the chain tip now falls to block98,
    // mine 2 new blocks on top of block 98 (block99' and block100') and then restore block99 and block 100.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, block99));
    BOOST_REQUIRE(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()) == block98);
    CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (int i = 0; i < 2; ++i) {
        CreateAndProcessBlock({}, coinbase_script);
    }
    const CBlockIndex* fork_block99;
    const CBlockIndex* fork_block100;
    {
        LOCK(chainman.GetMutex());
        fork_block99 = chainman.ActiveChain()[99];
        BOOST_REQUIRE(fork_block99->pprev == block98);
        fork_block100 = chainman.ActiveChain()[100];
        BOOST_REQUIRE(fork_block100->pprev == fork_block99);
    }
    // Restore original block99 and block100
    {
        LOCK(chainman.GetMutex());
        chainstate.ResetBlockFailureFlags(block99);
        chainman.RecalculateBestHeader();
    }
    chainstate.ActivateBestChain(state);
    BOOST_REQUIRE(WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()) == block100);

    {
        LOCK(chainman.GetMutex());
        BOOST_CHECK(!(block100->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(block99->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(fork_block100->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(fork_block99->nStatus & BLOCK_FAILED_VALID));
    }

    // Invalidate block98
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, block98));

    {
        LOCK(chainman.GetMutex());
        // block98 and all descendants of block98 are marked BLOCK_FAILED_VALID
        BOOST_CHECK(block98->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(block99->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(block100->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(fork_block99->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(fork_block100->nStatus & BLOCK_FAILED_VALID);
    }

    // Reconsider block99. ResetBlockFailureFlags clears BLOCK_FAILED_VALID from
    // block99 and its ancestors (block98) and descendants (block100)
    // but NOT from block99' and block100' (not a direct ancestor/descendant)
    {
        LOCK(chainman.GetMutex());
        chainstate.ResetBlockFailureFlags(block99);
        chainman.RecalculateBestHeader();
    }
    chainstate.ActivateBestChain(state);
    {
        LOCK(chainman.GetMutex());
        BOOST_CHECK(!(block98->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(block99->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(block100->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(fork_block99->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(fork_block100->nStatus & BLOCK_FAILED_VALID);
    }
}

//! Ensure that snapshot chainstate can be loaded when found on disk after a
//! restart, and that new blocks can be connected to both chainstates.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init, SnapshotTestSetup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& bg_chainstate = chainman.ActiveChainstate();

    this->SetupSnapshot();

    fs::path snapshot_chainstate_dir = *node::FindAssumeutxoChainstateDir(chainman.m_options.datadir);
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK_EQUAL(snapshot_chainstate_dir, gArgs.GetDataDirNet() / "chainstate_snapshot");

    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));
    const uint256 snapshot_tip_hash = WITH_LOCK(chainman.GetMutex(),
        return chainman.ActiveTip()->GetBlockHash());

    BOOST_CHECK_EQUAL(WITH_LOCK(chainman.GetMutex(), return chainman.m_chainstates.size()), 2);

    // "Rewind" the background chainstate so that its tip is not at the
    // base block of the snapshot - this is so after simulating a node restart,
    // it will initialize instead of attempting to complete validation.
    //
    // Note that this is not a realistic use of DisconnectTip().
    DisconnectedBlockTransactions unused_pool{MAX_DISCONNECTED_TX_POOL_BYTES};
    BlockValidationState unused_state;
    {
        LOCK2(::cs_main, bg_chainstate.MempoolMutex());
        BOOST_CHECK(bg_chainstate.DisconnectTip(unused_state, &unused_pool));
        unused_pool.clear();  // to avoid queuedTx assertion errors on teardown
    }
    BOOST_CHECK_EQUAL(bg_chainstate.m_chain.Height(), 109);

    // Test that simulating a shutdown (resetting ChainstateManager) and then performing
    // chainstate reinitializing successfully reloads both chainstates.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates.
    this->LoadVerifyActivateChainstate();

    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates.size(), 2);
        // Background chainstate has height of 109 not 110 here due to a quirk
        // of the LoadVerifyActivate only calling ActivateBestChain on one
        // chainstate. The height would be 110 after a real restart, but it's
        // fine for this test which is focused on the snapshot chainstate.
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates[0]->m_chain.Height(), 109);
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates[1]->m_chain.Height(), 210);

        BOOST_CHECK(chainman_restarted.CurrentChainstate().m_from_snapshot_blockhash);
        BOOST_CHECK(chainman_restarted.CurrentChainstate().m_assumeutxo == Assumeutxo::UNVALIDATED);

        BOOST_CHECK_EQUAL(chainman_restarted.ActiveTip()->GetBlockHash(), snapshot_tip_hash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
        BOOST_CHECK_EQUAL(chainman_restarted.HistoricalChainstate()->m_chain.Height(), 109);
    }

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the initialized snapshot chainstate");
    mineBlocks(10);
    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);

        // Background chainstate should be unaware of new blocks on the snapshot
        // chainstate, but the block disconnected above is now reattached.
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates.size(), 2);
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates[0]->m_chain.Height(), 110);
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates[1]->m_chain.Height(), 220);
        BOOST_CHECK_EQUAL(chainman_restarted.HistoricalChainstate(), nullptr);
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion, SnapshotTestSetup)
{
    this->SetupSnapshot();

    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& active_cs = chainman.ActiveChainstate();
    Chainstate& validated_cs{*Assert(WITH_LOCK(cs_main, return chainman.HistoricalChainstate()))};
    auto tip_cache_before_complete = active_cs.m_coinstip_cache_size_bytes;
    auto db_cache_before_complete = active_cs.m_coinsdb_cache_size_bytes;

    SnapshotCompletionResult res;
    m_node.notifications->m_shutdown_on_fatal_error = false;

    fs::path snapshot_chainstate_dir = *node::FindAssumeutxoChainstateDir(chainman.m_options.datadir);
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK_EQUAL(snapshot_chainstate_dir, gArgs.GetDataDirNet() / "chainstate_snapshot");

    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));
    const uint256 snapshot_tip_hash = WITH_LOCK(chainman.GetMutex(),
        return chainman.ActiveTip()->GetBlockHash());

    res = WITH_LOCK(::cs_main, return chainman.MaybeValidateSnapshot(validated_cs, active_cs));
    BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::SUCCESS);

    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_assumeutxo == Assumeutxo::VALIDATED));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));
    BOOST_CHECK_EQUAL(WITH_LOCK(chainman.GetMutex(), return chainman.HistoricalChainstate()), nullptr);

    // Cache should have been rebalanced and reallocated to the "only" remaining
    // chainstate.
    BOOST_CHECK(active_cs.m_coinstip_cache_size_bytes > tip_cache_before_complete);
    BOOST_CHECK(active_cs.m_coinsdb_cache_size_bytes > db_cache_before_complete);

    // Trying completion again should return false.
    res = WITH_LOCK(::cs_main, return chainman.MaybeValidateSnapshot(validated_cs, active_cs));
    BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::SKIPPED);

    // The invalid snapshot path should not have been used.
    fs::path snapshot_invalid_dir = gArgs.GetDataDirNet() / "chainstate_snapshot_INVALID";
    BOOST_CHECK(!fs::exists(snapshot_invalid_dir));
    // chainstate_snapshot should still exist.
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));

    // Test that simulating a shutdown (resetting ChainstateManager) and then performing
    // chainstate reinitializing successfully cleans up the background-validation
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates, and should clean up the now unnecessary
    // background-validation leveldb contents.
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(!fs::exists(snapshot_invalid_dir));
    // chainstate_snapshot should now *not* exist.
    BOOST_CHECK(!fs::exists(snapshot_chainstate_dir));

    const Chainstate& active_cs2 = chainman_restarted.ActiveChainstate();

    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates.size(), 1);
        BOOST_CHECK(!chainman_restarted.CurrentChainstate().m_from_snapshot_blockhash);
        BOOST_CHECK(active_cs2.m_coinstip_cache_size_bytes > tip_cache_before_complete);
        BOOST_CHECK(active_cs2.m_coinsdb_cache_size_bytes > db_cache_before_complete);

        BOOST_CHECK_EQUAL(chainman_restarted.ActiveTip()->GetBlockHash(), snapshot_tip_hash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the \"new\" IBD chainstate");
    mineBlocks(10);
    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion_hash_mismatch, SnapshotTestSetup)
{
    auto chainstates = this->SetupSnapshot();
    Chainstate& validation_chainstate = *std::get<0>(chainstates);
    Chainstate& unvalidated_cs = *std::get<1>(chainstates);
    ChainstateManager& chainman = *Assert(m_node.chainman);
    SnapshotCompletionResult res;
    m_node.notifications->m_shutdown_on_fatal_error = false;

    // Test tampering with the IBD UTXO set with an extra coin to ensure it causes
    // snapshot completion to fail.
    CCoinsViewCache& ibd_coins = WITH_LOCK(::cs_main,
        return validation_chainstate.CoinsTip());
    Coin badcoin;
    badcoin.out.nValue = m_rng.rand32();
    badcoin.nHeight = 1;
    badcoin.out.scriptPubKey.assign(m_rng.randbits(6), 0);
    Txid txid = Txid::FromUint256(m_rng.rand256());
    ibd_coins.AddCoin(COutPoint(txid, 0), std::move(badcoin), false);

    fs::path snapshot_chainstate_dir = gArgs.GetDataDirNet() / "chainstate_snapshot";
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));

    {
        ASSERT_DEBUG_LOG("failed to validate the -assumeutxo snapshot state");
        res = WITH_LOCK(::cs_main, return chainman.MaybeValidateSnapshot(validation_chainstate, unvalidated_cs));
        BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::HASH_MISMATCH);
    }

    {
        LOCK(chainman.GetMutex());
        BOOST_CHECK_EQUAL(chainman.m_chainstates.size(), 2);
        BOOST_CHECK(chainman.m_chainstates[0]->m_assumeutxo == Assumeutxo::VALIDATED);
        BOOST_CHECK(!chainman.m_chainstates[0]->SnapshotBase());
        BOOST_CHECK(chainman.m_chainstates[1]->m_assumeutxo == Assumeutxo::INVALID);
        BOOST_CHECK(chainman.m_chainstates[1]->SnapshotBase());
    }

    fs::path snapshot_invalid_dir = gArgs.GetDataDirNet() / "chainstate_snapshot_INVALID";
    BOOST_CHECK(fs::exists(snapshot_invalid_dir));

    // Test that simulating a shutdown (resetting ChainstateManager) and then performing
    // chainstate reinitializing successfully loads only the fully-validated
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates, and should clean up the now unnecessary
    // background-validation leveldb contents.
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(fs::exists(snapshot_invalid_dir));
    BOOST_CHECK(!fs::exists(snapshot_chainstate_dir));

    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman_restarted.m_chainstates.size(), 1);
        BOOST_CHECK(!chainman_restarted.CurrentChainstate().m_from_snapshot_blockhash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the \"new\" IBD chainstate");
    mineBlocks(10);
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);
    }
}

/** Helper function to parse args into args_man and return the result of applying them to opts */
template <typename Options>
util::Result<Options> SetOptsFromArgs(ArgsManager& args_man, Options opts,
                                      const std::vector<const char*>& args)
{
    const auto argv{Cat({"ignore"}, args)};
    std::string error{};
    if (!args_man.ParseParameters(argv.size(), argv.data(), error)) {
        return util::Error{Untranslated("ParseParameters failed with error: " + error)};
    }
    const auto result{node::ApplyArgsManOptions(args_man, opts)};
    if (!result) return util::Error{util::ErrorString(result)};
    return opts;
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_args, BasicTestingSetup)
{
    //! Try to apply the provided args to a ChainstateManager::Options
    auto get_opts = [&](const std::vector<const char*>& args) {
        static kernel::Notifications notifications{};
        static const ChainstateManager::Options options{
            .chainparams = ::Params(),
            .datadir = {},
            .notifications = notifications};
        return SetOptsFromArgs(*this->m_node.args, options, args);
    };
    //! Like get_opts, but requires the provided args to be valid and unwraps the result
    auto get_valid_opts = [&](const std::vector<const char*>& args) {
        const auto result{get_opts(args)};
        BOOST_REQUIRE_MESSAGE(result, util::ErrorString(result).original);
        return *result;
    };

    // test -assumevalid
    BOOST_CHECK(!get_valid_opts({}).assumed_valid_block);
    BOOST_CHECK_EQUAL(get_valid_opts({"-assumevalid="}).assumed_valid_block, uint256::ZERO);
    BOOST_CHECK_EQUAL(get_valid_opts({"-assumevalid=0"}).assumed_valid_block, uint256::ZERO);
    BOOST_CHECK_EQUAL(get_valid_opts({"-noassumevalid"}).assumed_valid_block, uint256::ZERO);
    BOOST_CHECK_EQUAL(get_valid_opts({"-assumevalid=0x12"}).assumed_valid_block, uint256{0x12});

    std::string assume_valid{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    BOOST_CHECK_EQUAL(get_valid_opts({("-assumevalid=" + assume_valid).c_str()}).assumed_valid_block, uint256::FromHex(assume_valid));

    BOOST_CHECK(!get_opts({"-assumevalid=xyz"}));                                                               // invalid hex characters
    BOOST_CHECK(!get_opts({"-assumevalid=01234567890123456789012345678901234567890123456789012345678901234"})); // > 64 hex chars

    // test -minimumchainwork
    BOOST_CHECK(!get_valid_opts({}).minimum_chain_work);
    BOOST_CHECK_EQUAL(get_valid_opts({"-minimumchainwork=0"}).minimum_chain_work, arith_uint256());
    BOOST_CHECK_EQUAL(get_valid_opts({"-nominimumchainwork"}).minimum_chain_work, arith_uint256());
    BOOST_CHECK_EQUAL(get_valid_opts({"-minimumchainwork=0x1234"}).minimum_chain_work, arith_uint256{0x1234});

    std::string minimum_chainwork{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    BOOST_CHECK_EQUAL(get_valid_opts({("-minimumchainwork=" + minimum_chainwork).c_str()}).minimum_chain_work, UintToArith256(uint256::FromHex(minimum_chainwork).value()));

    BOOST_CHECK(!get_opts({"-minimumchainwork=xyz"}));                                                               // invalid hex characters
    BOOST_CHECK(!get_opts({"-minimumchainwork=01234567890123456789012345678901234567890123456789012345678901234"})); // > 64 hex chars

    BOOST_CHECK_EQUAL(get_valid_opts({}).prevoutfetch_threads_num, DEFAULT_PREVOUTFETCH_THREADS);
    BOOST_CHECK_EQUAL(get_valid_opts({"-prevoutfetchthreads=0"}).prevoutfetch_threads_num, 0);
    BOOST_CHECK_EQUAL(get_valid_opts({"-prevoutfetchthreads=3"}).prevoutfetch_threads_num, 3);
    BOOST_CHECK_EQUAL(get_valid_opts({"-prevoutfetchthreads=100"}).prevoutfetch_threads_num, MAX_PREVOUTFETCH_THREADS);
    BOOST_CHECK(!get_opts({"-prevoutfetchthreads=-1"}));
}

BOOST_AUTO_TEST_SUITE_END()
