# ReSatoshi development status

Date: 2026-08-03

## Completed in this checkpoint

- Correct zero-value genesis transaction serialization, independently mined
  genesis hashes, and source-to-generator cross-check.
- Consensus UTXO expiry boundary at 5,256,000 blocks.
- Expiry-height chainstate buckets, persistent Recycle Pool balance, atomic DB
  writes, full block undo, and disconnect restoration.
- Miner construction and consensus validation of Recycle Pool payouts.
- 50 RST payout cap halving every 1,314,000 blocks.
- Per-block integer ASERT using a two-day half-life and the genesis anchor.
- ReSatoshi mainnet message bytes, port, Bech32 HRP, Base58 namespaces, and
  removal of inherited Bitcoin mainnet seeds/assumptions.
- Wallet balance, address-balance, and coin-selection paths exclude expired
  outputs while retaining the historical wallet transaction.

## Verification performed

- Full node and unit-test executable compile: passed.
- Genesis generator cross-check: passed.
- 35 focused ASERT, Recycle Pool, UTXO cache, and DB tests: passed.
- Live regtest block connect/invalidate/reconsider: `3 -> 2 -> 3`, passed.
- Wallet-enabled `test_bitcoin` compile and link (503 build steps): passed.
- Wallet expiry visibility boundary test and the 35 focused consensus tests:
  passed after wallet integration.

## Known test debt

- The inherited `miner_tests/CreateNewBlock_validity` fixture uses Bitcoin's
  hard-coded timestamp/relative-lock schedule. ASERT makes difficulty depend on
  every candidate timestamp, so this fixture still needs an ASERT-native block
  sequence. The failure is currently `bad-txns-nonfinal` in the fixture's
  relative-lock scenario, not a live-node connect/reorg failure.
- The complete upstream functional suite, GUI build,
  fuzzing, sanitizer runs, multi-node synchronization, long reorg simulation,
  and expiry-height-scale test have not yet been completed.
- Public DNS seeds, signed reproducible releases, long-running public testnet,
  and independent security review require external infrastructure or reviewers.

## Readiness assessment

This checkpoint is a working experimental consensus prototype, not a safe
value-bearing mainnet release. It should proceed through an ASERT-native miner
fixture, wallet expiry behavior, multi-node functional tests, fuzz/sanitizer
runs, testnet soak, and independent review before any launch decision.
