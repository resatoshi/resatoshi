// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparamsbase.h>
#include <kernel/chainparams.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <array>

BOOST_AUTO_TEST_SUITE(resatoshi_identity_tests)

BOOST_AUTO_TEST_CASE(main_and_alpha_are_isolated)
{
    const auto main{CChainParams::Main()};
    const auto alpha{CChainParams::TestNet()};
    const auto main_base{CreateBaseChainParams(ChainType::MAIN)};
    const auto alpha_base{CreateBaseChainParams(ChainType::TESTNET)};

    const MessageStartChars expected_main{0xa7, 0x52, 0xc9, 0x5f};
    const MessageStartChars expected_alpha{0x18, 0x9a, 0x6c, 0x98};
    BOOST_CHECK_EQUAL_COLLECTIONS(main->MessageStart().begin(), main->MessageStart().end(), expected_main.begin(), expected_main.end());
    BOOST_CHECK_EQUAL_COLLECTIONS(alpha->MessageStart().begin(), alpha->MessageStart().end(), expected_alpha.begin(), expected_alpha.end());

    BOOST_CHECK_EQUAL(main->GetDefaultPort(), 39595);
    BOOST_CHECK_EQUAL(main_base->RPCPort(), 39594);
    BOOST_CHECK_EQUAL(main->Bech32HRP(), "rs");
    BOOST_CHECK(main->DNSSeeds().empty());
    BOOST_CHECK(main->FixedSeeds().empty());

    BOOST_CHECK_EQUAL(alpha->GetDefaultPort(), 49595);
    BOOST_CHECK_EQUAL(alpha_base->RPCPort(), 49594);
    BOOST_CHECK_EQUAL(alpha_base->DataDir(), "alpha");
    BOOST_CHECK_EQUAL(alpha->Bech32HRP(), "trs");
    BOOST_CHECK(alpha->DNSSeeds().empty());
    BOOST_CHECK(alpha->FixedSeeds().empty());
    BOOST_CHECK(alpha->GetAvailableSnapshotHeights().empty());

    BOOST_CHECK(main->GenesisBlock().GetHash() != alpha->GenesisBlock().GetHash());
    BOOST_CHECK(main->Base58Prefix(CChainParams::PUBKEY_ADDRESS) != alpha->Base58Prefix(CChainParams::PUBKEY_ADDRESS));
    BOOST_CHECK(main->Base58Prefix(CChainParams::SCRIPT_ADDRESS) != alpha->Base58Prefix(CChainParams::SCRIPT_ADDRESS));
    BOOST_CHECK(main->Base58Prefix(CChainParams::SECRET_KEY) != alpha->Base58Prefix(CChainParams::SECRET_KEY));
}

BOOST_AUTO_TEST_SUITE_END()
