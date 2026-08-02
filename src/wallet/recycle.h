// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_RECYCLE_H
#define BITCOIN_WALLET_RECYCLE_H

#include <consensus/recycle_pool.h>
#include <wallet/transaction.h>

namespace wallet {

/** Return true when every output of a confirmed wallet transaction has expired. */
inline bool IsWalletTxExpired(const CWalletTx& wtx, int chain_height)
{
    const auto* confirmed{wtx.state<TxStateConfirmed>()};
    return confirmed && confirmed->confirmed_block_height >= 0 &&
           Consensus::IsUTXOExpired(confirmed->confirmed_block_height, chain_height);
}

} // namespace wallet

#endif // BITCOIN_WALLET_RECYCLE_H
