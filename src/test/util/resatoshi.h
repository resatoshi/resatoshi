// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_TEST_UTIL_RESATOSHI_H
#define BITCOIN_TEST_UTIL_RESATOSHI_H

#include <base58.h>
#include <bech32.h>
#include <chainparams.h>

#include <string>
#include <vector>

/**
 * Re-encode a valid Bitcoin mainnet address for the currently selected
 * network without changing its witness program or script payload.
 *
 * Upstream unit-test vectors deliberately contain Bitcoin mainnet addresses.
 * ReSatoshi keeps those payload vectors but uses isolated address namespaces.
 */
inline std::string ReencodeBitcoinMainnetAddressForTest(const std::string& address)
{
    const auto decoded_bech32{bech32::Decode(address)};
    if (decoded_bech32.encoding != bech32::Encoding::INVALID && decoded_bech32.hrp == "bc") {
        return bech32::Encode(decoded_bech32.encoding, Params().Bech32HRP(), decoded_bech32.data);
    }

    std::vector<unsigned char> decoded_base58;
    if (!DecodeBase58Check(address, decoded_base58, 21) || decoded_base58.size() != 21) {
        return address;
    }

    const std::vector<unsigned char>* replacement{nullptr};
    if (decoded_base58.front() == 0) {
        replacement = &Params().Base58Prefix(CChainParams::PUBKEY_ADDRESS);
    } else if (decoded_base58.front() == 5) {
        replacement = &Params().Base58Prefix(CChainParams::SCRIPT_ADDRESS);
    } else {
        return address;
    }

    std::vector<unsigned char> reencoded{replacement->begin(), replacement->end()};
    reencoded.insert(reencoded.end(), decoded_base58.begin() + 1, decoded_base58.end());
    return EncodeBase58Check(reencoded);
}

#endif // BITCOIN_TEST_UTIL_RESATOSHI_H
