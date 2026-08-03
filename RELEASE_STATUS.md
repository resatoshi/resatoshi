# ReSatoshi development status

Date: 2026-08-03

## Completed in this checkpoint

- Correct zero-value genesis transaction serialization, independently mined
  genesis hashes, and source-to-generator cross-check.
- Consensus UTXO expiry boundary at 5,256,000 blocks.
- Expiry-height chainstate buckets, persistent Recycle Pool balance, atomic DB
  writes, full block undo, and disconnect restoration.
- Miner construction and consensus validation of Recycle Pool payouts.
- Fixed Recycle Pool payout cap of 1 RST per block with no halving schedule.
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
- Live two-node regtest synchronization: both peers reached height 102 with
  identical best-block hashes.
- Live wallet transfer between nodes: 12.5 RST relayed, mined, and credited
  with one confirmation.
- Live disconnect/reconsider wallet test: receiver balance changed
  `12.5 -> 0 -> 12.5` and the original tip was restored.
- Full node restart persistence test: height 102, best-block hash, and the
  receiver's 12.5 RST balance were preserved.
- Standalone Recycle Pool property stress: 1,000,000 deterministic random
  valid transitions preserved exact accounting under both optimized and UBSan
  builds. Invalid, overflow-adjacent, era-boundary, and satoshi-truncation
  cases were also checked.
- Expiry/reorg state stress: 10,000 queued UTXOs, 5,000 pre-expiry spends,
  bulk expiry, payout, and exact Undo restoration passed under UBSan.
- Malformed Undo cases now reject out-of-range payout, invalid Coin value,
  overflowing creation height, duplicate entries, and inconsistent prior Pool
  balance without mutating live state.
- Recurring-expiry long-horizon simulation: one 600,000 RST expiry batch every
  ten years, from the 100-year boundary through year 600 (31,536,001 blocks).
  All 51 expiry events continued to add to the Pool while every funded block
  paid exactly 1 RST. Exact accounting, event-block Undo/reapply, optimized and
  UBSan runs, and the final representable block-height boundary all passed.
- Whole-supply 600-year simulation: ordinary block subsidy is the sole source
  of new RST, and every one of 50 recurring 600,000 RST expiry batches is
  funded by existing live UTXOs. After every one of 31,536,001 blocks, issued
  subsidy exactly equalled liquid UTXOs plus queued live UTXOs plus the Recycle
  Pool. The same identity passed expiry-block Undo/reapply and optimized and
  UBSan runs; the checker also rejected deliberate one-satoshi inflation and
  one-satoshi loss. At year 600 the exact identity was
  `20,999,949.97690000 = 16,754,350.97690000 + 0 + 4,245,599.00000000 RST`.

## Known test debt

- The inherited `miner_tests/CreateNewBlock_validity` fixture uses Bitcoin's
  hard-coded timestamp/relative-lock schedule. ASERT makes difficulty depend on
  every candidate timestamp, so this fixture still needs an ASERT-native block
  sequence. The failure is currently `bad-txns-nonfinal` in the fixture's
  relative-lock scenario, not a live-node connect/reorg failure.
- The complete upstream functional suite, GUI build, fuzzing, sanitizer runs,
  long reorg simulation, and expiry-height-scale test have not yet been
  completed. A basic two-node synchronization and transfer scenario has now
  passed, but broader network partition and competing-chain scenarios remain.
- AddressSanitizer/LeakSanitizer could not run in the current restricted
  process environment; focused UndefinedBehaviorSanitizer runs passed. This is
  not a substitute for the pending full sanitizer CI jobs.
- Public DNS seeds, signed reproducible releases, long-running public testnet,
  and independent security review require external infrastructure or reviewers.

## Readiness assessment

This checkpoint is a working experimental consensus prototype, not a safe
value-bearing mainnet release. It should proceed through an ASERT-native miner
fixture, wallet expiry behavior, multi-node functional tests, fuzz/sanitizer
runs, testnet soak, and independent review before any launch decision.
