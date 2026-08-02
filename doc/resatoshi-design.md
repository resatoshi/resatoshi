# ReSatoshi consensus design status

This document separates agreed project direction from consensus choices that
must not be silently fixed in code.

## Agreed direction

- Bitcoin Core is the starting codebase and its validation model should be
  preserved where ReSatoshi does not deliberately differ.
- The genesis timestamp message is `Flow like water`.
- Genesis issuance is zero. The genesis transaction output has value zero.
- Target block spacing is 10 minutes.
- A UTXO expires when its age reaches exactly 5,256,000 blocks. It is last
  spendable at age 5,255,999. The consensus constant is
  `Consensus::UTXO_EXPIRY_AGE`.
- Value from expired UTXOs enters a Recycle Pool and is redistributed through
  block rewards.
- Recycle Pool payout uses a fixed 1,314,000-block era (25 years at the target
  spacing). Its per-block cap starts at 50 RST and halves at every era boundary.
  The actual payout is the smaller of the cap and the available Pool balance.
  A block's newly expired value is added before its payout is calculated. An
  empty Pool pays zero; integer satoshi arithmetic leaves no fractional dust.
- Difficulty adjustment will use an ASERT-family per-block algorithm rather
  than Bitcoin's 2,016-block retarget.

## Finalized consensus choices

1. **Expiry/reorg processing:** expiry buckets are keyed by exact expiry height.
   Each unspent output is queued when created and removed from its bucket when
   spent. The due bucket is removed before payout. Undo records the full Coin
   and outpoint for every expired output, plus the prior Pool balance and the
   payout, so a reorg restores the exact prior state.
2. **Recycle accounting persistence:** expiry buckets and the aggregate Pool
   balance live in the chainstate database and are committed in the same
   flush transition as UTXOs and the best-block marker. They will not use a
   separate index database or a text file.
3. **Coinbase composition:** transaction fees remain separate from Recycle
   accounting. A valid block may claim at most the ordinary subsidy, fees, and
   computed Recycle payout. The scheduled Pool amount is debited whether or not
   the miner claims all of it; an underclaim is therefore a deterministic burn,
   preserving Bitcoin-compatible coinbase behavior without creating supply.
4. **ASERT:** mainnet uses integer ASERT from genesis, with 600-second spacing
   and a two-day (172,800-second) half-life. Genesis is the anchor and its
   virtual parent time is genesis time minus one target interval. Public testnet
   and signet use the same algorithm; regtest keeps no-retarget mode.
5. **Initial mining subsidy:** ordinary post-genesis mining starts at 50 RST and
   retains the inherited 210,000-block subsidy halving interval. The genesis
   output remains zero and unspendable.
6. **Mainnet identity:** message start is `a7 52 c9 5f`, default P2P port is
   39595, Bech32 HRP is `rs`, and legacy/extended-key versions are distinct
   from Bitcoin. Mainnet has no inherited seeds, assume-valid hash, chainwork,
   checkpoints, or assume-UTXO snapshots.

## Remaining release decisions

- Operator-controlled DNS seed domains and initial seed nodes cannot be filled
  in until infrastructure exists.
- Release signing keys, reproducible-build attestations, and public network
  activation date require project governance rather than a code default.

## Implementation status

- Project/client name changed to ReSatoshi.
- Custom message-start bytes, default port, Bech32 HRP, and genesis blocks are
  present in the uploaded source.
- Deterministic zero-value genesis mining and an independent cross-check test
  are present.
- Spending a UTXO at age 5,256,000 or later is rejected by consensus input
  validation. The boundary and overflow-safe, reversible Recycle Pool balance
  arithmetic have unit tests.
- Expiry buckets, Pool balance persistence, block connection/disconnection,
  full expiry undo, miner payout construction, and exact coinbase validation
  are connected to the chainstate path.
- Mainnet ASERT is enforced for every block and has schedule/half-life tests.
- Bitcoin mainnet seeds, chain statistics, assume-valid data, assume-UTXO data,
  address encodings, and buried activation heights have been removed or
  replaced for ReSatoshi mainnet.

This code is experimental and is not ready for a value-bearing mainnet.
